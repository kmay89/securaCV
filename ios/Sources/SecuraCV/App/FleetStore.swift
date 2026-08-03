// FleetStore.swift
//
// The one observable the whole app watches. It owns the transports (discovery,
// per-device HTTP, BLE), folds their inputs into the fleet + timeline, keeps
// the Dynamic Island in sync, and runs the heartbeat/self-test. Everything the
// device tells us is rendered through the SAME small set of primitives, so new
// firmware fields light up without an app update (the anti-rot bet).

import Foundation
import SwiftUI
import Combine

@MainActor
final class FleetStore: ObservableObject {
    // Surfaces
    @Published var witnesses: [Witness] = []
    @Published var timeline: [TimelineEvent] = []
    @Published var fleetName: String = "Your Canaries"
    @Published var isRefreshing = false

    // Demo mode: the seeded DemoFleet joins (never replaces) anything real,
    // so the app demos on a Simulator or a hardware-free phone. Views flip it
    // through setDemoMode(_:) so the choice persists; direct sets are for
    // previews only.
    @Published var demoMode: Bool

    // Collaborators
    let devices: DeviceStore
    let discovery = Discovery()
    let ble = BLEConsole()
    let alerts = AlertCenter()
    let heartbeat = Heartbeat()

    private var refreshTask: Task<Void, Never>?
    private var bag = Set<AnyCancellable>()

    // True while the heartbeat's lastVerified came from demo seeding rather
    // than a real end-to-end confirmation — so it can be revoked the moment
    // it might mask something real.
    private var heartbeatIsDemoFed = false

    var worstSeverity: Severity { witnesses.map(\.effectiveSeverity).max() ?? .ok }
    var allQuiet: Bool { worstSeverity == .ok }

    init() {
        let devices = DeviceStore()
        self.devices = devices
        self.demoMode = DemoFleet.defaultEnabled(hasPairedDevices: !devices.devices.isEmpty)

        // Forward every collaborator's change into ours, so any view observing
        // the store updates when discovery, BLE, alerts, heartbeat, or the
        // device list change — no view has to know the internal object graph.
        for child in [devices.objectWillChange, discovery.objectWillChange,
                      ble.objectWillChange, alerts.objectWillChange,
                      heartbeat.objectWillChange] {
            child.sink { [weak self] in self?.objectWillChange.send() }.store(in: &bag)
        }
    }

    // MARK: - lifecycle

    func onAppear() {
        discovery.start()
        ble.startScan()
        WatchLink.shared.activate(store: self)
        Task { await alerts.requestAuthorization() }
        Task { await hydrateFromCloud() }
        recordDemoBeatIfHarmless()
        startRefreshLoop()
        pushLiveActivity()
    }

    /// The UI's one entry point for demo mode — persists the choice and
    /// re-renders immediately. Leaving demo forgets any demo-fed heartbeat so
    /// the "provably alive" card never carries a verification it didn't earn.
    func setDemoMode(_ on: Bool) {
        guard on != demoMode else { return }
        demoMode = on
        DemoFleet.remember(on)
        if on { recordDemoBeatIfHarmless() } else { revokeDemoBeat() }
        Task { await refreshOnce() }
    }

    /// The demo may fake the delivery heartbeat ONLY while no real device is
    /// paired: with nothing real to go dark, "alive" is a harmless stage prop.
    /// The moment a real fleet exists, its dead-man's-switch (docs §5b) must
    /// never be masked by sample data — real beats only.
    private func recordDemoBeatIfHarmless() {
        guard demoMode else { return }
        if devices.devices.isEmpty {
            heartbeat.recordBeat()
            heartbeatIsDemoFed = true
        } else {
            revokeDemoBeat()
        }
    }

    /// Back to "Not yet verified" — but only if the beat was demo-fed; a real
    /// confirmation is never discarded.
    private func revokeDemoBeat() {
        if heartbeatIsDemoFed {
            heartbeat.reset()
            heartbeatIsDemoFed = false
        }
    }

    func onScenePhase(active: Bool) {
        if active {
            discovery.start()
            ble.startScan()
            heartbeat.tick()
            Task { await refreshOnce() }
        } else {
            discovery.stop()
            // The beacon scan is unfiltered with duplicates on — deliberately a
            // foreground transport (iOS withholds manufacturer data from
            // background scans anyway). Stopping it in the background saves the
            // radio; away-from-home delivery is APNs' job, not BLE's.
            ble.stopScan()
        }
    }

    private func hydrateFromCloud() async {
        let cloud = await CloudSync.shared.pull()
        if !cloud.isEmpty { devices.mergeFromCloud(cloud) }
    }

    // MARK: - refresh

    private func startRefreshLoop() {
        refreshTask?.cancel()
        refreshTask = Task { [weak self] in
            while !Task.isCancelled {
                await self?.refreshOnce()
                try? await Task.sleep(for: .seconds(20))
            }
        }
    }

    func refreshOnce() async {
        isRefreshing = true
        defer { isRefreshing = false }

        var next: [Witness] = []
        var events: [TimelineEvent] = []

        // ── Tier 3: paired devices, authenticated and locally verified ──
        await withTaskGroup(of: (Witness, [TimelineEvent])?.self) { group in
            for ref in devices.devices {
                group.addTask { await Self.poll(ref, store: self) }
            }
            for await result in group {
                if let (w, evs) = result { next.append(w); events.append(contentsOf: evs) }
            }
        }

        // ── Tier 2: /api/fleet on Canaries we can see but haven't paired ──
        // The fleet-wide, unauthenticated self-report. This is what makes a
        // canary-display (and any board that later gains an HTTP server) show
        // up over Wi-Fi at all — it serves no /api/v1 device-api.
        let pairedIDs = Set(devices.devices.map(\.id))
        let unpairedHosts: [String] = discovery.found
            .filter { !pairedIDs.contains($0.id) }
            .compactMap(\.host)
        if !unpairedHosts.isEmpty {
            let reports = await withTaskGroup(of: (String, FleetSelfReport)?.self) { group -> [(String, FleetSelfReport)] in
                for host in unpairedHosts {
                    group.addTask {
                        // Bare mDNS labels need their implied .local — see
                        // DeviceAPI.url(forDiscoveredHost:).
                        guard let url = DeviceAPI.url(forDiscoveredHost: host),
                              let report = try? await DeviceAPI.fleetSelfReport(at: url) else { return nil }
                        return (host, report)
                    }
                }
                var out: [(String, FleetSelfReport)] = []
                for await r in group { if let r { out.append(r) } }
                return out
            }
            for (host, report) in reports {
                // A hub answers for itself AND its peers, so every row counts —
                // and each needs its own id (see provisionalWitness).
                for (index, row) in report.devices.enumerated() {
                    if let i = next.firstIndex(where: { $0.name == row.name && !row.name.isEmpty }) {
                        FleetMerge.fold(row, into: &next[i])
                    } else {
                        next.append(FleetMerge.provisionalWitness(from: row, host: host, index: index))
                    }
                }
            }
        }

        // ── Tier 1a: the WAP-class GATT console snapshot ──
        for (id, snap) in ble.snapshotsByDevice where !next.contains(where: { $0.id == id }) {
            var w = Witness(id: id)
            w.seenViaBLE = true
            w.tamper = snap.tamper ?? false
            w.batteryPct = snap.batt
            w.link = .online
            next.append(w)
        }

        // ── Tier 1b: the universal presence beacon — every Canary, no broker,
        // no home Wi-Fi, no pairing. Attached last so it decorates the rows
        // above rather than competing with them.
        ble.pruneStaleSightings()
        FleetMerge.attach(ble.freshSightings, to: &next)

        // Demo fleet: seeded witnesses/events join anything real (ids are
        // "demo-"-namespaced, so they can't collide) — a live Canary paired
        // mid-demo still shows up beside the samples. The heartbeat is only
        // demo-fed while nothing real is paired (see recordDemoBeatIfHarmless).
        if demoMode {
            next.append(contentsOf: DemoFleet.witnesses())
            events.append(contentsOf: DemoFleet.timeline())
            recordDemoBeatIfHarmless()
        }

        witnesses = next.sorted { $0.effectiveSeverity > $1.effectiveSeverity }
        timeline = events.sorted { $0.timeBucket > $1.timeBucket }
        // Re-evaluate the dead-man's-switch EVERY cycle, not just on scene
        // changes: the island and the wrist serialize its state, and a phone
        // that stays foregrounded past the dark window must never keep
        // exporting `.alive` (the wrist would show a green glyph beside its
        // own "last beat 47 min ago" text).
        heartbeat.tick()
        pushLiveActivity()
        WatchLink.shared.pushCurrent()   // content-deduped; free when nothing moved
        evaluateAlerts()
        // Felt once, at the crossing — never on the cycles that stay there.
        // Compared against the last severity we BUZZED for, read and written
        // in one hop at commit time: overlapping refreshes (actor reentrancy
        // across the awaits above) can't both claim the same crossing.
        let felt = FeedbackPolicy.fleetTransition(from: lastFeltWorst, to: worstSeverity)
        lastFeltWorst = worstSeverity
        Feedback.play(felt)
    }

    /// The last worst-severity this store produced feedback against — the
    /// serialization point for the one-buzz-per-crossing promise.
    private var lastFeltWorst: Severity = .ok

    /// Poll one device: /info for liveness + /witness for the chain head, verify
    /// on-device, pin its key TOFU. Static so the task group stays Sendable-safe.
    private static func poll(_ ref: PairedDeviceRef, store: FleetStore) async -> (Witness, [TimelineEvent])? {
        guard ref.deviceType.isHTTPPairable, let api = try? store.devices.api(for: ref) else {
            return (Witness(id: ref.id, deviceType: ref.deviceType, name: ref.name), [])
        }
        guard let info = try? await api.info() else {
            var w = Witness(id: ref.id, deviceType: ref.deviceType, name: ref.name)
            w.link = .lost                       // couldn't reach it → dark
            return (w, [])
        }
        var w = Witness(id: info.deviceID, deviceType: ref.deviceType, name: info.name)
        // Derive the device's fingerprint from the key we already pinned, using
        // the firmware's own scheme (Wire/DeviceFingerprint). Without this the
        // field stays empty, FleetMerge.attach can never tie a heard beacon to
        // this row, and the same Canary appears twice — once here, once as an
        // anonymous SCV-XXXX — while this row may sit .lost and raise a false
        // alert. /api/v1/info carries no fingerprint, so the pinned key is the
        // source.
        if let pub = PinnedKeyStore.key(for: ref.id),
           let fp = DeviceFingerprint.hex(forPublicKey: pub) {
            w.fingerprint = fp
        }
        w.firmware = info.firmwareVersion
        w.rssiDBM = info.wifiRSSI
        w.link = .online
        w.lastSeen = Date()
        w.baseURL = ref.baseURL

        var events: [TimelineEvent] = []
        if let page = try? await api.witness(last: 20) {
            let verdict = ChainVerifier.verify(page, pinnedKey: PinnedKeyStore.key(for: ref.id))
            w.badge = verdict.badge
            // The wire's seq is u64; the model mirrors fleet_model.h's
            // uint32_t chain_length, so clamp at the fold (like the tolerant
            // enum decoders — a newer firmware never breaks an older app).
            w.chainLength = UInt32(clamping: page.records.map(\.seq).max() ?? 0)
            events = page.records.map { rec in
                TimelineEvent(id: "\(ref.id)#\(rec.seq)",
                              deviceID: ref.id, deviceName: info.name, zone: rec.zone,
                              headline: Self.headline(rec, device: info.name),
                              severity: rec.severity, badge: verdict.badge,
                              timeBucket: Self.bucket(rec.timestamp),
                              symbol: EventVocabulary.sfSymbol(forWire: rec.eventType))
            }
            if let head = page.records.max(by: { $0.seq < $1.seq }), !head.signature.isEmpty {
                w.lastEvent = Self.headline(head, device: info.name)
                w.lastEventAt = head.timestamp
                w.lastEventSeverity = head.severity
            }
        }
        return (w, events)
    }

    private static func headline(_ rec: WitnessRecord, device: String) -> String {
        // One vocabulary of meaning for every surface — the dictionary ids,
        // the device dialect, and a readable fallback for events from a
        // newer fleet than this app (Shared/EventVocabulary.swift).
        EventVocabulary.headline(forWire: rec.eventType, zone: rec.zone, deviceName: device)
    }

    /// Coarse 10-minute bucket — never a precise second (Invariant III).
    /// Internal (not private) because the wrist snapshot builder applies the
    /// SAME coarsening to every timestamp it puts on the wire.
    static func bucket(_ date: Date) -> Date {
        let t = date.timeIntervalSince1970
        return Date(timeIntervalSince1970: (t / 600).rounded(.down) * 600)
    }

    // MARK: - alerts + island

    private func evaluateAlerts() {
        // On-LAN, notifications fire locally; away, the relay path would drive
        // these via APNs+NSE. Here we surface anything that crosses a rule.
        for w in witnesses where w.effectiveSeverity >= .alert {
            if let level = alerts.level(for: w.effectiveSeverity, awayFromHome: false) {
                alerts.post(title: fleetName, body: "\(w.displayName): \(w.statusLine)",
                            level: level, threadID: w.id)
            }
        }
    }

    private func pushLiveActivity() {
        let ago: Int? = heartbeat.lastVerified.map { Int(Date().timeIntervalSince($0)) }
        let state = FleetActivityAttributes.State(fleet: witnesses, lastVerifiedAgo: ago)
        LiveActivityController.shared.start(fleetName: fleetName, state: state)
        Task { await LiveActivityController.shared.update(state) }
    }

    // MARK: - test alert (the "provably alive" button)

    /// `playFeedback: false` for wrist-originated tests: the answer lands in
    /// the hand that asked (WatchLink replies with the verdict snapshot and
    /// the watch plays its own), so the phone stays silent in the pocket.
    func runTestAlert(playFeedback: Bool = true) async {
        let fleet = fleetName
        let alerts = alerts
        await heartbeat.runTestAlert {
            // On-LAN self-test: post a local time-sensitive notification so
            // the user SEES the whole path light up — and CONFIRM the system
            // accepted it. Notifications off → this throws → the heartbeat
            // honestly records a FAILED path instead of a hollow "verified"
            // (the away path swaps this closure for device→relay→APNs).
            try await alerts.postConfirmed(title: fleet,
                                           body: "Test alert — your fleet can reach you.",
                                           level: .important, threadID: "selftest")
        }
        pushLiveActivity()
        // Forced: the wrist is waiting on this exact push to leave its
        // "Testing…" state, and a repeat failure with the same reason would
        // be byte-identical to the last snapshot and vanish into the dedup.
        WatchLink.shared.pushCurrent(force: true)
        if playFeedback {
            // The user asked with a tap here; a verified path earns the chirp.
            Feedback.play(FeedbackPolicy.pathTest(verified: heartbeat.state.isHealthy))
        }
    }
}
