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
        Task { await alerts.requestAuthorization() }
        Task { await hydrateFromCloud() }
        if demoMode { heartbeat.recordBeat() }   // the demo's path is, by definition, alive
        startRefreshLoop()
        pushLiveActivity()
    }

    /// The UI's one entry point for demo mode — persists the choice and
    /// re-renders immediately. Leaving demo forgets the demo heartbeat so the
    /// "provably alive" card never carries a verification it didn't earn.
    func setDemoMode(_ on: Bool) {
        guard on != demoMode else { return }
        demoMode = on
        DemoFleet.remember(on)
        if on { heartbeat.recordBeat() } else { heartbeat.reset() }
        Task { await refreshOnce() }
    }

    func onScenePhase(active: Bool) {
        if active {
            discovery.start()
            heartbeat.tick()
            Task { await refreshOnce() }
        } else {
            discovery.stop()
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

        await withTaskGroup(of: (Witness, [TimelineEvent])?.self) { group in
            for ref in devices.devices {
                group.addTask { await Self.poll(ref, store: self) }
            }
            for await result in group {
                if let (w, evs) = result { next.append(w); events.append(contentsOf: evs) }
            }
        }

        // Fold in BLE snapshots for anything we heard off-grid.
        for (id, snap) in ble.snapshotsByDevice where !next.contains(where: { $0.id == id }) {
            var w = Witness(id: id)
            w.seenViaBLE = true
            w.tamper = snap.tamper ?? false
            w.batteryPct = snap.batt
            w.link = .online
            next.append(w)
        }

        // Demo fleet: seeded witnesses/events join anything real (ids are
        // "demo-"-namespaced, so they can't collide) — a live Canary paired
        // mid-demo still shows up beside the samples.
        if demoMode {
            next.append(contentsOf: DemoFleet.witnesses())
            events.append(contentsOf: DemoFleet.timeline())
            heartbeat.recordBeat()
        }

        witnesses = next.sorted { $0.effectiveSeverity > $1.effectiveSeverity }
        timeline = events.sorted { $0.timeBucket > $1.timeBucket }
        pushLiveActivity()
        evaluateAlerts()
    }

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
        w.firmware = info.firmwareVersion
        w.rssiDBM = info.wifiRSSI
        w.link = .online
        w.lastSeen = Date()
        w.baseURL = ref.baseURL

        var events: [TimelineEvent] = []
        if let page = try? await api.witness(last: 20) {
            let verdict = ChainVerifier.verify(page, pinnedKey: PinnedKeyStore.key(for: ref.id))
            w.badge = verdict.badge
            w.chainLength = page.records.map(\.seq).max() ?? 0
            events = page.records.map { rec in
                TimelineEvent(id: "\(ref.id)#\(rec.seq)",
                              deviceID: ref.id, deviceName: info.name, zone: rec.zone,
                              headline: Self.headline(rec, device: info.name),
                              severity: rec.severity, badge: verdict.badge,
                              timeBucket: Self.bucket(rec.timestamp))
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
        switch rec.eventType {
        case "person_detected": return "Someone at \(rec.zone.isEmpty ? device : rec.zone)"
        case "vehicle_detected": return "Vehicle at \(rec.zone.isEmpty ? device : rec.zone)"
        case "motion_detected": return "Motion at \(rec.zone.isEmpty ? device : rec.zone)"
        case "tamper", "panic": return "Tamper at \(device)"
        default: return rec.eventType.replacingOccurrences(of: "_", with: " ").capitalized
        }
    }

    /// Coarse 10-minute bucket — never a precise second (Invariant III).
    private static func bucket(_ date: Date) -> Date {
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

    func runTestAlert() async {
        await heartbeat.runTestAlert { [weak self] in
            // On-LAN self-test: post a local time-sensitive notification so the
            // user SEES the whole path light up. The away path swaps this closure
            // for a device→relay→APNs round-trip.
            await MainActor.run {
                self?.alerts.post(title: self?.fleetName ?? "SecuraCV",
                                  body: "Test alert — your fleet can reach you.",
                                  level: .important, threadID: "selftest")
            }
        }
        pushLiveActivity()
    }
}
