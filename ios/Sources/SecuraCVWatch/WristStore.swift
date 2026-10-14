// WristStore.swift  (watch app target)
//
// The wrist's one observable — the receiving end of WatchLink. It adopts
// WristSnapshots from the phone (applicationContext while asleep, live
// message replies while foregrounded), keeps only the newest by the shared
// `isNewer(than:)` rule, parks every adopted snapshot in the watch-local
// app-group cache so the complications render it, and tells WidgetKit to
// redraw. The phone's FleetStore stays the single source of truth; this
// object never invents state — it only remembers, orders, and requests.

import Foundation
import WatchConnectivity
import WidgetKit

@MainActor
final class WristStore: NSObject, ObservableObject {
    /// The newest fleet truth we hold (cache-hydrated instantly on launch).
    @Published private(set) var snapshot: WristSnapshot?
    /// When THIS watch last heard from the phone (nil = cache only so far) —
    /// the honesty anchor behind "Updated … ago".
    @Published private(set) var lastHeardFromPhone: Date?
    @Published private(set) var isPhoneReachable = false
    /// The phone sent a schema this build can't read: the watch app is the
    /// stale half. Surfaced, never silent.
    @Published private(set) var phoneSpeaksNewerSchema = false
    /// A path self-test was requested from the wrist and hasn't reported
    /// back as state yet.
    @Published private(set) var testRequestInFlight = false
    /// A find deep link (the complication) named this witness; the glance
    /// view consumes it — resolving the row and pushing the search — and
    /// clears it. A route naming a row this snapshot doesn't hold consumes
    /// to nothing, never to a dead-end screen.
    @Published var pendingFindID: String?
    private var testTimeoutTask: Task<Void, Never>?
    /// When the complications were last told to redraw (see
    /// `reloadWidgetsIfTheyMoved`), and the longest a glance may go without
    /// one while snapshots keep arriving.
    fileprivate var lastWidgetReload: Date = .distantPast
    fileprivate static let widgetRefreshFloor: TimeInterval = 15 * 60

    func activate() {
        if snapshot == nil { snapshot = WristCache.load() }
        guard WCSession.isSupported() else { return }
        let session = WCSession.default
        session.delegate = self
        session.activate()
    }

    /// Ask the phone for fresh state over the live channel. Cheap no-op when
    /// the phone isn't reachable — the applicationContext path still delivers
    /// eventually.
    func requestRefresh() {
        let session = WCSession.default
        guard WCSession.isSupported(),
              session.activationState == .activated, session.isReachable else { return }
        session.sendMessage([WristSync.messageCommandKey: WristSync.commandRefresh],
                            replyHandler: { [weak self] reply in
                                Task { @MainActor in self?.adopt(context: reply) }
                            },
                            errorHandler: nil)
    }

    /// Fire the end-to-end alert-path self-test from the wrist (RFC §3.3
    /// screen 2). The phone runs it and REPLIES with the verdict-carrying
    /// snapshot — correlation by construction: only that reply resolves this
    /// request, so a queued context or an unrelated heartbeat can never be
    /// mistaken for the answer.
    func runPathTest() {
        let session = WCSession.default
        guard WCSession.isSupported(),
              session.activationState == .activated, session.isReachable else { return }
        testRequestInFlight = true
        // Belt for the wedge case: if the phone dies mid-test (or the reply
        // never arrives), the flag must not pin the UI on "Testing…" forever
        // — after the timeout the screen honestly re-renders whatever state
        // the last snapshot carries.
        testTimeoutTask?.cancel()
        testTimeoutTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(45))
            guard !Task.isCancelled else { return }
            self?.testRequestInFlight = false
        }
        session.sendMessage([WristSync.messageCommandKey: WristSync.commandTestAlertPath],
                            replyHandler: { [weak self] reply in
                                Task { @MainActor in self?.resolvePathTest(with: reply) }
                            },
                            errorHandler: { [weak self] _ in
                                Task { @MainActor in
                                    self?.testRequestInFlight = false
                                    self?.testTimeoutTask?.cancel()
                                }
                            })
    }

    /// Mute one witness from the wrist, for a chosen length. The phone owns
    /// the mute semantics (ledger, tamper punch-through, what "tonight"
    /// means); the reply snapshot carries the updated row, so the screen
    /// answers the tap immediately.
    func mute(id: String, duration: MuteDuration = .oneHour) {
        let session = WCSession.default
        guard WCSession.isSupported(),
              session.activationState == .activated, session.isReachable else { return }
        session.sendMessage([WristSync.messageCommandKey: WristSync.commandMute,
                             WristSync.muteIDKey: id,
                             WristSync.muteDurationKey: duration.rawValue],
                            replyHandler: { [weak self] reply in
                                Task { @MainActor in self?.adopt(context: reply) }
                            },
                            errorHandler: nil)
    }

    /// Acknowledge one witness's alert from the wrist. The phone carries it
    /// out through the same acknowledgment the Alerts tab and the
    /// notification action use, so it clears everywhere; the reply snapshot
    /// carries the row's new handling, so the screen answers the tap.
    func acknowledge(witnessID: String) {
        let session = WCSession.default
        guard WCSession.isSupported(),
              session.activationState == .activated, session.isReachable else { return }
        session.sendMessage([WristSync.messageCommandKey: WristSync.commandAck,
                             WristSync.ackIDKey: witnessID],
                            replyHandler: { [weak self] reply in
                                Task { @MainActor in self?.adopt(context: reply) }
                            },
                            errorHandler: nil)
    }

    /// Ask the phone to make one Canary chirp and blink (~15 s identify).
    /// The PHONE carries it out over Wi-Fi by device id; the completion gets
    /// the honest outcome — accepted or not, visual-only or audible, and the
    /// verbatim reason on failure. Completion always runs on the main actor.
    func identify(id: String,
                  completion: @escaping @MainActor (Bool, Bool, String?) -> Void) {
        let session = WCSession.default
        guard WCSession.isSupported(),
              session.activationState == .activated, session.isReachable else {
            Task { @MainActor in completion(false, false, "iPhone not reachable right now.") }
            return
        }
        session.sendMessage([WristSync.messageCommandKey: WristSync.commandIdentify,
                             WristSync.identifyIDKey: id],
                            replyHandler: { reply in
                                Task { @MainActor in
                                    completion(reply[WristSync.identifyOKKey] as? Bool ?? false,
                                               reply[WristSync.identifyVisualOnlyKey] as? Bool ?? false,
                                               reply[WristSync.identifyWhyKey] as? String)
                                }
                            },
                            errorHandler: { error in
                                Task { @MainActor in
                                    completion(false, false, error.localizedDescription)
                                }
                            })
    }

    /// The reply to OUR test request: adopt the snapshot it carries and play
    /// the verdict it states — the one correlated answer.
    private func resolvePathTest(with reply: [String: Any]) {
        testRequestInFlight = false
        testTimeoutTask?.cancel()
        adopt(context: reply)
        switch WristSync.snapshot(fromContext: reply)?.heartbeat {
        case .alive: WristFeedback.play(FeedbackPolicy.pathTest(verified: true))
        case .failed: WristFeedback.play(FeedbackPolicy.pathTest(verified: false))
        default: break
        }
    }

    private func adopt(context: [String: Any]) {
        guard let incoming = WristSync.snapshot(fromContext: context) else {
            if let v = WristSync.contextVersion(of: context), v > WristSnapshot.schemaVersion {
                phoneSpeaksNewerSchema = true
            }
            return
        }
        phoneSpeaksNewerSchema = false
        lastHeardFromPhone = Date()

        // Unstick only: a non-testing snapshot means no test is running on
        // the phone anymore. The felt VERDICT never comes from here — only
        // from resolvePathTest's correlated reply — so an unrelated queued
        // context can't buzz as an answer.
        if incoming.heartbeat != .testing {
            testRequestInFlight = false
            testTimeoutTask?.cancel()
        }

        guard incoming.isNewer(than: snapshot) else { return }
        let previous = snapshot
        let previousWorst = previous?.severity ?? .ok
        snapshot = incoming
        WristCache.save(incoming)
        reloadWidgetsIfTheyMoved(from: previous, to: incoming)

        // Same one policy as the phone: escalations and the all-clear, felt
        // once at the transition.
        WristFeedback.play(FeedbackPolicy.fleetTransition(from: previousWorst,
                                                          to: incoming.severity))
    }
}

extension WristStore {
    /// Reload the complications only when what they DRAW moved — the glance
    /// (WristSnapshot.drawsSameGlance) or the rows the Find complication
    /// names — or when the last reload is old enough that its "as of" would
    /// mislead. WidgetKit reloads are budgeted; since the snapshot also
    /// carries each room's presence words, reloading on every snapshot would
    /// spend that budget on changes no complication shows and leave none for
    /// the ones that do.
    fileprivate func reloadWidgetsIfTheyMoved(from old: WristSnapshot?, to new: WristSnapshot) {
        // Everything WristLastFind.findableTarget gates on, so a withdrawn
        // discovery consent or a new twin drops the Find complication's deep
        // link at once, not at the next refresh floor.
        func findable(_ s: WristSnapshot?) -> [String] {
            let consent = s?.discoveryConsented == true ? "c" : "-"
            return [consent] + (s?.witnesses ?? []).map {
                "\($0.id)|\($0.name)|\($0.fingerprint ?? "")|\($0.suffixAmbiguous == true)"
            }
        }
        let glanceMoved = old.map { !new.drawsSameGlance(as: $0) } ?? true
        let rowsMoved = findable(old) != findable(new)
        let overdue = Date().timeIntervalSince(lastWidgetReload) > Self.widgetRefreshFloor
        guard glanceMoved || rowsMoved || overdue else { return }
        lastWidgetReload = Date()
        WidgetCenter.shared.reloadAllTimelines()
    }
}

extension WristStore: WCSessionDelegate {
    nonisolated func session(_ session: WCSession,
                             activationDidCompleteWith activationState: WCSessionActivationState,
                             error: Error?) {
        guard activationState == .activated else { return }
        Task { @MainActor in
            let session = WCSession.default
            self.isPhoneReachable = session.isReachable
            // The system keeps the last context for us across launches —
            // adopt it before asking for anything fresher.
            self.adopt(context: session.receivedApplicationContext)
            self.requestRefresh()
        }
    }

    nonisolated func session(_ session: WCSession,
                             didReceiveApplicationContext applicationContext: [String: Any]) {
        Task { @MainActor in self.adopt(context: applicationContext) }
    }

    nonisolated func sessionReachabilityDidChange(_ session: WCSession) {
        Task { @MainActor in
            self.isPhoneReachable = WCSession.default.isReachable
            if self.isPhoneReachable { self.requestRefresh() }
        }
    }
}

// MARK: - Previews

extension WristStore {
    /// A store pre-seeded with the deterministic sample — previews only.
    static func preview() -> WristStore {
        let store = WristStore()
        store.snapshot = .sample()
        store.lastHeardFromPhone = Date()
        return store
    }
}
