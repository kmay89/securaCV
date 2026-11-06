// SetupPortalClient.swift
//
// The hands for SetupPortal: join a Canary's setup network from inside the
// app, ask the Canary which networks it can see, hand its captive page the
// home Wi-Fi the person picked, and watch the device's own verdict. The one
// place the app uses NetworkExtension — NEHotspotConfiguration is the
// system's own "join this network for me" door (iOS asks once).
//
// What is checked, and how: the portal's `/status` is the device saying
// "connected" after it actually got an address on the home network — that
// is the verdict shown, never "the request was accepted". A wrong password
// comes back as the firmware's own reason.
//
// How the phone leaves the setup network, and why it is this way:
//   * The configuration is applied WITHOUT `joinOnce`. A joinOnce network is
//     tied to the app being in front, and the one thing a person does in the
//     middle of this step is leave — to the Passwords app or Settings for
//     the Wi-Fi key — and come back to a phone that had quietly dropped the
//     Canary's network, and a Join that failed for no reason it could name.
//   * Instead the client itself forgets the network — `finish()`, on the
//     verdict, on a failure the person walks away from, or when the screen
//     closes — which puts the phone back on the home Wi-Fi at once.
//   * And at launch `forgetLingeringSetupNetworks()` removes any SecuraCV-
//     configuration an earlier run left behind (the app killed mid-step), so
//     the phone can never drift back onto a Canary's setup network by itself
//     weeks later. Only the SecuraCV- shapes are touched; the app configures
//     no other network.
//   * `finish()` also ends every step still in flight. `connect` and `hand`
//     are unstructured work the screen cannot cancel, so each checks the
//     client's `generation` after every await and stops when `finish()` has
//     moved it — a walkthrough closed while the Canary was still listing
//     its networks must not come back to life as a watcher polling a
//     network the phone has left.
//   * The ready watch reads `/status` once a second and treats a portal
//     that stops answering as news, not as noise. Both firmware portals
//     take the setup network down 1.6 s after ANY client reads `success`
//     (onboarding_fsm.h `ap_hold_after_ack_ms`), so a join finished on the
//     Canary's own page can be acknowledged there, and the network gone,
//     between two of this client's polls. Three silent polls end the step
//     in words that say so (SetupPortal.portalGoneLine), never a spinner
//     over a network that no longer exists.

import Foundation
import NetworkExtension

@MainActor
final class SetupPortalClient: ObservableObject {
    enum Phase: Equatable {
        case idle
        case joining              // asking iOS to join the SecuraCV- setup network
        case reachingPortal       // on it; waiting for 192.168.4.1 to answer
        case listing              // asking the Canary which networks it can see
        case ready                // on its setup network, its list in hand: pick and Join
        case posting              // sending the home Wi-Fi
        case waiting(SetupPortal.JoinState)
        case done
        case failed(String)
    }

    @Published private(set) var phase: Phase = .idle
    /// What the Canary's own radio heard (`GET /scan`), strongest first —
    /// empty until listed, or when it heard nothing it could show.
    @Published private(set) var networks: [SetupPortal.Network] = []
    /// The setup network this client asked iOS to join, so `finish()` can
    /// forget exactly that one.
    private var joinedSSID: String?

    /// The phone is on the Canary's setup network and its page answered —
    /// so `hand(...)` (or a retry of it after a typo) needs no new prompt.
    @Published private(set) var onSetupNetwork = false

    /// The home network THIS phone posted to `/join` — nil when the Canary
    /// reached `.done` without it (the person finished on the Canary's own
    /// page in iOS's captive pop-up, or from another phone). The view
    /// remembers a network, and names it, only when it sent it.
    @Published private(set) var handedSSID: String?

    /// Watches `/status` while the list is on screen (`watchWhileReady`).
    private var readyWatch: Task<Void, Never>?

    /// Moved by `finish()`. `connect` and `hand` note it when they start
    /// and stop at the first await after it moved: the screen closed, or a
    /// new connect began, and nothing older may touch the phase again.
    private var generation = 0

    /// Polls of `/status` answered by nobody before the ready watch judges
    /// the setup network gone: more than one, so a busy ESP's dropped
    /// answer is not the end; few enough that a person is told within
    /// seconds, not left with a list over a network the phone has left.
    static let silentPollsBeforeGone = 3

    /// A session pinned to the Wi-Fi interface: on a captive network with
    /// no internet iOS would otherwise route around it over cellular, and
    /// 192.168.4.1 would answer from somewhere else or not at all.
    private let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.allowsCellularAccess = false
        config.waitsForConnectivity = false
        config.timeoutIntervalForRequest = 6
        config.timeoutIntervalForResource = 10
        return URLSession(configuration: config)
    }()

    /// Step one: join the Canary's setup network (iOS asks once), wait for
    /// its page, and ask it which networks it can see — so the person
    /// picks from the Canary's own list instead of typing a name it may
    /// not be able to hear. Ends in `.ready` (list in `networks`, possibly
    /// empty: the form then takes a typed name) or a named failure.
    func connect(setupSSID: String, setupKey: String) async {
        stopReadyWatch()
        generation += 1
        let gen = generation
        onSetupNetwork = false
        handedSSID = nil
        networks = []
        phase = .joining
        let config = NEHotspotConfiguration(ssid: setupSSID, passphrase: setupKey, isWEP: false)
        // Not joinOnce: see the header. The client forgets the network itself.
        config.joinOnce = false
        var outcome = SetupPortal.HotspotJoinOutcome.joined
        do {
            try await NEHotspotConfigurationManager.shared.apply(config)
        } catch {
            outcome = Self.joinOutcome(error)
        }
        guard gen == generation else {
            // The screen closed while iOS was joining: `finish()` ran before
            // the network was known to it, so let it go here.
            if outcome.isOn { NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: setupSSID) }
            return
        }
        guard outcome.isOn else {
            phase = .failed(outcome.advice(ssid: setupSSID) ?? "iOS couldn't join \(setupSSID).")
            return
        }
        joinedSSID = setupSSID

        phase = .reachingPortal
        let answered = await waitForPortal(seconds: 25, gen: gen)
        guard gen == generation else { return }
        guard answered else {
            // Leave the network now: a phone parked on a setup network with
            // no page to talk to has no internet and nothing to do there.
            finish()
            phase = .failed("Joined \(setupSSID), but the Canary's setup page didn't answer. Move closer, or power the Canary off and on and try again. If iOS asked about Local Network just now and the answer was no, SecuraCV can't reach the page — allow it in Settings.")
            return
        }
        onSetupNetwork = true

        phase = .listing
        let listed = await scan(seconds: 10, gen: gen)
        guard gen == generation else { return }
        networks = listed
        phase = .ready
        startReadyWatch()
    }

    /// Step two: hand over the home Wi-Fi and watch the device's own
    /// verdict. Safe to call again after a failure (a typo, a 5 GHz name):
    /// the phone is still on the setup network, so a retry costs no prompt.
    func hand(homeSSID: String, homePassword: String,
              timeZone: String? = TimeZone.current.identifier) async {
        stopReadyWatch()
        let gen = generation
        if let problem = SetupPortal.credentialProblem(ssid: homeSSID, password: homePassword) {
            phase = .failed(problem)
            return
        }
        phase = .posting
        var request = URLRequest(url: SetupPortal.baseURL.appendingPathComponent("join"))
        request.httpMethod = "POST"
        request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        request.httpBody = Data(SetupPortal.joinBody(ssid: homeSSID, password: homePassword, timeZone: timeZone).utf8)
        let posted: Result<Data, Error>
        do {
            let (data, _) = try await session.data(for: request)
            posted = .success(data)
        } catch {
            posted = .failure(error)
        }
        guard gen == generation else { return }
        switch posted {
        case .success(let data):
            let reply = SetupPortal.parseJoinReply(data)
            guard reply.ok else {
                phase = .failed(SetupPortal.advice(for: reply.reason ?? "The Canary refused the credentials."))
                return
            }
        case .failure(let error):
            phase = .failed("The Canary's setup page didn't take the credentials: \(error.localizedDescription)")
            return
        }
        handedSSID = homeSSID

        // The device's own verdict. It keeps its setup network up until the
        // phone has seen the answer, then takes it down — so a "success"
        // read here is the join, confirmed at the source.
        let deadline = Date().addingTimeInterval(90)
        while Date() < deadline {
            let read = await status()
            guard gen == generation else { return }
            if let state = read {
                phase = .waiting(state)
                switch state {
                case .success:
                    phase = .done
                    finish()
                    return
                case .fail(let reason):
                    phase = .failed(SetupPortal.advice(for: reason))
                    return
                case .idle, .connecting: break
                }
            }
            try? await Task.sleep(for: .seconds(2))
        }
        guard gen == generation else { return }
        phase = .failed("No verdict from the Canary in time. If it joined, it appears on the Fleet tab shortly; otherwise power it off and on and try again.")
    }

    /// Let the setup network go (done, abandoned, or the screen closed):
    /// the phone leaves it now, back to the home Wi-Fi, and forgets it.
    func finish() {
        generation += 1
        stopReadyWatch()
        if let ssid = joinedSSID {
            NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: ssid)
        }
        joinedSSID = nil
        onSetupNetwork = false
    }

    /// At launch: forget any SecuraCV- setup network an earlier run of the
    /// app configured and never let go of (killed mid-walkthrough), so the
    /// phone never joins a Canary's setup network on its own. Only the
    /// networks this app itself configured are listed by iOS, and only the
    /// setup-network shapes among them are touched.
    nonisolated static func forgetLingeringSetupNetworks() {
        NEHotspotConfigurationManager.shared.getConfiguredSSIDs { ssids in
            for ssid in ssids where SetupPortal.isSetupNetwork(ssid) {
                NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: ssid)
            }
        }
    }

    /// iOS's answer to the join, in the portal's plain vocabulary. Pure, so
    /// SetupPortalTests can hand it each error iOS defines.
    nonisolated static func joinOutcome(_ error: Error) -> SetupPortal.HotspotJoinOutcome {
        // NEHotspotConfigurationError is a plain NS_ENUM (no bridged error
        // struct, no `.code` — the CI compiler said so): the domain is on
        // the NSError, the case is its code.
        let ns = error as NSError
        guard ns.domain == NEHotspotConfigurationErrorDomain,
              let code = NEHotspotConfigurationError(rawValue: ns.code) else {
            return .other(error.localizedDescription)
        }
        switch code {
        case .alreadyAssociated: return .alreadyOn
        case .userDenied: return .canceled
        case .invalidWPAPassphrase, .invalidWEPPassphrase: return .badKey
        case .applicationIsNotInForeground: return .notInForeground
        case .pending: return .pending
        default: return .other(error.localizedDescription)
        }
    }

    // MARK: - while the person decides

    private func startReadyWatch() {
        readyWatch?.cancel()
        readyWatch = Task { [weak self] in await self?.watchWhileReady() }
    }

    private func stopReadyWatch() {
        readyWatch?.cancel()
        readyWatch = nil
    }

    /// While the list is on screen, keep an eye on `/status`: a join that
    /// finished some other way — the Canary's own page in iOS's captive
    /// pop-up, another phone at the same Canary — ends here as `.done` too,
    /// with `handedSSID` nil because this phone did not send it.
    ///
    /// Once a second, because the firmware holds the setup network only
    /// 1.6 s after the FIRST client to read `success` (the page in the
    /// pop-up acknowledges it too), so a slower beat would read success
    /// only by luck. And a portal that stops answering is the other way
    /// this ends: the network went away — after a success this client did
    /// not catch, or because the Canary was unplugged — and the person is
    /// told so, pointed at the watch-it-appear step, instead of kept over a
    /// list that no longer has a Canary behind it. A `fail` read here is
    /// someone else's wrong password, not this phone's: the list stays.
    private func watchWhileReady() async {
        var silentPolls = 0
        while !Task.isCancelled, phase == .ready {
            try? await Task.sleep(for: .seconds(1))
            guard !Task.isCancelled, phase == .ready else { return }
            switch await status() {
            case .success?:
                phase = .done
                finish()
                return
            case nil:
                silentPolls += 1
                if silentPolls >= Self.silentPollsBeforeGone {
                    finish()
                    phase = .failed(SetupPortal.portalGoneLine)
                    return
                }
            default:
                silentPolls = 0
            }
        }
    }

    // MARK: - the portal's three routes

    /// Poll `GET /scan` until the Canary stops answering "scanning" (it
    /// sweeps asynchronously, ~1-3 s) or the time runs out. Empty on any
    /// failure: the form then takes a typed name, as it always did.
    private func scan(seconds: Int, gen: Int) async -> [SetupPortal.Network] {
        let deadline = Date().addingTimeInterval(TimeInterval(seconds))
        while Date() < deadline, gen == generation {
            var request = URLRequest(url: SetupPortal.baseURL.appendingPathComponent("scan"))
            request.cachePolicy = .reloadIgnoringLocalAndRemoteCacheData
            if let reply = try? await session.data(for: request),
               let parsed = SetupPortal.parseScan(reply.0) {
                if !parsed.scanning { return parsed.networks }
            }
            try? await Task.sleep(for: .milliseconds(900))
        }
        return []
    }

    private func waitForPortal(seconds: Int, gen: Int) async -> Bool {
        let deadline = Date().addingTimeInterval(TimeInterval(seconds))
        while Date() < deadline, gen == generation {
            if await status() != nil { return true }
            try? await Task.sleep(for: .seconds(1))
        }
        return false
    }

    private func status() async -> SetupPortal.JoinState? {
        var request = URLRequest(url: SetupPortal.baseURL.appendingPathComponent("status"))
        request.cachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        guard let reply = try? await session.data(for: request) else { return nil }
        return SetupPortal.parseStatus(reply.0)
    }
}
