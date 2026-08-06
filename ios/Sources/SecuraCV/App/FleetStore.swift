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
    /// What actually needed you, and what we managed to do about it — the
    /// list the Alerts tab is a list OF.
    let alertLog = AlertLedger()
    /// Set when a wake carried the news here instead of the LAN, so the next
    /// evaluation can record the honest delivery. Cleared once spent.
    private var lastAwayWake: WakeClass?

    private var refreshTask: Task<Void, Never>?
    private var bag = Set<AnyCancellable>()

    /// Durable per-witness mutes, re-applied at every fold (rows are rebuilt
    /// from device truth, so mute must outlive them).
    private let muteLedger = MuteLedger()

    // The living canary — the display firmware's mood engine, mirrored
    // (Shared/CanaryMood.swift), fed the fleet's truth every refresh. The
    // published face/posture drive the character on every surface; the mood
    // LINE is ambient copy only (the Voice rule: it may rephrase
    // contentment or name who's being looked for, never word an alarm).
    private let moodKeeper = CanaryMoodKeeper()
    @Published private(set) var canaryFace: CanaryFace = .calm
    @Published private(set) var canaryPosture: CanaryPosture = .asFace
    @Published private(set) var canaryAnxiety: Int = 0
    @Published private(set) var canaryTrustDays: Int = 0
    @Published private(set) var moodLine: String?
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

        // The news-dedupe ledgers are rebuilt from the persisted history, so
        // an alarm that outlives a relaunch stays ONE alert: still on the
        // tab, still open, but never re-posted as if the app had just found
        // it. (In-memory-only dedupe re-notified every ongoing condition on
        // every cold start — restart spam, the one leak the per-refresh
        // guards couldn't see.)
        (postedAlerts, ackedAlerts) = AlertLedger.foldOpenAlerts(records: alertLog.records)

        // Forward every collaborator's change into ours, so any view observing
        // the store updates when discovery, BLE, alerts, heartbeat, or the
        // device list change — no view has to know the internal object graph.
        for child in [devices.objectWillChange, discovery.objectWillChange,
                      ble.objectWillChange, alerts.objectWillChange,
                      heartbeat.objectWillChange, alertLog.objectWillChange] {
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
        // The away path, if the user armed it. Enabling is idempotent and
        // never prompts — it only asks iOS for a push token and makes sure
        // the iCloud subscription exists, so a reinstall or a new iPhone
        // re-arms itself without the user hunting for a switch.
        if AlertRule.anyReachesAnywhere(rules: alerts.rules) {
            Task {
                await AwayPush.shared.enable()
                await AwayPush.shared.sweepOldWakes()
            }
        }
        recordDemoBeatIfHarmless()
        // History hygiene at the door: settled, seen rows past the retention
        // window leave; the badge tells the truth about what's still unseen.
        alertLog.retentionSweep()
        syncBadge()
        startRefreshLoop()
        startSentinel()
        pushLiveActivity()
    }

    /// A wake reached this device from off-network. The notification the user
    /// sees was already composed by the NSE; this makes the APP true — pull
    /// the fleet so opening it shows current reality, and record in the
    /// history that the away path is what carried it.
    func noteAwayWake(_ wake: WakeClass?) {
        lastAwayWake = wake
        Task { await refreshOnce() }
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
        // Availability first: every CloudSync call gates on it, so skipping
        // this makes the whole private-DB sync a no-op (it silently was one
        // until the flag learned to set itself).
        await CloudSync.shared.refreshAvailability()
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

    /// Feed the mood engine the fleet's truth and publish the character's
    /// state. Runs at the top of EVERY republish path (refresh, sentinel,
    /// mute, ack), so no surface can ever receive a stale face beside fresh
    /// severity — a lost Canary changes the bird in the same push that
    /// changes the numbers.
    private func updateCanaryMood() {
        // Alarm-unacked reads the PRE-MUTE truth: mute caps nagging, it does
        // not acknowledge a live alarm — the bird must not come back for a
        // condition the user has only silenced.
        let alarming = witnesses.filter { $0.displaySeverity >= .alert }
        let allAcked = !alarming.isEmpty && alarming.allSatisfy {
            ackedAlerts[$0.id] == alertFingerprint($0)
        }
        let inputs = CanaryMoodInputs(fleet: witnesses,
                                      alarmUnacked: !alarming.isEmpty && !allAcked)
        let reading = moodKeeper.observe(inputs)
        canaryFace = reading.face
        canaryPosture = reading.posture
        canaryAnxiety = reading.state.anxiety
        canaryTrustDays = reading.state.trustDays
        moodLine = composeMoodLine(reading: reading)
    }

    /// Ambient copy only, and every line names log-able state (the honesty
    /// rule): who is being looked for, how many things need care, how many
    /// clean days the streak holds.
    private func composeMoodLine(
        reading: (face: CanaryFace, posture: CanaryPosture, state: CanaryMoodState, milestone: Bool)
    ) -> String? {
        switch reading.face {
        case .hidden, .asleep:
            return nil   // the instruments own the stage
        case .calm:
            if reading.milestone {
                return reading.state.trustDays >= 30 ? "A clean month together"
                                                     : "A clean week together"
            }
            if reading.state.trustDays >= 7 {
                return "\(reading.state.trustDays) clean days together"
            }
            return "Watching with you"
        case .worried:
            switch reading.posture {
            case .searching:
                if let late = witnesses.first(where: { $0.link == .stale }) {
                    return "Looking for \(late.displayName)…"
                }
                return "Looking for a quiet Canary…"
            case .calling:
                if let lost = witnesses.first(where: { $0.link.isDark }) {
                    return "Calling for \(lost.displayName)"
                }
                return "Calling for a lost Canary"
            case .asFace:
                return "Something feels off"
            }
        case .distressed:
            return "Feeling rough — the fleet needs care"
        }
    }

    /// The last worst-severity this store produced feedback against — the
    /// serialization point for the one-buzz-per-crossing promise.
    private var lastFeltWorst: Severity = .ok

    /// Island + wrist + iPhone widgets, in one place — every mutation path
    /// (poll, sentinel, mute, ack) fans out identically, and the mood folds
    /// FIRST so the character and the numbers always ship together.
    private func republishGlanceSurfaces() {
        updateCanaryMood()
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
        alertLog.mark(.muted, forWitness: id)
        applyMutesAndRepublish()
    }

    func unmute(_ id: String) {
        muteLedger.clear(id)
        applyMutesAndRepublish()
    }

    /// The Quiet Hour verb (Siri / Shortcuts / the Action button): every
    /// real witness muted at once. Demo rows are skipped — sample data must
    /// never inflate the honest count — and tamper still punches through per
    /// row (Witness.effectiveSeverity owns that guarantee; this path cannot
    /// weaken it). Returns how many were quieted, for the spoken dialog.
    @discardableResult
    func quietFleet(for duration: TimeInterval = 3600) -> Int {
        let ids = witnesses.map(\.id).filter { !$0.hasPrefix(DemoFleet.idPrefix) }
        for id in ids { mute(id, for: duration) }
        return ids.count
    }

    /// The symmetric verb: clear every active mute, so a quiet hour is never
    /// a trap someone forgets they set. Returns how many mutes it ended.
    @discardableResult
    func resumeFleet() -> Int {
        let ids = muteLedger.activeMutes()
        for id in ids { unmute(id) }
        return ids.count
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

    /// Turn a refused delivery into a sentence the Alerts tab can show. The
    /// thrown reason is a fragment ("notifications are off for SecuraCV"), so
    /// it gets a capital and a period and nothing else — the user reads the
    /// system's real objection, not our paraphrase of it.
    private static func deliveryFailure(_ error: Error) -> String {
        let reason = error.localizedDescription
        guard let first = reason.first else { return "iOS refused to show it." }
        let sentence = first.uppercased() + reason.dropFirst()
        return sentence.hasSuffix(".") ? sentence : sentence + "."
    }

    private func alertFingerprint(_ w: Witness) -> String {
        // Pre-mute severity on purpose: for every witness that can enter the
        // alert loop the two severities coincide (mute caps below alert, and
        // the punch-through cases are uncapped), and the mood engine's
        // alarm-unacked rule must match acks against the LIVE condition.
        "\(w.displaySeverity.rawValue)|\(w.statusLine)"
    }

    /// The Ack action's landing pad: remember WHAT was acknowledged, so the
    /// same ongoing condition stays quiet but any change breaks through.
    func acknowledgeAlert(for id: String) {
        if let w = witnesses.first(where: { $0.id == id }) {
            ackedAlerts[id] = alertFingerprint(w)
        } else if let posted = postedAlerts[id] {
            ackedAlerts[id] = posted
        }
        alertLog.mark(.acknowledged, forWitness: id)
        syncBadge()
        // An ack changes the story (the bird may return to the stage) —
        // every surface hears about it now, not at the next poll.
        republishGlanceSurfaces()
    }

    /// The user left the Alerts tab: everything it listed is now "seen".
    /// Seen is about the badge and the unseen dots, never about handling —
    /// glancing at a live alarm does not acknowledge it.
    func markAlertsSeen() {
        alertLog.markSeen()
        syncBadge()
    }

    /// "Clear history" — settled rows only (the ledger keeps anything that
    /// still needs a human), and the badge is retold the truth immediately.
    func clearAlertHistory() {
        alertLog.clearSettled()
        syncBadge()
    }

    /// The app badge is the unseen count — "something landed that you have
    /// not looked at" — kept in one place so every path that changes the
    /// ledger leaves the icon telling the truth.
    private func syncBadge() {
        alerts.setBadge(alertLog.unseenCount)
    }

    /// Is this iPhone home right now?
    ///
    /// Answered by the fleet's own reachability rather than by asking iOS
    /// about the network: a device that is hearing a Canary *is* on the home
    /// network, and no SSID read, location permission, or geofence is needed
    /// to know it. That is the same inference the wake publisher has always
    /// relied on — this just stops the rest of the alert path pretending the
    /// answer is always "home".
    ///
    /// `.online` specifically, not merely "not dark": a stale witness means
    /// we heard from it a while ago, which is exactly the state a phone
    /// passes through on its way out the door.
    private var seesFleet: Bool {
        witnesses.contains { $0.link == .online }
    }

    private func evaluateAlerts() {
        // Whether this device is away decides two things below: which rules
        // may speak at all (a rule armed "on Wi-Fi only" must not fire from
        // across town), and whether darkness is reportable by this device.
        let awayFromHome = !seesFleet

        // Two deliveries, one decision. On the LAN we post locally; if this
        // device can see the fleet it is HOME, so it is also the one that
        // publishes the content-free wake that reaches the user's other
        // devices across town. Both outcomes are written into the history —
        // the tab's whole job is answering "did this actually reach me?", and
        // it can only answer honestly if we record what really happened.
        var live = Set<String>()
        for w in witnesses where w.effectiveSeverity >= .alert {
            live.insert(w.id)
            let fingerprint = alertFingerprint(w)
            guard postedAlerts[w.id] != fingerprint else { continue }   // already told
            guard ackedAlerts[w.id] != fingerprint else { continue }    // user said "seen it"

            // A phone that has left home cannot tell a Canary that DIED from
            // one it simply can no longer reach — both arrive as silence.
            // Reporting the second as the first is the notification storm
            // every owner gets on the drive to work, and it is the fastest
            // way to teach someone to ignore this app. So an away phone stays
            // quiet about darkness alone, and keeps speaking for everything
            // it can still genuinely observe (tamper the device itself
            // reported, a failed signature). Nothing is marked as told, so
            // the condition is reported properly on arriving home.
            //
            // The authority on darkness is whatever stayed home: an Apple TV
            // showing the Wall with "stand watch" on (ResidentWatch), which
            // publishes the offline wake from the LAN where the question can
            // actually be answered.
            if AlertCenter.unknowableFromAway(
                awayFromHome: awayFromHome, isDark: w.link.isDark, tamper: w.tamper) {
                continue
            }
            if postedAlerts[w.id] != nil {
                // The condition CHANGED without a calm gap (dark became
                // tamper): the old record's story is over even though the
                // witness never left the live set — close it, or it sits
                // "Ongoing" forever, exempt from retention. The new
                // condition gets its own record just below.
                alertLog.resolve(witnessID: w.id)
            }
            postedAlerts[w.id] = fingerprint

            // The ledger key must carry the witness: `alertFingerprint` is
            // only ever used keyed BY id in the ledgers above, so two Canaries
            // both reading "Gone dark" produce the same fingerprint — as a
            // global record id that would collapse them into one row wearing
            // whichever name arrived first.
            let record = alertLog.note(id: "\(w.id)|\(fingerprint)", witnessID: w.id,
                                       name: w.displayName, severity: w.effectiveSeverity,
                                       headline: w.statusLine)
            if let level = alerts.level(for: w.effectiveSeverity, awayFromHome: awayFromHome) {
                // CONFIRM, never assume. `level` says a rule wants to tell
                // them; it says nothing about whether iOS will actually show
                // it. postConfirmed checks authorization and awaits the
                // system's acceptance, so a denied-permission phone records
                // "Not delivered — Notifications are off" instead of a
                // comforting lie. This tab exists to be trusted about exactly
                // this; recording an unverified success would make it the
                // same kind of overstatement the PR set out to remove.
                let recordID = record.id
                let body = "\(w.displayName): \(w.statusLine)"
                let arrivedAway = lastAwayWake != nil
                Task { [weak self] in
                    guard let self else { return }
                    do {
                        try await self.alerts.postConfirmed(title: self.fleetName, body: body,
                                                            level: level, threadID: w.id)
                        // A wake that already reached the pocket outranks a
                        // local post; the ledger only moves delivery up.
                        self.alertLog.markDelivery(arrivedAway ? .away : .onLAN, for: recordID)
                    } catch {
                        self.alertLog.markDelivery(.notDelivered, for: recordID,
                                                   reason: Self.deliveryFailure(error))
                    }
                }
            } else if !alerts.hasArmedRule(for: w.effectiveSeverity) {
                alertLog.markDelivery(.notDelivered, for: record.id,
                                      reason: "No armed rule covers this.")
            } else if FocusGate.criticalOnly() {
                // Order matters: blaming Focus for something no rule was
                // watching would send the user to fix the wrong setting.
                alertLog.markDelivery(.notDelivered, for: record.id,
                                      reason: "Your Focus is set to life-safety only.")
            } else {
                alertLog.markDelivery(.notDelivered, for: record.id,
                                      reason: "This level never pushes.")
            }
            // Reaching the user's OTHER devices is a separate question from
            // reaching this one — but it is still the SAME rule's question.
            // Gate on the rule that actually matches this witness, so a
            // condition whose rule says "On Wi-Fi only" never leaves the
            // house just because some unrelated rule wants away reach.
            if alerts.reachesAnywhere(severity: w.effectiveSeverity), lastAwayWake == nil {
                AwayPush.shared.publishWake(WakeClass(witness: w))
            }
        }
        lastAwayWake = nil
        // Calmed witnesses leave both ledgers — their NEXT alert is news —
        // and the history closes its loop in the same breath: the moment a
        // condition would be news again is exactly the moment its old record
        // must stop reading as "still happening".
        for id in postedAlerts.keys where !live.contains(id) {
            alertLog.resolve(witnessID: id)
        }
        postedAlerts = postedAlerts.filter { live.contains($0.key) }
        ackedAlerts = ackedAlerts.filter { live.contains($0.key) }
        syncBadge()
    }

    private func pushLiveActivity() {
        let ago: Int? = heartbeat.lastVerified.map { Int(Date().timeIntervalSince($0)) }
        let state = FleetActivityAttributes.State(fleet: witnesses, lastVerifiedAgo: ago)
        // The island is an episode, not wallpaper (IslandPolicy): it exists
        // only while something is live — a condition at warn or above, the
        // dead-man's-switch talking, or a path test the user just started —
        // and leaves the stage (with a short all-clear linger) when the
        // episode resolves. A quiet fleet on an ordinary day puts nothing in
        // the status bar; the widgets are the always-there glance.
        if IslandPolicy.shouldShow(worstSeverity: worstSeverity,
                                   heartbeat: heartbeat.wristState) {
            LiveActivityController.shared.start(fleetName: fleetName, state: state)
            Task { await LiveActivityController.shared.update(state) }
        } else {
            Task { await LiveActivityController.shared.endEpisode(with: state) }
        }
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
            // the user SEES this half of the path light up — and CONFIRM the
            // system accepted it. Notifications off → this throws → the
            // heartbeat honestly records a FAILED path instead of a hollow
            // "verified".
            //
            // The copy says exactly what this proved and no more. It travels
            // no network, so it cannot speak for the away path: that drill is
            // `alert_relay --send-test` on the hub, which sends a real poke
            // over the real topic to the phone in your pocket. Claiming
            // "your fleet can reach you" from a notification the phone posted
            // to itself is the kind of comfortable lie this project exists to
            // not tell.
            try await alerts.postConfirmed(
                title: fleet,
                body: "Notifications work on this iPhone. (Away alerts are a separate test — run it from the hub.)",
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
