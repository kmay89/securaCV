// DeviceDetailView.swift
//
// One Canary up close: trust (chain length + badge + a "Verify now" that
// re-checks Ed25519 on the phone), liveness/diagnostics, mute, and the
// AirPlay chime route where the hardware supports it. Config is rendered
// from the device's OWN schema, so new firmware sections appear here with no
// app update.

import SwiftUI

struct DeviceDetailView: View {
    let witness: Witness
    @EnvironmentObject var store: FleetStore
    @State private var verifying = false
    @State private var verdict: ChainVerdict?
    @State private var showingGlassSettings = false

    /// The pushed `witness` value goes stale the moment the store updates
    /// (e.g. right after Mute) — render mute state from the live row.
    private var liveWitness: Witness {
        store.witnesses.first { $0.id == witness.id } ?? witness
    }

    @ObservedObject private var home = HomeKitBridge.shared

    private var standing: HomeKitStanding {
        home.standing(for: liveWitness,
                      presentInHome: home.seenInHome(liveWitness),
                      homeHubPresent: home.homeHubPresent)
    }

    /// Where the nightlight answers HTTP: a paired base URL if one exists,
    /// else the mDNS host discovery heard it advertise, else the host baked
    /// into a `lan:<host>#<index>` provisional id (the /api/fleet rows —
    /// those never match a discovery row by id, so the id IS the route).
    /// Hosts go through the same normalization every discovered address
    /// takes (`DeviceAPI.url(forDiscoveredHost:)`) so a bare mDNS label
    /// gains its `.local` and passes the private-address gate. Nil when no
    /// route exists — the section says "not reachable" instead of guessing.
    private var nightlightBaseURL: URL? {
        if let url = liveWitness.baseURL { return url }
        if let host = store.discovery.found.first(where: { $0.deviceID == witness.id })?.host {
            return DeviceAPI.url(forDiscoveredHost: host)
        }
        if witness.id.hasPrefix("lan:") {
            let host = witness.id.dropFirst(4).split(separator: "#").first.map(String.init) ?? ""
            return DeviceAPI.url(forDiscoveredHost: host)
        }
        return nil
    }

    private var standingLabel: String {
        switch standing {
        case .off: return "Off"
        case .needsAuthorization: return "Needs access"
        case .notPaired: return "Not seen"
        case .paired: return "In the home"
        case .pairedWithoutHomeHub: return "In the home, no hub"
        case .staleInHome: return "Stale"
        }
    }

    var body: some View {
        ScrollViewReader { scroller in
        List {
            // The device itself, before any numbers about it: the turntable
            // hero (or the honest no-picture card) — see DeviceFigureCard.
            Section {
                DeviceFigureCard(witness: liveWitness)
            }
            .listRowBackground(Color.clear)

            // Who it is, directly under what it looks like — the two halves
            // of one answer, and neither was typed by a human: the figure is
            // resolved from what the device published, the name is derived
            // from its key.
            Section {
                BirthCertificateCard(witness: liveWitness, pairedAt: pairedAt)
            }

            Section("Trust") {
                LabeledContent("Signature") {
                    Label(witness.badge.label, systemImage: witness.badge.sfSymbol)
                        .foregroundStyle(witness.badge.isTrusted ? Theme.color(.calm) : .primary)
                }
                LabeledContent("Chain length", value: "\(witness.chainLength)")
                Button {
                    verifying = true
                    Task { verdict = await verifyNow(); verifying = false }
                } label: {
                    HStack {
                        Text("Verify now")
                        if verifying { Spacer(); ProgressView() }
                        else if let v = verdict { Spacer(); Text(verdictLabel(v)).foregroundStyle(.secondary) }
                    }
                }
            }

            // Where this one stands with a hub, and what to do about it —
            // shown only when there is something to do (HubGuidanceCard draws
            // nothing for a connected hub, and nothing for a device that
            // never said).
            if liveWitness.hub.needsAttention {
                Section { HubGuidanceCard(hub: liveWitness.hub) }
            }

            Section("Health") {
                // The ambient nearness whisper, said in words here: the row's
                // glyph and this line come from the same calm keeper
                // (Shared/NearnessKeeper — margin + dwell, never a sort).
                if store.nearestWitnessID == witness.id {
                    LabeledContent("Nearby") {
                        Label("Nearest to this iPhone", systemImage: "location.north.circle")
                            .foregroundStyle(Theme.color(.info))
                    }
                }
                LabeledContent("Liveness", value: witness.link.label)
                // HOW this one senses — a device-level fact derived from the
                // type it publishes (SenseModality), because no per-event
                // wire carries a modality claim. Radar presence and a camera
                // person-detection deserve different mental models even when
                // they land as the same coarse event. Unmapped types get no
                // row, never a guess.
                if let modality = SenseModality(publishedType: liveWitness.publishedType) {
                    LabeledContent("Senses") {
                        Label(modality.label, systemImage: modality.sfSymbol)
                    }
                }
                // What its own pipeline reports seeing right now, off the
                // BLE v2 beacon — present tense only: the row exists while
                // the claim is fresh (Witness.seeingNow's aging rule) and
                // simply isn't drawn once the beacon goes quiet. The class
                // vocabulary is Person/Vehicle/Animal/Package, nothing
                // finer, by construction (Invariant II).
                if let seeing = liveWitness.seeingNow() {
                    LabeledContent("Seeing now") {
                        Label(Self.seeingLine(seeing), systemImage: seeing.kind.sfSymbol)
                            .foregroundStyle(Theme.color(.info))
                    }
                }
                if let r = witness.rssiDBM { LabeledContent("Wi-Fi", value: "\(r) dBm") }
                if let b = witness.batteryPct, b >= 0 { LabeledContent("Battery", value: "\(b)%") }
                if !witness.firmware.isEmpty { LabeledContent("Firmware", value: witness.firmware) }
            }

            // The room right now — the WAP's 1 Hz sensing snapshot, mounted
            // only when this row is a paired WAP whose token still resolves
            // (the stream is Bearer-gated; an unpaired row or a Keychain-
            // lost token would 401 on every poll, so it gets no tile —
            // the same up-front guard verifyNow applies).
            if witness.deviceType == .wap,
               let ref = store.devices.devices.first(where: { $0.id == witness.id }),
               let api = try? store.devices.api(for: ref) {
                SensingNowSection(api: api)
            }

            // The Sense wellbeing surface — the radar's coarse readings,
            // rendered at last (they were modeled and invisible; the
            // competitive audit called the Sense product "invisible in the
            // app that sells it"). Shown only when the row carries any of
            // them. The live wire is /api/fleet's coarse wellbeing words:
            // canary-sense stays MQTT-only by design, and a display that
            // hears its retained state now relays it as peer rows the
            // fleet fold fills these fields from (FleetMerge.fold(row:)).
            // The demo fleet seeds the same fields; temperature/humidity
            // remain demo-only — no producer hardware exists yet.
            if liveWitness.hasWellbeingData {
                Section {
                    if let present = liveWitness.radarPresent {
                        LabeledContent("Presence", value: present ? "Present" : "Clear")
                    }
                    if let occupants = liveWitness.radarOccupants {
                        LabeledContent("Occupants", value: Witness.occupantsLabel(occupants))
                    }
                    if let locked = liveWitness.breathingLock {
                        // "Sensed", never a vital-signs claim: the lock is a
                        // rhythm the radar can currently hold, nothing more.
                        LabeledContent("Breathing rhythm", value: locked ? "Sensed" : "Not sensed")
                    }
                    if let t = liveWitness.tempC {
                        LabeledContent("Temperature",
                                       value: Measurement(value: t, unit: UnitTemperature.celsius)
                                           .formatted())
                    }
                    if let h = liveWitness.humidityPct {
                        LabeledContent("Humidity", value: "\(h)%")
                    }
                } header: {
                    Text("Wellbeing")
                } footer: {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("As the radar last reported it — coarse by design: presence, a count that tops out at 2+, a breathing rhythm yes/no. No camera, no identity, and vitals never enter the sealed log.")
                        // Provenance: these words arrive relayed (a display
                        // repeating the sense device's retained broker claim,
                        // or a poll of the device itself), so say when they
                        // were heard rather than presenting them as a live
                        // feed. Rows from before the stamp existed show
                        // nothing — never an invented freshness.
                        if let at = liveWitness.wellbeingAt {
                            Text("Heard \(at, format: .relative(presentation: .named)) — a relayed reading, not a live feed.")
                        }
                    }
                }
            }

            // "Where IS it?" — the hot/cold search over this Canary's own
            // Bluetooth beacon, plus (WAP-class) making the device chirp and
            // blink for you. Offered whenever the beacon can be recognized:
            // matching needs the fingerprint, which a paired device carries
            // once its key is pinned (FleetStore.poll, first successful poll)
            // and every beacon-decorated row carries by construction. A row
            // with no fingerprint honestly has nothing to match against.
            if !liveWitness.fingerprint.isEmpty {
                Section {
                    NavigationLink {
                        FindCanaryView(witness: liveWitness)
                    } label: {
                        Label("Find this Canary", systemImage: "location.north.circle")
                    }
                } footer: {
                    Text("Warmer/colder over its Bluetooth beacon — room-level, honest words, no fake arrow. A WAP-class Canary can also chirp and blink so it finds you back.")
                }
            }

            if home.isEnabled {
                // The Doctor row: what Apple Home and the fleet each say
                // about this Canary, and one calm line when they disagree.
                Section {
                    LabeledContent("Apple Home", value: standingLabel)
                } footer: {
                    if let note = standing.doctorNote {
                        Label(note, systemImage: "exclamationmark.triangle")
                            .font(.caption)
                            .foregroundStyle(Theme.color(.warn))
                    }
                }
            }

            if witness.deviceType == .wap {
                // "Chime", not "Two-way": there is no talk feature, and a
                // section header must not promise one (non-negotiable #4).
                Section("Chime") {
                    HStack {
                        Label("AirPlay chime", systemImage: "airplayaudio")
                        Spacer()
                        AirPlayRoutePicker().frame(width: 44, height: 44)
                    }
                }
            }

            if witness.deviceType == .nightlight {
                // The nightlight's own knobs — lamp color/strength, the
                // night schedule, the 12-hour clock — rendered from the
                // device's own /api/settings (the device describes, the
                // app renders).
                NightlightSection(base: nightlightBaseURL)
            }

            // EVERY display serves /api/settings and /api/set, not just the
            // nightlight — the screen brightness, the night window, the red
            // shift and the peek length were all being served to nobody on a
            // Watch Station or a Dash. One screen renders whatever the glass
            // reports, so this needs no per-product branch.
            if witness.deviceType.servesGlassSettings, let base = nightlightBaseURL {
                Section {
                    Button {
                        showingGlassSettings = true
                    } label: {
                        Label("Display settings", systemImage: "slider.horizontal.3")
                    }
                } footer: {
                    Text("Brightness, the night window, how long it lights when you glance at it — and the lamp's color, if it has one. Read from the device, written back to it.")
                }
                .sheet(isPresented: $showingGlassSettings) {
                    GlassSettingsSheet(witness: liveWitness, base: base)
                }
            }

            Section {
                // What this one Canary may interrupt for. Narrowing only —
                // there is deliberately no "never" (WitnessPushFloor): a
                // permanent per-device silence is the failure mode this
                // product can't ship. Unpairing is the honest way to hear
                // nothing, and it looks like what it is.
                Picker("Tell me about this one",
                       selection: Binding(get: { store.pushFloor(for: witness.id) },
                                          set: { store.setPushFloor($0, for: witness.id) })) {
                    ForEach(WitnessPushFloor.allCases) { floor in
                        Text(floor.title).tag(floor)
                    }
                }
            } footer: {
                Text(store.pushFloor(for: witness.id).explanation)
            }

            Section {
                if liveWitness.isMuted {
                    Button("Unmute") { store.unmute(witness.id) }
                        .tint(Theme.color(.warn))
                } else {
                    // Three real choices instead of one fixed hour — a mute
                    // whose length matches the reason for it is a mute people
                    // end up not needing to extend.
                    Menu {
                        ForEach(MuteDuration.offered(at: Date())) { duration in
                            Button {
                                store.mute(witness.id, duration: duration)
                            } label: { Label(duration.title, systemImage: duration.sfSymbol) }
                        }
                    } label: {
                        Text("Mute…")
                    }
                    .tint(Theme.color(.warn))
                }
            } footer: {
                Text(mutedFooter)
            }

            // This Canary's own history — the per-device answer every
            // camera app has and the Alerts tab (fleet-wide by design)
            // deliberately doesn't. Purely a filter over the same ledger:
            // nothing new is recorded, and the collapse/lifecycle/reach
            // rules arrive intact. The day-shape ribbon above the rows is
            // the same scrub the Alerts tab mounts, walking this list.
            if !ownRecords.isEmpty {
                Section {
                    TimelineScrubSection(records: ownRecords) { bucket in
                        if let id = anchorID(for: bucket) {
                            // Animated, or the release of a scrub reads as a
                            // teleport — continuity is the confirmation.
                            withAnimation(.snappy) { scroller.scrollTo(id, anchor: .top) }
                        }
                    }
                    .listRowInsets(EdgeInsets())
                    .listRowBackground(Color.clear)
                    // UIKit's hairline must never undercut the instrument's
                    // one baseline (same rule as the Alerts mount).
                    .listRowSeparator(.hidden)
                    ForEach(ownRecords) { record in
                        AlertRecordRow(record: record).id(record.id)
                    }
                } header: {
                    Text("History")
                } footer: {
                    Text("This phone's notebook, narrowed to this Canary — the same rows as the Alerts tab. The sealed witness chain on the device itself is the full record.")
                }
            }
        }
        .navigationTitle(witness.displayName)
        .navigationBarTitleDisplayMode(.inline)
        // Leaving the screen is "I've looked" for THIS Canary's rows — the
        // same on-the-way-out stamp the Alerts tab uses, scoped: reading
        // history here must clear this Canary's unseen dots and its share
        // of the app badge, and nobody else's.
        .onDisappear { store.markAlertsSeen(for: witness.id) }
        }
    }

    /// This Canary's slice of the alert ledger, newest first (the ledger's
    /// own order — the same assumption the Alerts tab makes).
    private var ownRecords: [AlertRecord] {
        store.alertLog.records.filter { $0.witnessID == witness.id }
    }

    /// The row a scrubbed bucket should bring into view — same rule as the
    /// Alerts tab: the newest record at or before the bucket, so scrubbing
    /// into a quiet stretch lands on the last thing that actually happened.
    private func anchorID(for bucket: Date) -> String? {
        let target = bucket.timeIntervalSince1970
        let records = ownRecords
        if let hit = records.first(where: { $0.lastBucket.timeIntervalSince1970 <= target }) {
            return hit.id
        }
        return records.last?.id
    }

    /// "Person · 87%" — the confidence rides along only when the wire
    /// scored the claim; a sender may honestly not score it.
    private static func seeingLine(_ seeing: (kind: SeenClass, score: Int?)) -> String {
        if let score = seeing.score { return "\(seeing.kind.label) · \(score)%" }
        return seeing.kind.label
    }

    /// When this phone paired it. Nil for a Canary we can see but have not
    /// paired — it has no relationship with us to date, and the certificate
    /// card says "paired" only when there is a pairing to name.
    private var pairedAt: Date? {
        store.devices.devices.first { $0.id == witness.id }?.pairedAt
    }

    /// Says when the quiet ends, because every mute here does end.
    private var mutedFooter: String {
        if let until = liveWitness.mutedUntil, liveWitness.isMuted {
            return "Quiet until \(until.formatted(date: .omitted, time: .shortened)). Tamper and a failed signature still punch through."
        }
        return "A muted Canary stops nagging but stays visible — tamper and a failed signature still punch through. Every mute ends on its own."
    }

    private func verifyNow() async -> ChainVerdict {
        guard let ref = store.devices.devices.first(where: { $0.id == witness.id }),
              let api = try? store.devices.api(for: ref),
              let page = try? await api.witness(last: 100) else {
            return .unsigned
        }
        return ChainVerifier.verify(page, pinnedKey: PinnedKeyStore.key(for: witness.id))
    }

    private func verdictLabel(_ v: ChainVerdict) -> String {
        switch v {
        case .verified: return "Verified ✓"
        case .signedUnpinned: return "Signed"
        case .unsigned: return "Unsigned"
        case .brokenLink(let seq): return "Broken at #\(seq)"
        case .signatureFailed: return "FAILED"
        }
    }
}

#Preview("Device detail — demo Canary") {
    NavigationStack {
        DeviceDetailView(witness: DemoFleet.witnesses().first!)
            .environmentObject(DemoFleet.previewStore())
    }
}
