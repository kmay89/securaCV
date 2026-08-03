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
import WidgetKit

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

    // Consent-first discovery: nil = never asked, false = "Not now", true =
    // radios may run. No mDNS browse, no BLE scan — and therefore no iOS
    // Local Network / Bluetooth permission dialog — before the user says so.
    @Published private(set) var discoveryConsent: Bool?

    // Collaborators
    let devices: DeviceStore
    let discovery = Discovery()
    let ble = BLEConsole()
    let alerts = AlertCenter()
    let heartbeat = Heartbeat()

    private var refreshTask: Task<Void, Never>?
    private var bag = Set<AnyCancellable>()

    /// Durable per-witness mutes, re-applied at every fold (rows are rebuilt
    /// from device truth, so mute must outlive them).
    private let muteLedger = MuteLedger()
    /// Content fingerprint of the last glance snapshot handed to the iPhone
    /// widgets — reload their timelines only when the truth changed.
    private var lastGlanceFingerprint: Data?

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
        self.discoveryConsent = Consents.discovery()

        // Notification actions must have a landing pad from the very first
        // moment: on a cold launch from a background Ack/Mute tap, iOS
        // invokes the delegate before any SwiftUI .task runs — wiring these
        // in onAppear would silently drop that tap.
        alerts.onMute = { [weak self] id in self?.mute(id) }
        alerts.onAck = { [weak self] id in self?.acknowledgeAlert(for: id) }

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
        startRadiosIfConsented()
        WatchLink.shared.activate(store: self)
        // Permission timing: a returning user with a paired fleet expects
        // alerts, so ask at launch. A brand-new user gets asked at the
        // moment alerts first matter (their first Test Alert) — never as an
        // ambush on first open.
        if !devices.devices.isEmpty {
            Task { await alerts.requestAuthorization() }
        }
        Task { await hydrateFromCloud() }
        recordDemoBeatIfHarmless()
        startRefreshLoop()
        startSentinel()
        pushLiveActivity()
    }

    /// The consent gate's one entry point. Granting starts the radios NOW —
    /// so iOS's Local Network prompt appears right after the user's own yes,
    /// in context. Declining stops them and clears anything they'd found.
    func setDiscoveryConsent(_ granted: Bool) {
        Consents.setDiscovery(granted)
        discoveryConsent = granted
        if granted {
            discovery.start()
            ble.startScan()
            startSentinel()
            Task { await refreshOnce() }
        } else {
            discovery.stop()
            ble.stopScan()
        }
    }

    private func startRadiosIfConsented() {
        guard discoveryConsent == true else { return }
        discovery.start()
        ble.startScan()
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
            startRadiosIfConsented()
            startSentinel()
            heartbeat.tick()
            Task { await refreshOnce() }
        } else {
            discovery.stop()
            // The beacon scan is unfiltered with duplicates on — deliberately a
            // foreground transport (iOS withholds manufacturer data from
            // background scans anyway). Stopping it in the background saves the
            // radio; away-from-home delivery is APNs' job, not BLE's.
            ble.stopScan()
            // The sentinel is a foreground loop too — probing from the
            // background would just burn radio for a screen nobody sees.
            sentinelTask?.cancel()
            sentinelTask = nil
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

        // Durable mutes survive the rebuild (tamper still punches through —
        // that guarantee lives in Witness.effectiveSeverity).
        muteLedger.apply(to: &next)

        // The sentinel may have marked a device dark between polls; a fresh
        // fold must not flip it back green until a probe or poll actually
        // ANSWERS — rows the miss ladder darkened stay darkened here.
        for i in next.indices {
            let misses = missCounts[next[i].id] ?? 0
            if misses >= LivenessLadder.staleAfterMisses {
                let laddered = LivenessLadder.link(afterMisses: misses)
                if laddered.rawValue > next[i].link.rawValue {
                    next[i].link = laddered
                }
            }
        }

        witnesses = next.sorted { $0.effectiveSeverity > $1.effectiveSeverity }
        timeline = events.sorted { $0.timeBucket > $1.timeBucket }
        // Re-evaluate the dead-man's-switch EVERY cycle, not just on scene
        // changes: the island and the wrist serialize its state, and a phone
        // that stays foregrounded past the dark window must never keep
        // exporting `.alive` (the wrist would show a green glyph beside its
        // own "last beat 47 min ago" text).
        heartbeat.tick()
        republishGlanceSurfaces()
        evaluateAlerts()
        commitFeltTransition()
    }

    /// The last worst-severity this store produced feedback against — the
    /// serialization point for the one-buzz-per-crossing promise.
    private var lastFeltWorst: Severity = .ok

    /// Island + wrist + iPhone widgets, in one place — every mutation path
    /// (poll, sentinel, mute) fans out identically.
    private func republishGlanceSurfaces() {
        pushLiveActivity()
        WatchLink.shared.pushCurrent()   // content-deduped; free when nothing moved
        publishGlanceCache()             // ditto, for the iPhone widgets
    }

    /// Felt once, at the crossing — never on the cycles that stay there.
    /// Compared against the last severity we BUZZED for, read and written in
    /// one hop at commit time: overlapping refreshes (actor reentrancy
    /// across awaits) can't both claim the same crossing.
    private func commitFeltTransition() {
        let felt = FeedbackPolicy.fleetTransition(from: lastFeltWorst, to: worstSeverity)
        lastFeltWorst = worstSeverity
        Feedback.play(felt)
    }

    // MARK: - the liveness sentinel (seconds-fast, all on-LAN)

    /// Consecutive probe misses per paired device — the ladder's input.
    private var missCounts: [String: Int] = [:]
    private var sentinelTask: Task<Void, Never>?

    /// Probe every paired, HTTP-capable Canary on a tight timeout every few
    /// seconds while the app is open. No cloud anywhere in the loop: an
    /// unplugged Canary reads "Quiet" in ~10s and "Lost" in ~15s straight
    /// from this phone's own Wi-Fi — while cloud cameras are still waiting
    /// for a server to miss a keepalive.
    private func startSentinel() {
        guard sentinelTask == nil else { return }
        sentinelTask = Task { [weak self] in
            while !Task.isCancelled {
                await self?.probeOnce()
                try? await Task.sleep(for: .seconds(5))
            }
        }
    }

    private func probeOnce() async {
        let targets = devices.devices.compactMap { ref -> (String, URL)? in
            guard ref.deviceType.isHTTPPairable, let url = ref.baseURL else { return nil }
            return (ref.id, url)
        }
        guard !targets.isEmpty else { return }

        let answers = await withTaskGroup(of: (String, Bool).self) { group -> [String: Bool] in
            for (id, url) in targets {
                group.addTask { (id, await LivenessProbe.isAnswering(url)) }
            }
            var out: [String: Bool] = [:]
            for await (id, alive) in group { out[id] = alive }
            return out
        }

        var rows = witnesses
        var changed = false
        for (id, alive) in answers {
            let misses = alive ? 0 : (missCounts[id] ?? 0) + 1
            missCounts[id] = misses
            guard let i = rows.firstIndex(where: { $0.id == id }) else { continue }
            let link = LivenessLadder.link(afterMisses: misses)
            if rows[i].link != link {
                rows[i].link = link
                if alive { rows[i].lastSeen = Date() }
                changed = true
            }
        }
        guard changed else { return }
        witnesses = rows.sorted { $0.effectiveSeverity > $1.effectiveSeverity }
        republishGlanceSurfaces()
        evaluateAlerts()
        commitFeltTransition()
    }

    // MARK: - mute (the ledger is the truth; rows are its projection)

    /// Mute one witness — from the detail screen, a notification action, or
    /// the wrist. Caps nagging at Notice; tamper and a failed signature
    /// still punch through (Witness.effectiveSeverity owns that guarantee).
    func mute(_ id: String, for duration: TimeInterval = 3600) {
        muteLedger.set(until: Date().addingTimeInterval(duration), for: id)
        applyMutesAndRepublish()
    }

    func unmute(_ id: String) {
        muteLedger.clear(id)
        applyMutesAndRepublish()
    }

    private func applyMutesAndRepublish() {
        var rows = witnesses
        for i in rows.indices {
            rows[i].mutedUntil = muteLedger.muteUntil(for: rows[i].id)
        }
        witnesses = rows.sorted { $0.effectiveSeverity > $1.effectiveSeverity }
        republishGlanceSurfaces()
    }

    // MARK: - the iPhone widgets' cache

    /// Park the same glance snapshot the wrist gets into the phone-side app
    /// group, and wake the widgets — only when the truth actually changed.
    private func publishGlanceCache() {
        var snapshot = WristSnapshot(store: self)
        guard let fingerprint = try? WristSync.makeEncoder().encode(snapshot),
              fingerprint != lastGlanceFingerprint else { return }
        lastGlanceFingerprint = fingerprint
        snapshot.sentAt = Date()
        PhoneGlanceCache.save(snapshot)
        WidgetCenter.shared.reloadTimelines(ofKind: PhoneGlanceCache.widgetKind)
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

    // One alert per CONDITION, not per refresh cycle: a witness that stays
    // alarmed posts once, an acknowledged alert stays quiet until the
    // condition changes or clears, and a witness that calms down becomes
    // news-worthy again. (Without this ledger every 20-second refresh — and
    // every 5-second sentinel pass — would re-post the same alarm with a
    // fresh identifier: the exact spam that gets alerts turned off.)
    private var postedAlerts: [String: String] = [:]
    private var ackedAlerts: [String: String] = [:]

    private func alertFingerprint(_ w: Witness) -> String {
        "\(w.effectiveSeverity.rawValue)|\(w.statusLine)"
    }

    /// The Ack action's landing pad: remember WHAT was acknowledged, so the
    /// same ongoing condition stays quiet but any change breaks through.
    func acknowledgeAlert(for id: String) {
        if let w = witnesses.first(where: { $0.id == id }) {
            ackedAlerts[id] = alertFingerprint(w)
        } else if let posted = postedAlerts[id] {
            ackedAlerts[id] = posted
        }
    }

    private func evaluateAlerts() {
        // On-LAN, notifications fire locally; away, the relay path would drive
        // these via APNs+NSE. Here we surface anything that crosses a rule.
        var live = Set<String>()
        for w in witnesses where w.effectiveSeverity >= .alert {
            live.insert(w.id)
            let fingerprint = alertFingerprint(w)
            guard postedAlerts[w.id] != fingerprint else { continue }   // already told
            guard ackedAlerts[w.id] != fingerprint else { continue }    // user said "seen it"
            postedAlerts[w.id] = fingerprint
            if let level = alerts.level(for: w.effectiveSeverity, awayFromHome: false) {
                alerts.post(title: fleetName, body: "\(w.displayName): \(w.statusLine)",
                            level: level, threadID: w.id)
            }
        }
        // Calmed witnesses leave both ledgers — their NEXT alert is news.
        postedAlerts = postedAlerts.filter { live.contains($0.key) }
        ackedAlerts = ackedAlerts.filter { live.contains($0.key) }
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
        // Moment-of-need permission ask: the user just said "alert me" —
        // THIS is when the notification prompt makes sense (a denied prompt
        // stays denied; requestAuthorization never re-prompts a no).
        if !alerts.authorized {
            await alerts.requestAuthorization()
        }
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
