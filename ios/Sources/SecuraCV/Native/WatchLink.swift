// WatchLink.swift
//
// The iPhone side of the wrist pipeline (RFC
// docs/design/apple_watch_and_notifications.md §3.2): pushes the shared
// WristSnapshot over WatchConnectivity and answers the watch's two requests
// (refresh, run the path self-test). Delivery is `updateApplicationContext` —
// latest-state-wins, queued by the system while the watch sleeps — so the
// wrist converges on current truth without any bookkeeping of its own.
//
// Push discipline: a snapshot is sent only when its CONTENT changed (compared
// on the pinned, sorted-keys encoding with revision/sentAt zeroed), plus a
// low-cadence re-send so a long-quiet fleet still proves the pipe itself is
// alive. Revisions are monotonic and persisted, so the watch can order
// deliveries even across app relaunches; `sentAt` breaks the tie after a
// reinstall resets the counter.
//
// Guarded on WatchConnectivity like LiveActivityController is on ActivityKit —
// same file shape, same no-op fallback, so no caller ever cares.

#if canImport(WatchConnectivity)
import Foundation
import WatchConnectivity

@MainActor
final class WatchLink: NSObject {
    static let shared = WatchLink()

    private weak var store: FleetStore?

    /// Fingerprint of the last content actually pushed (volatile fields
    /// zeroed), and when — see `push(_:)`.
    private var lastPushedFingerprint: Data?
    private var lastPushedAt: Date = .distantPast

    /// Re-send even unchanged state this often, so "no news" stays
    /// distinguishable from "no pipe".
    private let quietResendInterval: TimeInterval = 60 * 60

    private static let revisionKey = "wrist_snapshot_revision_v1"

    func activate(store: FleetStore) {
        guard WCSession.isSupported() else { return }
        self.store = store
        let session = WCSession.default
        session.delegate = self
        session.activate()
    }

    /// Push the store's current truth if it changed (or the quiet re-send
    /// interval elapsed). Call after every fleet refresh — cheap when nothing
    /// moved. `force: true` bypasses the content dedup: use it when the PUSH
    /// itself is the answer to something the watch asked for (a finished path
    /// test whose outcome happens to equal the last one would otherwise be
    /// deduped away, leaving the wrist stuck on "Testing…").
    func pushCurrent(force: Bool = false) {
        guard let store else { return }
        push(WristSnapshot(store: store), force: force)
    }

    private func push(_ snapshot: WristSnapshot, force: Bool = false) {
        guard WCSession.isSupported() else { return }
        let session = WCSession.default
        guard session.activationState == .activated,
              session.isPaired, session.isWatchAppInstalled else { return }

        guard let fingerprint = Self.fingerprint(of: snapshot) else { return }
        let quietTooLong = Date().timeIntervalSince(lastPushedAt) > quietResendInterval
        if !force && fingerprint == lastPushedFingerprint && !quietTooLong { return }

        do {
            try session.updateApplicationContext(WristSync.context(for: stamped(snapshot)))
            lastPushedFingerprint = fingerprint
            lastPushedAt = Date()
        } catch {
            // A failed context update is retried by the next refresh tick;
            // never noisy — the watch shows staleness honestly regardless.
        }
    }

    /// Stamp the send-time fields: the next monotonic revision and now.
    private func stamped(_ snapshot: WristSnapshot) -> WristSnapshot {
        let defaults = UserDefaults.standard
        let next = UInt64(max(0, defaults.integer(forKey: Self.revisionKey))) + 1
        defaults.set(Int(next), forKey: Self.revisionKey)
        var s = snapshot
        s.revision = next
        s.sentAt = Date()
        return s
    }

    /// Content identity: the pinned encoding with the send-time fields
    /// zeroed, so "same fleet truth" compares equal across pushes.
    private static func fingerprint(of snapshot: WristSnapshot) -> Data? {
        var s = snapshot
        s.revision = 0
        s.sentAt = Date(timeIntervalSince1970: 0)
        return try? WristSync.makeEncoder().encode(s)
    }

    /// The watch asked for fresh state right now (its foreground moment).
    /// Reply on the live channel AND remember it as the latest push.
    private func replyWithCurrentSnapshot(_ replyHandler: @escaping ([String: Any]) -> Void) {
        guard let store else { replyHandler([:]); return }
        let snapshot = stamped(WristSnapshot(store: store))
        if let context = try? WristSync.context(for: snapshot) {
            lastPushedFingerprint = Self.fingerprint(of: snapshot)
            lastPushedAt = Date()
            replyHandler(context)
        } else {
            replyHandler([:])
        }
    }
}

extension WatchLink: WCSessionDelegate {
    nonisolated func session(_ session: WCSession,
                             activationDidCompleteWith activationState: WCSessionActivationState,
                             error: Error?) {
        guard activationState == .activated else { return }
        Task { @MainActor in self.pushCurrent() }
    }

    // iOS-side requirements for switching between paired watches: hand the
    // session straight back so the newly active watch gets state.
    nonisolated func sessionDidBecomeInactive(_ session: WCSession) {}
    nonisolated func sessionDidDeactivate(_ session: WCSession) {
        session.activate()
    }

    /// The live channel: the watch app is foregrounded and reachable.
    nonisolated func session(_ session: WCSession,
                             didReceiveMessage message: [String: Any],
                             replyHandler: @escaping ([String: Any]) -> Void) {
        let command = message[WristSync.messageCommandKey] as? String
        Task { @MainActor in
            switch command {
            case WristSync.commandRefresh:
                self.replyWithCurrentSnapshot(replyHandler)
            case WristSync.commandMute:
                if let id = message[WristSync.muteIDKey] as? String, !id.isEmpty {
                    // Unspecified or unreadable → an hour, the length this
                    // command has always meant. A duration we can't parse
                    // must never become a longer silence than asked for.
                    let raw = message[WristSync.muteDurationKey] as? String ?? ""
                    self.store?.mute(id, duration: MuteDuration(rawValue: raw) ?? .oneHour)
                }
                // Answer with the post-mute snapshot so the wrist row updates
                // in the same breath as the tap.
                self.replyWithCurrentSnapshot(replyHandler)
            case WristSync.commandIdentify:
                // The wrist asked one Canary to make itself known. The phone
                // carries it out over Wi-Fi (FleetStore.identifyCanary) and
                // the reply says exactly what happened — the wrist's copy
                // must claim a chirp only when a device actually accepted.
                guard let id = message[WristSync.identifyIDKey] as? String, !id.isEmpty,
                      let store = self.store else {
                    replyHandler([WristSync.identifyOKKey: false,
                                  WristSync.identifyWhyKey: "The iPhone couldn't take the request."])
                    return
                }
                let outcome = await store.identifyCanary(id: id)
                var reply: [String: Any] = [WristSync.identifyOKKey: outcome.ok,
                                            WristSync.identifyVisualOnlyKey: outcome.visualOnly]
                if let why = outcome.why { reply[WristSync.identifyWhyKey] = why }
                replyHandler(reply)
            case WristSync.commandTestAlertPath:
                // Run the test, THEN answer with the verdict-carrying
                // snapshot: the reply IS this request's result, so the wrist
                // can never mistake an unrelated heartbeat for its answer.
                // playFeedback: false — the answer belongs to the hand that
                // asked; the watch plays its own verdict, the phone stays
                // silent in the pocket.
                await self.store?.runTestAlert(playFeedback: false)
                self.replyWithCurrentSnapshot(replyHandler)
            default:
                replyHandler([:])
            }
        }
    }
}

#else

/// No-op fallback so callers never care whether the platform has
/// WatchConnectivity (same shape as LiveActivityController's ActivityKit
/// fallback).
@MainActor
final class WatchLink {
    static let shared = WatchLink()
    func activate(store: FleetStore) {}
    func pushCurrent(force: Bool = false) {}
}
#endif
