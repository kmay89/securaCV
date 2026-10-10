// HubSetupRunner.swift
//
// The hub walkthrough's engine: find the hub, wait for Home Assistant to
// exist, finish its first-run setup with the owner's account, run the
// provisioning plan, and publish honest per-step state the screen renders
// live. The decisions live in HubOnboarding and HubProvisionPlan
// (host-tested); HubAPI does the calls; this class is the sequence and
// the narration.
//
// Two promises the sequence keeps. It never acts on a hub that is merely
// answering HTTP — only on one whose onboarding API answers (HubProbe
// .ready), which is the whole difference from the desktop's old watch. And
// nothing is done twice: every run re-reads the hub's state first, so a
// phone that was closed mid-run picks up where things actually stand.

import Combine
import Foundation

@MainActor
final class HubSetupRunner: ObservableObject {
    enum Phase: Equatable {
        case idle
        case watching(HubProbe)      // the first-boot watch, with the last probe
        case onboarding
        case provisioning
        case done
        case failed(String)
    }

    enum StepState: Equatable {
        case planned, running, done, skipped(String), failed(String), yours(String)
    }

    @Published private(set) var phase: Phase = .idle
    /// The hub being finished — discovered, or typed.
    @Published private(set) var hubURL: URL?
    @Published private(set) var hubName: String = ""
    /// Which discovered instance the person chose, when Bonjour hears more
    /// than one. With several hubs on the LAN nothing is dialed until one
    /// is chosen: the owner's typed password and the add-on installs must
    /// never land on the wrong Home Assistant by sort order.
    @Published var selectedHubID: String?
    @Published private(set) var log: [String] = []
    @Published private(set) var onboardOutcome: HubOnboardOutcome?
    @Published private(set) var steps: [HubProvisionStep] = []
    @Published private(set) var stepState: [String: StepState] = [:]
    /// Everything that is honestly the owner's to finish, gathered.
    @Published private(set) var leftovers: [String] = []
    /// How long the watch has been running — the screen shows the
    /// countdown to "maybe go find it" the same way the Flasher does.
    @Published private(set) var watchStartedAt: Date?

    let discovery = HubDiscovery()
    private var watchTask: Task<Void, Never>?
    private var bag = Set<AnyCancellable>()

    init() {
        // A nested ObservableObject does not re-render a view that observes
        // only this one: forward the browse's changes so the discovered-hub
        // rows appear the moment Bonjour hears one.
        discovery.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &bag)
    }

    /// Past the honest first-boot window with NOTHING heard, the screen
    /// swaps patience for the go-find-it checklist. A hub that answers
    /// (preparing) never escalates — it is found, just not finished.
    static let escalateAfter: TimeInterval = 25 * 60

    // MARK: - the watch

    /// Start looking. `typedHost` overrides discovery (an IP from the
    /// router when mDNS is blocked); otherwise the Bonjour browse and the
    /// default `homeassistant.local` are tried, in that order.
    func startWatching(typedHost: String? = nil) {
        stopWatching()
        watchStartedAt = Date()
        phase = .watching(.offline)
        discovery.start()
        watchTask = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                let candidate = self.candidateURL(typedHost: typedHost)
                if let url = candidate {
                    let probe = await HubAPI.probe(url)
                    guard !Task.isCancelled else { return }
                    self.hubURL = url
                    if case .watching(let was) = self.phase, was != probe {
                        self.note(Self.line(for: probe))
                    }
                    self.phase = .watching(probe)
                    if probe == .ready { return }   // the screen takes over
                }
                try? await Task.sleep(for: .seconds(5))
            }
        }
    }

    func stopWatching() {
        watchTask?.cancel()
        watchTask = nil
        discovery.stop()
    }

    /// Typed first, then the one discovered hub, then the default name —
    /// and every candidate has already passed the "only on this network"
    /// gate. Several discovered hubs yield NO candidate until the person
    /// picks one (`selectedHubID`); the screen asks.
    private func candidateURL(typedHost: String?) -> URL? {
        if let typed = typedHost, !typed.trimmingCharacters(in: .whitespaces).isEmpty {
            return HubOnboarding.baseURL(forHost: typed)
        }
        let dialable = discovery.found.filter { $0.baseURL != nil }
        if dialable.count > 1 {
            guard let chosen = dialable.first(where: { $0.id == selectedHubID }) else { return nil }
            hubName = chosen.name
            return chosen.baseURL
        }
        if let only = dialable.first {
            hubName = only.name
            return only.baseURL
        }
        return HubOnboarding.baseURL(forHost: HubOnboarding.defaultHost)
    }

    /// More than one Home Assistant answers on this network and none has
    /// been chosen — the screen shows the choice instead of a probe line.
    var needsHubChoice: Bool {
        discovery.found.filter { $0.baseURL != nil }.count > 1 && selectedHubID == nil
    }

    static func line(for probe: HubProbe) -> String {
        switch probe {
        case .offline: return "Nothing answering yet — still booting."
        case .preparing: return "Found it — it's on your Wi-Fi and installing Home Assistant, the long part of first boot. Nothing to do; this phone keeps watching."
        case .ready: return "Home Assistant is answering — ready to finish."
        }
    }

    var escalated: Bool {
        guard let start = watchStartedAt, case .watching(.offline) = phase else { return false }
        return Date().timeIntervalSince(start) > Self.escalateAfter
    }

    // MARK: - finish

    /// Finish the hub: the account, then the plan. Safe to call again
    /// after any stumble.
    func finish(login: HubOwnerLogin) async {
        guard let url = hubURL else {
            phase = .failed("No hub to finish yet — let the watch find it first.")
            return
        }
        stopWatching()
        let api = HubAPI(base: url)
        phase = .onboarding
        leftovers = []
        var session: HubSession?
        do {
            let result = try await api.completeOnboarding(login: login) { [weak self] line in
                Task { @MainActor in self?.note(line) }
            }
            onboardOutcome = result.outcome
            session = result.session
            if let note = result.outcome.note { leftovers.append(note) }
        } catch {
            phase = .failed("Couldn't finish Home Assistant's setup: \(error.localizedDescription) Nothing is lost — try again, or open the hub and its wizard walks you through the same steps.")
            return
        }
        guard let token = session else {
            phase = .failed(onboardOutcome?.note ?? "Couldn't sign in to the hub to continue.")
            return
        }
        HubStore.save(HubRecord(baseURL: url, name: hubName.isEmpty ? "Your hub" : hubName,
                                ownerUsername: login.normalized.username, finishedAt: nil))

        phase = .provisioning
        let finished = await provision(api: api, session: token)
        await api.revoke(token)
        // "Done" is a claim: it is made only when every planned action ran.
        // A plan that stopped keeps the record unfinished and the phase
        // failed, so the screen offers the retry instead of a finish line.
        HubStore.save(HubRecord(baseURL: url, name: hubName.isEmpty ? "Your hub" : hubName,
                                ownerUsername: login.normalized.username,
                                finishedAt: finished ? Date() : nil))
        if finished {
            phase = .done
        } else {
            phase = .failed("The hub's setup stopped partway — the step list says where. Nothing is half-done in a way a retry can't finish: every step is skipped once it is there.")
        }
    }

    private func note(_ line: String) {
        log.append(line)
        if log.count > 200 { log.removeFirst(log.count - 200) }
    }

    // MARK: - the plan

    /// Run the plan. True when every planned action ran (or was already
    /// there); false the moment one fails, with the rest left untouched.
    private func provision(api: HubAPI, session: HubSession) async -> Bool {
        note("Reading what the hub already has…")
        let observed = HubProvisionPlan.observe(
            repositories: await api.supervisorGET("store/repositories", session: session),
            addons: await api.supervisorGET("addons", session: session),
            mosquittoInfo: await api.supervisorGET(HubProvisionPlan.infoPath(slug: HubProvisionPlan.mosquittoSlug), session: session),
            kernelInfo: await api.supervisorGET(HubProvisionPlan.infoPath(slug: HubProvisionPlan.kernelSlug), session: session),
            configEntries: await api.coreGET("config/config_entries/entry", session: session))
        let plan = HubProvisionPlan.plan(observed: observed)
        steps = plan
        if HubProvisionPlan.todo(plan).isEmpty {
            note("Everything this phone sets up is already in place — nothing to redo.")
        }
        for step in plan {
            for a in step.actions {
                stepState[a.id] = a.already ? .skipped(a.reason) : .planned
            }
        }
        for step in plan {
            note(step.title)
            for planned in step.actions where !planned.already {
                stepState[planned.id] = .running
                let state = await perform(planned.action, api: api, session: session)
                stepState[planned.id] = state
                if case .failed(let why) = state {
                    note("Stopped at \"\(HubProvisionPlan.label(for: planned.action))\": \(why)")
                    leftovers.append("\(step.title) didn't finish: \(why) Running this again is safe — nothing is done twice.")
                    return false
                }
                if case .yours(let what) = state { leftovers.append(what) }
            }
            if let yours = step.yours { leftovers.append(yours) }
        }
        return true
    }

    /// Unreachable while HubProvisionPlanTests holds every Supervisor
    /// action to a path — said plainly rather than force-unwrapped.
    private static let noPath = "This build has no Supervisor path for that step."

    private func perform(_ action: HubProvisionAction, api: HubAPI, session: HubSession) async -> StepState {
        // Every Supervisor call takes its path from HubProvisionPlan's one
        // table (supervisorPath / infoPath / restartPath), so the paths the
        // tests pin are the paths this runner calls — they used to be typed
        // twice, and the tests checked a copy nothing used.
        let path = HubProvisionPlan.supervisorPath(for: action)
        do {
            switch action {
            case .registerRepository(let url):
                guard let path else { return .failed(Self.noPath) }
                try await api.supervisorPOST(path, body: ["repository": url], session: session)
                return .done
            case .installAddon:
                guard let path else { return .failed(Self.noPath) }
                try await api.supervisorPOST(path, session: session, timeout: 900)
                return .done
            case .startAddon:
                guard let path else { return .failed(Self.noPath) }
                try await api.supervisorPOST(path, session: session, timeout: 300)
                return .done
            case .setKernelMode(let mode):
                guard let path else { return .failed(Self.noPath) }
                var options = await api.supervisorGET(HubProvisionPlan.infoPath(slug: HubProvisionPlan.kernelSlug), session: session)
                    .flatMap(HubProvisionPlan.options(in:)) ?? [:]
                options["mode"] = mode
                try await api.supervisorPOST(path, body: ["options": options], session: session)
                return .done
            case .addBrokerLogin(let username):
                guard let path else { return .failed(Self.noPath) }
                // Keep a password this phone already minted (a re-run must
                // not lock out Canaries flashed with the first one); mint
                // one only when none exists.
                let password: String
                if let kept = HubSecretStore.brokerLogin(), kept.username == username {
                    password = kept.password
                } else {
                    guard let minted = HubProvisionPlan.mintPassword() else {
                        return .failed("This phone couldn't mint a password.")
                    }
                    password = minted
                }
                var options = await api.supervisorGET(HubProvisionPlan.infoPath(slug: HubProvisionPlan.mosquittoSlug), session: session)
                    .flatMap(HubProvisionPlan.options(in:)) ?? [:]
                let current = (options["logins"] as? [[String: Any]]) ?? []
                options["logins"] = HubProvisionPlan.loginsAppending(username: username, password: password, to: current)
                try await api.supervisorPOST(path, body: ["options": options], session: session)
                // The broker reads `logins` at start, so it must cycle.
                try await api.supervisorPOST(HubProvisionPlan.restartPath(slug: HubProvisionPlan.mosquittoSlug),
                                             session: session, timeout: 120)
                try HubSecretStore.set(.init(username: username, password: password))
                return .done
            case .connectMQTT:
                // The add-on announces itself; accept the card it raised.
                // Give the announcement a minute to arrive after the start.
                let deadline = Date().addingTimeInterval(60)
                while Date() < deadline {
                    if let flows = await api.coreGET("config/config_entries/flow", session: session),
                       let id = HubProvisionPlan.discoveredMQTTFlowID(in: flows) {
                        switch try await api.flow("/\(id)", body: [:], session: session) {
                        case .created, .alreadyConfigured: return .done
                        case .needsForm(let step): return .yours("Home Assistant wants a click on the MQTT card (\(step)) — Settings → Devices & Services.")
                        case .failed(let why): return .yours("Connecting MQTT needs a click in Home Assistant: \(why)")
                        }
                    }
                    try? await Task.sleep(for: .seconds(5))
                }
                // No card: start the flow by hand with the add-on's address.
                switch try await api.flow("", body: ["handler": "mqtt"], session: session) {
                case .created, .alreadyConfigured: return .done
                case .failed(let why): return .yours("Connecting MQTT needs a click in Home Assistant: \(why)")
                case .needsForm: break
                }
                return .yours("Home Assistant wants the broker typed once: Settings → Devices & Services → Add integration → MQTT, broker core-mosquitto, port 1883.")
            case .connectSecuraCV:
                switch try await api.flow("", body: ["handler": "securacv"], session: session) {
                case .created, .alreadyConfigured: return .done
                case .failed(let why): return .yours("The SecuraCV integration isn't connected yet: \(why)")
                case .needsForm: break
                }
                // The flow's one form is the setup mode; auto is the default.
                guard let flows = await api.coreGET("config/config_entries/flow", session: session),
                      let list = try? JSONSerialization.jsonObject(with: flows) as? [[String: Any]],
                      let id = list.first(where: { $0["handler"] as? String == "securacv" })?["flow_id"] as? String
                else { return .yours("Finish the SecuraCV integration in Home Assistant: Settings → Devices & Services → Add integration → SecuraCV.") }
                switch try await api.flow("/\(id)", body: ["setup_mode": "auto"], session: session) {
                case .created, .alreadyConfigured: return .done
                case .needsForm(let step): return .yours("The SecuraCV integration wants a click (\(step)) — Settings → Devices & Services.")
                case .failed(let why): return .yours("The SecuraCV integration isn't connected yet: \(why)")
                }
            }
        } catch {
            return .failed(error.localizedDescription)
        }
    }
}
