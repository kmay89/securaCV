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
    /// A tap outside the app (notification, widget, link) that wants a place
    /// inside it. The shell switches tabs on it; the destination consumes
    /// and CLEARS it, so a stale route can never re-fire on a later visit.
    @Published var pendingRoute: AppRoute?
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

    /// Per-witness reach ("this one only when it's serious") — durable for
    /// the same reason mutes are: the rows it applies to are rebuilt every
    /// refresh.
    private let witnessPrefs = WitnessAlertPrefs()

    /// The local, never-uploaded answer counters behind the "you dismiss
    /// almost all of these" offer (Invariant IV — two integers per severity,
    /// on this phone, forever).
    private let tuning = AlertTuningLedger()

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
        alerts.onMute = { [weak self] id, duration in self?.mute(id, duration: duration) }
        alerts.onAck = { [weak self] id in self?.acknowledgeAlert(for: id) }
        // The plain tap on a notification: route to the alert it was about.
        // The thread id is a witness id for witness alerts and a summary
        // marker for storms; the Alerts tab anchors when it matches a record
        // and simply shows the list when not — an anchor hint, never a
        // capability.
        alerts.onOpen = { [weak self] threadID in
            self?.pendingRoute = .alerts(witnessID: threadID)
        }

        // Tamper heard over a BLE NOTIFY is the fastest signal the fleet
        // has — sub-second, no polling anywhere in the path. Re-fold and
        // re-evaluate NOW; waiting out the 20-second cycle would squander
        // the one transport that pushes.
        ble.onUrgentSnapshot = { [weak self] _ in
            Task { await self?.refreshOnce() }
        }
        // The connectionless twin: a tamper or alert CHIRP used to wait out
        // the 20-second cycle the GATT NOTIFY path above bypasses — the one
        // signal built for when Wi-Fi and broker are down was the slowest
        // thing in the app. Same answer: re-fold NOW.
        ble.onUrgentChirp = { [weak self] _ in
            Task { await self?.refreshOnce() }
        }

        // Someone who installs this app ONLY to help watch a relative's fleet
        // pairs nothing, so the launch-time "you have devices, let's ask about
        // notifications" moment never comes for them — and their household
        // alerts would be suppressed while the owner's screen said they were
        // being told. Hand the participant path the app's one authorization
        // request so it can ask at the moment they accept.
        HouseholdShare.shared.requestNotificationAuthorization = { [weak self] in
            guard let self else { return false }
            if !self.alerts.authorized { await self.alerts.requestAuthorization() }
            return self.alerts.authorized
        }

        // The news-dedupe ledgers are rebuilt from the persisted history, so
        // an alarm that outlives a relaunch stays ONE alert: still on the
        // tab, still open, but never re-posted as if the app had just found
        // it. (In-memory-only dedupe re-notified every ongoing condition on
        // every cold start — restart spam, the one leak the per-refresh
        // guards couldn't see.)
        (postedAlerts, ackedAlerts) = AlertLedger.foldOpenAlerts(records: alertLog.records)

        // Started unconditionally, and it is NOT discovery: NWPathMonitor
        // reports this phone's own link type and nothing about the network it
        // is on — no scanning, no SSID, no permission, nothing to consent to.
        // It runs from launch because `sawFleetOnThisNetwork` is a claim about
        // the current attachment, and starting it late would begin every
        // session unable to tell a blackout from a guest network.
        NetworkVantage.shared.start()

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
        // The household legs, re-armed the same way and for the same reason.
        // Both directions have to be asked rather than remembered: a device
        // that ACCEPTED an invitation must re-subscribe after a reinstall,
        // and an owner only ever learns that somebody joined — or left — by
        // looking. Nothing here creates a share; a household nobody was
        // invited to stays a zone that was never made.
        Task {
            await HouseholdShare.shared.refreshParticipation()
            await HouseholdShare.shared.refreshMembers()
            await HouseholdShare.shared.sweepOldEscalations()
            // The answered markers get swept unconditionally, unlike the
            // escalations: they live in the owner's own private database where
            // nobody is subscribed, so deleting one cannot push anything at
            // anyone, and leaving them would accumulate a timestamped record of
            // when the owner was awake to answer an alarm.
            await HouseholdShare.shared.sweepOldAnswered()
        }
        recordDemoBeatIfHarmless()
        // The dead-man's-switch may only count silence it could have heard:
        // the app hears nothing in the background by design, so the window
        // restarts here rather than accusing the fleet of going dark while
        // the phone was asleep in a pocket.
        heartbeat.noteListening()
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
            heartbeat.recordDemoBeat()
        } else {
            revokeDemoBeat()
        }
    }

    /// Back to "Not yet verified" — but only if the beat was demo-fed; a real
    /// confirmation is never discarded. The flag lives on the heartbeat (and
    /// is never persisted) so this stays true across a relaunch: a demo beat
    /// that outlived the app would be a stage prop nothing could revoke.
    private func revokeDemoBeat() {
        if heartbeat.isDemoFed { heartbeat.reset() }
    }

    func onScenePhase(active: Bool) {
        if active {
            startRadiosIfConsented()
            startSentinel()
            // Listening again — see onAppear. Backgrounded time is not
            // evidence of a silent fleet.
            heartbeat.noteListening()
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
            .filter { !pairedIDs.contains($0.deviceID) }
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
                    // The self-report carries no id, so the display name is
                    // the only key — and names are NOT unique (Discovery
                    // documents that every unit flashed from one config seeds
                    // the same one). Fold only when the name picks out
                    // exactly ONE row; on ambiguity, stand alone rather than
                    // decorate the wrong Canary — the same conservative rule
                    // FleetMerge.attach applies to two-byte beacon suffixes.
                    let matches = next.indices.filter {
                        next[$0].name == row.name && !row.name.isEmpty
                    }
                    if matches.count == 1 {
                        FleetMerge.fold(row, into: &next[matches[0]])
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
        // And the chirps — the momentary broadcast alerts that replace a
        // sender's beacon on air for 2 s. Same conservative attach; a tamper
        // chirp raises the row's level through the existing alert ledger,
        // which already caps repeats at one alert per condition.
        FleetMerge.attach(chirps: ble.freshChirps, to: &next)

        // Demo fleet: seeded witnesses/events join anything real (ids are
        // "demo-"-namespaced, so they can't collide) — a live Canary paired
        // mid-demo still shows up beside the samples. The heartbeat is only
        // demo-fed while nothing real is paired (see recordDemoBeatIfHarmless).
        if demoMode {
            next.append(contentsOf: DemoFleet.witnesses())
            events.append(contentsOf: DemoFleet.timeline())
            recordDemoBeatIfHarmless()
        }

        // What each row IS, from the mDNS advert, for any row that didn't
        // learn it from the transport that built it. A paired device is
        // polled over /api/v1, which carries no product string and no board
        // id at all — so without this a paired Canary would be the one kind
        // that never draws its own picture, which is exactly backwards.
        //
        // Fill gaps, never replace: a stronger tier that already said what
        // this device is outranks an advert.
        for i in next.indices {
            guard let advert = discovery.found.first(where: { $0.deviceID == next[i].id })
            else { continue }
            if next[i].publishedType == nil, !advert.publishedType.isEmpty {
                next[i].publishedType = advert.publishedType
            }
            if next[i].hardware == nil, !advert.hardware.isEmpty {
                next[i].hardware = advert.hardware
            }
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
        // The all-clear earns a line. A condition ending is news the same way
        // it starting was — silence must never be the only "it's fine" signal
        // (status-page doctrine). Derived from the ledger every fold, so it
        // can't drift from what the Alerts tab says, and digest-tier by
        // construction: there is no path from here to a notification.
        events.append(contentsOf: AlertFreshness.allClearEvents(alertLog.records))
        timeline = events.sorted { $0.timeBucket > $1.timeBucket }
        // The fleet's own liveness feeds the heartbeat: a Canary answering is
        // a real check-in (and says exactly that — never "delivery
        // verified", which only an accepted notification earns). With nothing
        // paired there is no beat to miss, so the dead-man's-switch stays
        // quiet instead of alarming about a fleet the user hasn't got.
        heartbeat.expectsBeats = !devices.devices.isEmpty
        if FleetBeat.heard(in: witnesses, demoPrefix: DemoFleet.idPrefix) {
            heartbeat.recordBeat(source: .fleetCheckIn)
        }
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
    /// Per-device witness-chain heads, watched by the sentinel — the moment
    /// a head moves, the news gets a full refresh NOW instead of waiting
    /// out the 20-second cycle (HeadWatch, host-tested).
    private var headWatch = HeadWatch()

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
        if changed {
            witnesses = rows.sorted { $0.effectiveSeverity > $1.effectiveSeverity }
            republishGlanceSurfaces()
            evaluateAlerts()
            commitFeltTransition()
        }

        // ── The event head-watch: the same 5-second pass that asks "is it
        // alive?" also asks "did anything HAPPEN?" — one chain record's
        // worth per answering device. The moment any head moves, the full
        // refresh runs now, so a new event reaches the alert loop in ~5 s
        // on the LAN instead of riding out the 20-second cycle. This is
        // where "faster than the cloud cameras" is actually earned.
        let liveRefs = devices.devices.filter {
            $0.deviceType.isHTTPPairable && answers[$0.id] == true
        }
        guard !liveRefs.isEmpty else { return }
        let heads = await withTaskGroup(of: (String, UInt64)?.self) { group -> [(String, UInt64)] in
            for ref in liveRefs {
                guard let api = try? devices.api(for: ref) else { continue }
                group.addTask {
                    // `try?` flattens the optional (SE-0230): nil here is a
                    // failed read OR an empty chain — neither is news.
                    guard let head = try? await api.witnessHeadSeq() else { return nil }
                    return (ref.id, head)
                }
            }
            var out: [(String, UInt64)] = []
            for await h in group { if let h { out.append(h) } }
            return out
        }
        var news = false
        for (id, seq) in heads where headWatch.hasNews(id: id, headSeq: UInt32(clamping: seq)) {
            news = true
        }
        if news { await refreshOnce() }
    }

    // MARK: - mute (the ledger is the truth; rows are its projection)

    /// Mute one witness — from the detail screen, a notification action, or
    /// the wrist. Caps nagging at Notice; tamper and a failed signature
    /// still punch through (Witness.effectiveSeverity owns that guarantee).
    func mute(_ id: String, for duration: TimeInterval = 3600) {
        quiet(id, until: Date().addingTimeInterval(duration))
    }

    /// The named durations (1 hour / until tonight / until morning) — the
    /// same three on the phone, the wrist, and anywhere else that grows a
    /// mute button. Every one of them ends; MuteDuration has no untimed case
    /// to choose, which is the guarantee rather than a habit.
    func mute(_ id: String, duration: MuteDuration, now: Date = Date()) {
        quiet(id, until: duration.expiry(from: now))
    }

    private func quiet(_ id: String, until: Date) {
        // A mute on the storm summary means "quiet all of them" — same
        // fan-out as the storm Ack, and every guarantee still holds per
        // witness (tamper punch-through, the mute's own expiry).
        if id == AlertStorm.threadID {
            for witnessID in stormWitnessIDs { quiet(witnessID, until: until) }
            return
        }
        // "Stop telling me about this" is an answer, and the local counters
        // hear it as one — a class the user mutes over and over is a class
        // the app should offer to stop pushing (AlertTuning).
        for record in alertLog.liveRecords(forWitness: id) {
            tuning.recordDismissed(record.severity)
        }
        muteLedger.set(until: until, for: id)
        alertLog.mark(.muted, forWitness: id, until: until)
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
        // The dialect this repo's firmware actually serves, tried FIRST: a
        // WAP answers /api/status and /api/events/today, and no in-repo
        // firmware answers /api/v1/info — so a poll that led with v1 marked
        // every real WAP dark forever, events and all. The v1 body below
        // stays intact as the fallback for the reference device-api
        // (canary-vision/docs/api.md), same both-dialects spirit as
        // DeviceAPI.authorize(_:).
        if let status = try? await api.wapStatus() {
            return await pollWAP(ref, api: api, status: status)
        }
        guard let info = try? await api.info() else {
            var w = Witness(id: ref.id, deviceType: ref.deviceType, name: ref.name)
            w.link = .lost                       // couldn't reach it → dark
            return (w, [])
        }
        var w = Witness(id: info.deviceID, deviceType: ref.deviceType, name: info.name)
        // TOFU: on FIRST sight of a device with no pinned key, fetch its
        // public key and pin it — the pairing receipt deliberately carries no
        // key, so this poll is where trust-on-first-use actually happens.
        // pin() never overwrites: once pinned, the key is only ever READ, and
        // a device that starts presenting a different key keeps failing the
        // verification below (badge .failed → alert-severity, mute-proof) —
        // the loud path a changed key must take.
        if PinnedKeyStore.key(for: ref.id) == nil,
           let pub = try? await api.publicKey() {
            PinnedKeyStore.pin(pub, for: ref.id)
        }
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

        // Same attributed self-row fold as the WAP path — and this v1 path
        // is the one a camera-line device would answer on, so it is where
        // an attributed seeing claim would actually first appear.
        if let base = ref.baseURL,
           let report = try? await DeviceAPI.fleetSelfReport(at: base),
           let selfRow = report.devices.first {
            FleetMerge.fold(selfRow, into: &w, attributed: true)
        }

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

    /// Poll one WAP-dialect device: /api/status carried identity + the chain
    /// head (already fetched by the caller); /api/events/today carries the
    /// sensing feed the timeline renders — the wire the DeviceEvent dialect
    /// was built for.
    private static func pollWAP(_ ref: PairedDeviceRef, api: DeviceAPI,
                                status: WapStatus) async -> (Witness, [TimelineEvent])? {
        var w = Witness(id: ref.id, deviceType: ref.deviceType, name: ref.name)
        // TOFU: same rule and same moment as the v1 path — first sight with
        // no pinned key fetches and pins; pin() never overwrites.
        if PinnedKeyStore.key(for: ref.id) == nil,
           let pub = try? await api.publicKey() {
            PinnedKeyStore.pin(pub, for: ref.id)
        }
        // The trusted fingerprint is DERIVED from the pinned key, never read
        // off the wire: /api/status carries a self-claimed "fingerprint",
        // and a claim is not a derivation (the FleetMerge attribution rule).
        if let pub = PinnedKeyStore.key(for: ref.id),
           let fp = DeviceFingerprint.hex(forPublicKey: pub) {
            w.fingerprint = fp
        }
        w.firmware = status.firmware ?? ""
        w.link = .online
        w.lastSeen = Date()
        w.baseURL = ref.baseURL
        w.chainLength = UInt32(clamping: status.chainSeq ?? 0)

        // The device's own /api/fleet self-report, folded ATTRIBUTED: this
        // poll is the device's own address, so its self row may carry the
        // claims the name-matched path must refuse — the seeing class above
        // all (FleetMerge's rule). Today a WAP's self row adds its hub
        // standing, board id and birth day, which /api/status never carried;
        // the day a camera-line firmware serves the endpoint, the class
        // signals light up here with no further app change.
        if let base = ref.baseURL,
           let report = try? await DeviceAPI.fleetSelfReport(at: base),
           let selfRow = report.devices.first {
            FleetMerge.fold(selfRow, into: &w, attributed: true)
        }

        // The event feed: unsigned ring rows, newest first. Dismissed rows
        // were handled at the device; ambient rows are room-sense chatter
        // the device's own dashboard graphs — the phone timeline carries
        // named occurrences only (Wire/WapEvents.swift). TrustBadge stays
        // .unknown throughout: nothing on this wire is Ed25519-checked
        // against the pinned key, and "verified" means exactly that check.
        let feed = (try? await api.wapEventsToday()) ?? WapEventsToday()
        let rows = feed.events
            .filter { !$0.isDismissed && $0.isNamedOccurrence }
        let dates = WapEventRow.anchoredDates(for: rows, fetchedAt: Date())
        let events = zip(rows, dates).map { row, date in
            TimelineEvent(id: "\(ref.id)#e\(row.id)",
                          deviceID: ref.id, deviceName: ref.name, zone: "",
                          headline: EventVocabulary.headline(forWire: row.type,
                                                             state: row.state,
                                                             zone: "", deviceName: ref.name),
                          severity: EventVocabulary.severity(forWire: row.type),
                          badge: .unknown,
                          timeBucket: date,
                          symbol: EventVocabulary.sfSymbol(forWire: row.type))
        }
        // The row's story — and its live level only when the device itself
        // claims "now". WapEventRow.liveSeverity encodes the rule the first
        // client's review settled: a CLOSED ring row is history (it can sit
        // "newest" for hours, so it caps at the calm tick and can never
        // latch the fleet red), while an OPEN bundle is the device's own
        // present tense — its true severity speaks, and the latch
        // self-clears the moment the bundle closes and the flag drops.
        // Open rows are serialized ahead of the ring, so the worst open
        // row decides; the timeline keeps every row's true severity either
        // way, so history stays honestly colored.
        if let head = rows.first, let headDate = dates.first {
            w.lastEvent = EventVocabulary.headline(forWire: head.type,
                                                   state: head.state,
                                                   zone: "", deviceName: ref.name)
            w.lastEventAt = headDate
            w.lastEventSeverity = rows.filter(\.isOpen)
                .map(\.liveSeverity)
                .max() ?? head.liveSeverity
        }
        // The tamper story, narrated per kind (system.integrity). Tamper
        // rows are sealed-and-closed the moment they commit (durability
        // over bundling — a power-loss record cannot wait out a RAM
        // buffer), so "still standing" is no longer readable off an open
        // bundle: the wire says it outright in the envelope's tamper.kind
        // (boot kinds stand for the boot, SD kinds clear on recovery,
        // absent = nothing to confess). The flag stays level-triggered and
        // self-clears, because this witness is rebuilt on every poll. An
        // OPEN tamper bundle — should a future firmware hold one — still
        // latches; a CLOSED row is history and must not (record vs siren,
        // the same rule liveSeverity encodes). An unknown kind word
        // narrates as the bare "Tamper detected" — calm default, never a
        // guess.
        if let standing = feed.tamper, !standing.kind.isEmpty {
            w.tamper = true
            w.tamperKind = TamperKind(wire: standing.kind)?.narration ?? ""
        } else if let live = rows.first(where: { $0.isOpen && $0.type == "tamper" }) {
            w.tamper = true
            w.tamperKind = TamperKind(wire: live.state)?.narration ?? ""
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

    /// Each witness's buzz-burst so far — the repeat governor's memory (the
    /// dog-at-the-door damper). Deliberately NOT cleared when a condition
    /// calms: the governor's own calm gap decides when a burst is over,
    /// because "the dog left the porch for ninety seconds" must not reset
    /// the rest the repeats already earned. In-memory only; a relaunch
    /// forgetting a burst costs at most one extra buzz, which is the honest
    /// direction to fail.
    private var repeatMemory: [String: RepeatGovernor.Memory] = [:]

    /// The witnesses the LAST storm summary spoke for. A storm notification
    /// carries the synthetic `fleet-storm` thread, so its Ack / Mute actions
    /// arrive naming no witness at all — this list is how those taps reach
    /// every Canary the summary covered instead of silently doing nothing
    /// (which would leave the records unacknowledged and eligible for
    /// escalation the user believes they answered).
    private var stormWitnessIDs: [String] = []

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
    ///
    /// The storm summary's thread is synthetic, so its Ack fans out to every
    /// witness the summary spoke for — answering "4 Canaries need attention"
    /// answers all four, exactly what the tap meant.
    func acknowledgeAlert(for id: String) {
        if id == AlertStorm.threadID {
            for witnessID in stormWitnessIDs { acknowledgeAlert(for: witnessID) }
            return
        }
        if let w = witnesses.first(where: { $0.id == id }) {
            ackedAlerts[id] = alertFingerprint(w)
        } else if let posted = postedAlerts[id] {
            ackedAlerts[id] = posted
        }
        // An ack is the counters' "this mattered" — the other half of the
        // evidence behind the demotion offer.
        for record in alertLog.liveRecords(forWitness: id) {
            tuning.recordActed(record.severity)
        }
        // Tell the owner's OTHER devices, before marking, that this one is
        // dealt with. Without it, acknowledging here leaves an iPad's
        // escalation timer running on a stale belief, and a household member
        // gets "Nobody answered" about an alarm that was answered — the
        // cry-wolf failure on the one channel whose value is staying quiet.
        // Keyed by the occurrence, exactly like the escalation it guards, so
        // both sides compute the same name with nothing to sync.
        //
        // ONLY for alarms that could actually reach the household. A marker
        // for a Notice can never suppress anything (nothing below the top tier
        // is ever escalated), so writing one buys nothing and costs a record
        // in iCloud carrying a precise creation time we don't control — an
        // "answered at 03:14" trail of exactly the kind this project coarsens
        // everywhere else. The cheapest privacy is the write you don't make.
        let integrityFailed = witnesses.first(where: { $0.id == id })?.badge == .failed
        for record in alertLog.liveRecords(forWitness: id)
        where EscalationPolicy.isTopTier(severity: record.severity,
                                         integrityFailed: integrityFailed) {
            HouseholdShare.shared.noteAnswered(
                occurrenceKey: HouseholdRelay.occurrenceRecordName(recordID: record.id,
                                                                  alarmBucket: record.lastBucket))
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

    /// The device-detail door: reading one Canary's history on its own
    /// screen clears that Canary's dots and its share of the badge — and
    /// nobody else's, because only its rows were on screen. Same
    /// seen-not-handled semantics as the tab.
    func markAlertsSeen(for witnessID: String) {
        alertLog.markSeen(witnessID: witnessID)
        syncBadge()
    }

    /// "Clear history" — settled rows only (the ledger keeps anything that
    /// still needs a human), and the badge is retold the truth immediately.
    func clearAlertHistory() {
        alertLog.clearSettled()
        syncBadge()
    }

    /// The user swiped a settled row away. That is an answer too ("I didn't
    /// need to be told this"), so it counts before the row goes.
    func dismissAlert(id: String) {
        if let record = alertLog.record(id: id) {
            tuning.recordDismissed(record.severity)
        }
        alertLog.remove(id: id)
        syncBadge()
    }

    // MARK: - per-witness reach

    func pushFloor(for id: String) -> WitnessPushFloor {
        witnessPrefs.floor(for: id)
    }

    /// Narrow (never silence) what one Canary may interrupt for. Republished
    /// immediately so the detail screen reflects the choice in the same
    /// breath as the tap.
    func setPushFloor(_ floor: WitnessPushFloor, for id: String) {
        witnessPrefs.set(floor, for: id)
        objectWillChange.send()
    }

    /// How many Canaries the user has narrowed — the rules sheet says so, so
    /// a per-device choice made months ago can never become an invisible
    /// reason alerts aren't arriving.
    var narrowedWitnessCount: Int { witnessPrefs.narrowedIDs().count }

    // MARK: - the app noticing it's being annoying

    /// The one demotion worth offering right now, if any. Recomputed on read:
    /// it is two dictionary lookups over counters that only move when the
    /// user answers something.
    var tuningAdvice: AlertTuning.Advice? {
        AlertTuning.advice(stats: tuning.stats, rules: alerts.rules, declined: tuning.declined)
    }

    /// "Yes, stop pushing these." Turns the rule off — the events still land
    /// in Today and in the history, they simply stop interrupting — and
    /// forgets the evidence, so a rule re-armed later is judged on what
    /// happens next rather than on the history that got it turned off.
    func applyTuning(_ advice: AlertTuning.Advice) {
        // EVERY rule that covers the class, or the button lies: the shipped
        // rules overlap by severity, so disabling one would leave the next
        // one pushing the exact alerts the user just asked to stop.
        for i in alerts.rules.indices where advice.ruleIDs.contains(alerts.rules[i].id) {
            alerts.rules[i].enabled = false
        }
        tuning.forget(advice.severity)
        objectWillChange.send()
    }

    /// "No, keep pushing them." Remembered per CLASS, so the offer is made
    /// once and never becomes its own nag.
    func declineTuning(_ advice: AlertTuning.Advice) {
        tuning.decline(key: advice.id)
        objectWillChange.send()
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

    /// Where this phone is, as well as it can honestly tell.
    ///
    /// `seesFleet` alone is NOT that answer, and the difference is not
    /// academic: darkness is the only thing that makes `seesFleet` false, so
    /// an away-guard fed by it fires precisely when every Canary has gone
    /// quiet — the one-Canary household whose Canary just died, and the power
    /// cut that took the whole house. HomePresence adds the two
    /// permission-free facts that tell those apart from a drive to work.
    private var presence: HomePresence {
        let seeing = seesFleet
        // Latch the vantage while we can still prove it: a Canary answering
        // now is what makes this network's later silence newsworthy.
        if seeing { NetworkVantage.shared.noteFleetSeen() }
        return HomePresence.evaluate(
            seesFleet: seeing,
            onWiFi: NetworkVantage.shared.onWiFi,
            sawFleetOnThisNetwork: NetworkVantage.shared.sawFleetOnThisNetwork)
    }

    private func evaluateAlerts() {
        // Whether this device is away decides two things below: which rules
        // may speak at all (a rule armed "on Wi-Fi only" must not fire from
        // across town), and whether darkness is reportable by this device.
        //
        // The two questions take DIFFERENT answers out of the same presence,
        // and conflating them was the bug. Suppressing a report demands proof
        // we are elsewhere (`.away` only). Deciding an "on Wi-Fi only" rule
        // may speak is the mirror image — it demands proof we are home — so
        // `.unknown` counts as away there and as home nowhere.
        let here = presence
        let awayFromHome = here != .home

        // Two deliveries, one decision. On the LAN we post locally; if this
        // device can see the fleet it is HOME, so it is also the one that
        // publishes the content-free wake that reaches the user's other
        // devices across town. Both outcomes are written into the history —
        // the tab's whole job is answering "did this actually reach me?", and
        // it can only answer honestly if we record what really happened.
        var live = Set<String>()
        // Everything this pass decided to buzz for, gathered before any
        // posting: whether each buzzes alone or the pass collapses into ONE
        // storm summary ("3 Canaries need attention") is a question about
        // the whole set, and it can only be answered after the loop.
        struct PendingPost {
            var recordID: String
            var witness: Witness
            var level: AlertLevel
            var deliveredAway: Bool
        }
        var pendingPosts: [PendingPost] = []
        // Wakes gathered the same way, for the same reason — see the away
        // leg of the storm collapse below.
        var pendingWakes: [Witness] = []
        for w in witnesses where w.effectiveSeverity >= .alert {
            live.insert(w.id)
            let fingerprint = alertFingerprint(w)
            // Already told, or the user said "seen it" — nothing new to post.
            // A top-tier alarm nobody answered is the one exception, and it
            // gets exactly one more chance to be heard (EscalationPolicy).
            if postedAlerts[w.id] == fingerprint || ackedAlerts[w.id] == fingerprint {
                escalateIfUnanswered(w, recordID: "\(w.id)|\(fingerprint)",
                                     awayFromHome: awayFromHome)
                continue
            }

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
                presence: here, isDark: w.link.isDark, tamper: w.tamper) {
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
            // The user's per-witness choice, applied before anything is
            // posted: a Canary narrowed to "serious only" doesn't push its
            // everyday news — but the record above still exists, so the
            // history shows what happened and says whose choice quieted it.
            let floor = witnessPrefs.floor(for: w.id)
            let allowedByWitness = w.effectiveSeverity >= floor.minSeverity
            // The repeat governor speaks before the delivery is scheduled:
            // a burst of same-story repeats from one Canary earns each
            // repeat a longer rest (the dog-at-the-door damper). Tamper and
            // a failed signature are never governed, and a repeat that
            // ESCALATES pierces the rest — RepeatGovernor owns those rules,
            // host-tested. A rested repeat still writes its record above,
            // so the history stays complete; it just doesn't buzz.
            let repeatVerdict = RepeatGovernor.consider(
                severity: w.effectiveSeverity,
                tamper: w.tamper,
                integrityFailed: w.badge == .failed,
                memory: repeatMemory[w.id],
                now: Date())
            // Stored on BOTH verdicts: a rested repeat still advances the
            // burst's seen-clock, which is what keeps a continuous flap
            // from aging into a fake calm gap.
            repeatMemory[w.id] = repeatVerdict.memory
            if allowedByWitness,
               let level = alerts.level(for: w.effectiveSeverity, awayFromHome: awayFromHome) {
                if repeatVerdict.buzz {
                    // Collected, not posted: whether this buzzes alone or
                    // folds into a storm summary is a decision about the
                    // whole pass, made once, after the loop.
                    pendingPosts.append(PendingPost(
                        recordID: record.id, witness: w, level: level,
                        deliveredAway: lastAwayWake != nil || awayFromHome))
                } else {
                    alertLog.markDelivery(.notDelivered, for: record.id,
                                          reason: RepeatGovernor.restingReason(repeatVerdict.restingFor ?? 0))
                }
            } else if !alerts.hasArmedRule(for: w.effectiveSeverity) {
                alertLog.markDelivery(.notDelivered, for: record.id,
                                      reason: "No armed rule covers this.")
            } else if !allowedByWitness {
                // Their own words back: this Canary was narrowed on purpose.
                alertLog.markDelivery(.notDelivered, for: record.id,
                                      reason: floor.undeliveredReason)
            } else if alerts.quietHoursSuppresses(w.effectiveSeverity) {
                // Before Focus, because blaming iOS for a window WE hold
                // would send the user to fix the wrong setting.
                alertLog.markDelivery(.notDelivered, for: record.id,
                                      reason: QuietHours.undeliveredReason)
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
            //
            // Quiet hours gate the wake too, and that is NOT the same call we
            // make for Focus. Focus is enforced by iOS on the device that
            // receives the notification, so publishing and letting the far
            // end decide is correct. Quiet hours are OURS: the notification
            // extension that turns a wake into a banner compiles one shared
            // file and has never heard of this setting, so a wake published
            // now would buzz the user's iPad at 3am no matter what they set.
            // The only place that decision can be honored is here, before it
            // leaves. (Critical is exempt by construction — QuietHours cannot
            // hold it — so a tamper wake still goes out.)
            // The governor's rest binds the wake too: a repeat that isn't
            // worth this phone's buzz isn't worth the iPad's across town —
            // and the tiers that must never rest (tamper, a failed chain)
            // never rest anywhere, because the same verdict covers both.
            // COLLECTED, not published: each wake record fires every
            // subscribed device's push, so a storm publishing per witness
            // would hand the user's other devices the exact multi-buzz
            // pile-up the local collapse exists to prevent. One decision
            // about the whole pass, after the loop.
            if allowedByWitness, repeatVerdict.buzz,
               alerts.reachesAnywhere(severity: w.effectiveSeverity),
               !alerts.quietHoursSuppresses(w.effectiveSeverity), lastAwayWake == nil {
                pendingWakes.append(w)
            }
        }

        // ── The away leg of the storm collapse: at or past the same
        // threshold, ONE wake — carrying the worst class — reaches the
        // user's other devices, mirroring the one summary this phone shows.
        if pendingWakes.count >= AlertStorm.threshold {
            if let worst = pendingWakes.max(by: { $0.effectiveSeverity < $1.effectiveSeverity }) {
                AwayPush.shared.publishWake(WakeClass(witness: worst))
            }
        } else {
            for w in pendingWakes {
                AwayPush.shared.publishWake(WakeClass(witness: w))
            }
        }

        // ── Deliver what the pass gathered: one storm summary, or each on
        // its own. AlertStorm owns the threshold (host-tested); the ledger
        // keeps every per-Canary record either way — the collapse is about
        // how many times a pocket buzzes, never about what the history holds.
        if let storm = AlertStorm.collapse(pendingPosts.map {
            AlertStorm.Pending(name: $0.witness.displayName,
                               severity: $0.witness.effectiveSeverity,
                               statusLine: $0.witness.statusLine)
        }) {
            // The summary inherits the pass's loudest level — a storm with a
            // tamper in it pierces exactly as the tamper alone would have.
            let level = pendingPosts.contains { $0.level == .critical } ? AlertLevel.critical : .important
            let posts = pendingPosts
            // Stamped before the post: the Ack on a storm banner can arrive
            // the moment iOS shows it.
            stormWitnessIDs = posts.map(\.witness.id)
            Task { [weak self] in
                guard let self else { return }
                do {
                    try await self.alerts.postConfirmed(title: self.fleetName, body: storm.body,
                                                        level: level, threadID: AlertStorm.threadID)
                    for p in posts {
                        self.alertLog.markDelivery(p.deliveredAway ? .away : .onLAN, for: p.recordID)
                    }
                    self.heartbeat.recordBeat(source: .pathVerified)
                } catch {
                    for p in posts {
                        self.alertLog.markDelivery(.notDelivered, for: p.recordID,
                                                   reason: Self.deliveryFailure(error))
                    }
                }
            }
        } else {
            for p in pendingPosts {
                // CONFIRM, never assume. `level` says a rule wants to tell
                // them; it says nothing about whether iOS will actually show
                // it. postConfirmed checks authorization and awaits the
                // system's acceptance, so a denied-permission phone records
                // "Not delivered — Notifications are off" instead of a
                // comforting lie. This tab exists to be trusted about exactly
                // this; recording an unverified success would make it the
                // same kind of overstatement the PR set out to remove.
                //
                // ("Away" here is a claim about where the phone was: a phone
                // across town still posts locally for what it can genuinely
                // observe, and labeling that "On Wi-Fi" would be a false
                // statement on the one tab whose entire job is honesty.)
                let body = "\(p.witness.displayName): \(p.witness.statusLine)"
                Task { [weak self] in
                    guard let self else { return }
                    do {
                        try await self.alerts.postConfirmed(title: self.fleetName, body: body,
                                                            level: p.level, threadID: p.witness.id)
                        // A wake that already reached the pocket outranks a
                        // local post; the ledger only moves delivery up.
                        self.alertLog.markDelivery(p.deliveredAway ? .away : .onLAN, for: p.recordID)
                        // A REAL alert that iOS accepted proves the same
                        // thing the Test Alert proves, and it proves it on the
                        // day it mattered — so it counts as a verification of
                        // the path, not merely as a delivery.
                        self.heartbeat.recordBeat(source: .pathVerified)
                    } catch {
                        self.alertLog.markDelivery(.notDelivered, for: p.recordID,
                                                   reason: Self.deliveryFailure(error))
                    }
                }
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

    /// One more buzz for a top-tier alarm nobody answered — and only ever
    /// one (`EscalationPolicy` rations it; the ledger's stamp enforces the
    /// "once" across relaunches, because the stamp is persisted and the
    /// in-memory ledgers are not).
    ///
    /// The second-household-member leg belongs to the relay
    /// (docs/design/alert_relay.md R1). What this can reach today is the
    /// user's OWN other devices, which is worth reaching: the phone that
    /// missed the first alert may be the one on the kitchen counter.
    /// Re-raise a top-tier alarm nobody answered.
    ///
    /// Deliberately runs even from an away phone, and deliberately runs
    /// *before* the away-darkness guard: it never makes a new observation —
    /// it only re-raises a record this device already posted from home — and
    /// "nobody answered" is most likely to be true precisely when the owner
    /// is out. `awayFromHome` is passed so the ledger says where it landed.
    private func escalateIfUnanswered(_ w: Witness, recordID: String,
                                      awayFromHome: Bool) {
        guard let record = alertLog.record(id: recordID), record.isOpen else { return }
        guard EscalationPolicy.shouldEscalate(severity: w.displaySeverity,
                                              integrityFailed: w.badge == .failed,
                                              firstPosted: record.lastBucket,
                                              now: Date(),
                                              acknowledged: record.handling != .new,
                                              alreadyEscalated: record.wasEscalated) else { return }
        // Nothing armed for this severity means we never posted the first
        // one; a second would be worse than silence. The user's per-witness
        // narrowing binds here for the same reason.
        guard alerts.hasArmedRule(for: w.effectiveSeverity),
              w.effectiveSeverity >= witnessPrefs.floor(for: w.id).minSeverity else { return }
        // Stamped BEFORE the await: at-most-once must not depend on how the
        // delivery goes, or a refused post would re-escalate every cycle.
        alertLog.markEscalated(id: recordID)
        let body = EscalationPolicy.body(name: w.displayName, statusLine: w.statusLine)
        Task { [weak self] in
            guard let self else { return }
            do {
                // Critical: the whole point of rationing escalation to the
                // top tier is that this one may pierce a silent phone.
                try await self.alerts.postConfirmed(title: self.fleetName, body: body,
                                                    level: .critical, threadID: w.id)
                self.alertLog.markDelivery(awayFromHome ? .away : .onLAN, for: recordID)
                self.heartbeat.recordBeat(source: .pathVerified)
            } catch {
                self.alertLog.markDelivery(.notDelivered, for: recordID,
                                           reason: Self.deliveryFailure(error))
            }
        }
        if alerts.reachesAnywhere(severity: w.effectiveSeverity) {
            AwayPush.shared.publishWake(WakeClass(witness: w))
        }
        // And the last rung: somebody who is not the owner. Asked as its own
        // question, in its own file, because reaching a second person is the
        // highest-cost thing this app can do with an alert — `escalated: true`
        // is this call site's statement that the alarm already went
        // unanswered, and HouseholdRelay independently refuses anything below
        // the top tier rather than trusting that flag.
        //
        // Note what is NOT gated on here: the away-reach rule, the Focus, the
        // quiet hours. Those govern whether THIS user is interrupted. A
        // household member is a different person with their own phone and
        // their own settings, and the owner's quiet hours are not theirs.
        if HouseholdRelay.mayReachHousehold(severity: w.displaySeverity,
                                            integrityFailed: w.badge == .failed,
                                            escalated: true) {
            // Named after the OCCURRENCE, so the owner's iPad escalating the
            // same alarm writes the same record and its write loses — one
            // buzz on a household phone, not one per device the owner owns.
            HouseholdShare.shared.publishEscalation(
                WakeClass(witness: w),
                occurrenceKey: HouseholdRelay.occurrenceRecordName(recordID: recordID,
                                                                   alarmBucket: record.lastBucket))
        }
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

    // MARK: - fleet Wi-Fi rollout

    /// The fleet as the Wi-Fi rollout sees it: who can take new credentials
    /// over which path RIGHT NOW. Paired WAP-class Canaries are updatable
    /// (HTTP when online, the BLE rescue when dark but in range); everything
    /// else that lives on home Wi-Fi is named hands-on so the sheet can say
    /// so instead of leaving it out and looking finished. Demo rows and
    /// BLE-beacon-only sightings (no Wi-Fi to update) are excluded.
    func wifiRolloutCandidates() -> [FleetWiFiRollout.Candidate] {
        let bleIDs = ble.provisionableDeviceIDs
        var out: [FleetWiFiRollout.Candidate] = []
        var seen = Set<String>()
        for ref in devices.devices where ref.deviceType.isHTTPPairable {
            let w = witnesses.first { $0.id == ref.id }
            out.append(FleetWiFiRollout.Candidate(
                id: ref.id,
                name: w?.displayName ?? ref.name,
                updatable: ref.baseURL != nil && devices.token(for: ref.id) != nil,
                online: w?.link == .online,
                bleReachable: bleIDs.contains(ref.id),
                rssiDBM: w?.rssiDBM))
            seen.insert(ref.id)
        }
        for w in witnesses where !seen.contains(w.id) {
            guard !w.id.hasPrefix(DemoFleet.idPrefix) else { continue }
            // A row we only ever heard as a BLE beacon has no Wi-Fi of ours
            // to update — listing it would be a chore that can't complete.
            if w.seenViaBLE && w.baseURL == nil { continue }
            seen.insert(w.id)
            out.append(FleetWiFiRollout.Candidate(
                id: w.id, name: w.displayName, updatable: false,
                online: w.link == .online, bleReachable: false, rssiDBM: w.rssiDBM))
        }
        return out
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
        let heartbeat = heartbeat
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
            // Timed, because "nearly instant" is a claim and claims get
            // measured (non-negotiable #4): the stopwatch covers ask →
            // system accepted, the same span the confirmation covers.
            let started = Date()
            try await alerts.postConfirmed(
                title: fleet,
                body: "Notifications work on this iPhone. (Away alerts are a separate test — run it from the hub.)",
                level: .important, threadID: "selftest")
            await heartbeat.noteTestRoundTrip(ms: Int(Date().timeIntervalSince(started) * 1000))
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
