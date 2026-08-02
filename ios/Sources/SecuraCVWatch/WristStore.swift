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
    private var testTimeoutTask: Task<Void, Never>?

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
    /// screen 2). The phone runs it; the verdict comes back as heartbeat
    /// state in the next snapshot.
    func runPathTest() {
        let session = WCSession.default
        guard WCSession.isSupported(),
              session.activationState == .activated, session.isReachable else { return }
        testRequestInFlight = true
        // Belt for the wedge case: if the phone dies mid-test (or its result
        // push never arrives), the flag must not pin the UI on "Testing…"
        // forever — after the timeout the screen honestly re-renders whatever
        // state the last snapshot carries.
        testTimeoutTask?.cancel()
        testTimeoutTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(45))
            guard !Task.isCancelled else { return }
            self?.testRequestInFlight = false
        }
        session.sendMessage([WristSync.messageCommandKey: WristSync.commandTestAlertPath],
                            replyHandler: { _ in },
                            errorHandler: { [weak self] _ in
                                Task { @MainActor in self?.testRequestInFlight = false }
                            })
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
        testRequestInFlight = false
        testTimeoutTask?.cancel()
        guard incoming.isNewer(than: snapshot) else { return }
        snapshot = incoming
        WristCache.save(incoming)
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
