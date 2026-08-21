// GlassSettingsSheet.swift
//
// The display's own settings, all of them, on the phone.
//
// This screen renders whatever the device answered with (GlassAPI.knobs) —
// so a Watch Station shows its screen and night controls, a nightlight shows
// those plus its lamp, and neither case is written down here as a product
// name. A firmware that grows a knob shows up when the glass starts reporting
// it, not when the App Store does.
//
// TWO RULES THE INTERACTION FOLLOWS
//
//   1. WRITE THROUGH, THEN RE-READ. Every change posts one key and then
//      re-reads /api/settings, so what you see afterwards is the device's
//      state and not the app's memory of what it sent. The glass validates,
//      clamps and debounces on its own; if it disagrees with the slider, the
//      slider is what's wrong, and this screen shows the glass winning.
//   2. SAY WHEN IT DIDN'T LAND. A setting that fails goes back to the value
//      the device reports and says so — the silent optimistic update is how
//      a settings screen quietly lies about a device it can't reach.

import SwiftUI
import CoreLocation

struct GlassSettingsSheet: View {
    let witness: Witness
    let base: URL

    @Environment(\.dismiss) private var dismiss
    @State private var settings: GlassSettings?
    @State private var knobs: [GlassKnob] = []
    @State private var problem: String?
    @State private var busy = false
    @State private var wxPlace = ""
    @State private var wxPlaceStatus: String?
    /// Every knob as the device reported it when this sheet OPENED — the
    /// safety net under a session of fiddling. Captured once; "Undo
    /// changes" replays it through the ordinary write path (SettingsRevert).
    @State private var snapshot: [GlassKnob]?

    var body: some View {
        NavigationStack {
            Group {
                if let settings {
                    List {
                        if let problem {
                            Section {
                                Label(problem, systemImage: "exclamationmark.triangle")
                                    .font(.footnote)
                                    .foregroundStyle(Theme.color(.warn))
                            }
                        }

                        if settings.hasLamp {
                            Section {
                                LampColorControl(settings: settings, base: base,
                                                 onChange: { await reload(after: $0) })
                            } header: {
                                Text("Lamp color")
                            } footer: {
                                Text("A scene, or a color you pick. Whichever you choose becomes the one that's on — and neither dresses an alarm: if something needs you, the glass drops the look and shows the real color.")
                            }
                        }

                        ForEach($knobs) { $knob in
                            Section {
                                GlassKnobRow(knob: $knob) { edited in
                                    await write(edited)
                                }
                            } footer: {
                                if !knob.blurb.isEmpty { Text(knob.blurb) }
                            }
                        }

                        if settings.hasDirectWeather && !settings.wxHub {
                            Section {
                                TextField("City, region — e.g. Austin, TX", text: $wxPlace)
                                    .textInputAutocapitalization(.words)
                                    .autocorrectionDisabled()
                                Button(settings.wxLocSet ? "Replace stored location"
                                                        : "Set weather location") {
                                    Task { await setWeatherLocation() }
                                }
                                .disabled(wxPlace.trimmingCharacters(in: .whitespaces).isEmpty || busy)
                                if let wxPlaceStatus {
                                    Text(wxPlaceStatus)
                                        .font(.footnote)
                                        .foregroundStyle(.secondary)
                                }
                            } header: {
                                Text("Weather location")
                            } footer: {
                                Text("Type a place; the phone looks it up and sends the glass a "
                                   + "coarse ~11 km grid point — never your exact address, and "
                                   + "never this phone's location. The glass keeps only that "
                                   + "rounded point.")
                            }
                        }
                    }
                } else if problem != nil {
                    ContentUnavailableView("Can't reach this display",
                                           systemImage: "wifi.exclamationmark",
                                           description: Text(problem ?? ""))
                } else {
                    ProgressView("Asking the glass what it can do…")
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
            .navigationTitle(witness.displayName)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    // Offered only while there is honestly something to walk
                    // back — a clean session never grows the button.
                    if !revertWrites.isEmpty {
                        Button("Undo changes") { Task { await revert() } }
                            .disabled(busy)
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .task { await load() }
            .refreshable { await load() }
        }
    }

    private func load() async {
        do {
            let s = try await GlassAPI.settings(at: base)
            settings = s
            knobs = GlassAPI.knobs(for: s)
            problem = nil
            // The first successful read is the session's "before" — the
            // state "Undo changes" walks back to. Never overwritten: the
            // net is only worth anything anchored where the session began.
            if snapshot == nil { snapshot = knobs }
        } catch {
            problem = error.localizedDescription
        }
    }

    /// What "Undo changes" would write right now — empty means clean.
    private var revertWrites: [GlassKnob] {
        guard let snapshot else { return [] }
        return SettingsRevert.writes(toRestore: snapshot, from: knobs)
    }

    /// Walk the device back to the opening snapshot, one key per POST (the
    /// device's own contract), then re-read so the glass has the last word.
    private func revert() async {
        guard !busy else { return }
        busy = true
        defer { busy = false }
        for knob in revertWrites {
            do {
                try await GlassAPI.set(knob.key, GlassAPI.wireValue(for: knob), at: base)
            } catch {
                problem = "“\(knob.title)” wouldn't go back — \(error.localizedDescription)"
            }
        }
        await load()
    }

    /// Post one key, then re-read. The re-read is the honest part: the glass
    /// clamps and validates on its own, so the value that comes back is the
    /// truth and the control follows it rather than the finger.
    private func write(_ knob: GlassKnob) async {
        guard !busy else { return }
        busy = true
        defer { busy = false }
        do {
            try await GlassAPI.set(knob.key, GlassAPI.wireValue(for: knob), at: base)
            await load()
        } catch {
            problem = "“\(knob.title)” didn't stick — \(error.localizedDescription)"
            await load()      // snap back to what the device actually has
        }
    }

    private func reload(after ok: Bool) async {
        if !ok { problem = "The glass didn't take that color." }
        await load()
    }

    /// Geocode the typed place ON THE PHONE (no location permission — this is
    /// a text lookup, not a fix), coarsen to the 0.1° grid, and send the one
    /// combined integer the firmware accepts atomically (wx_loc_encode in
    /// glass_settings.h: (lat10+900)*4000 + (lon10+1800)).
    private func setWeatherLocation() async {
        busy = true
        defer { busy = false }
        wxPlaceStatus = "Looking up…"
        do {
            let marks = try await CLGeocoder().geocodeAddressString(wxPlace)
            guard let loc = marks.first?.location else {
                wxPlaceStatus = "Couldn't find that place — try adding a region."
                return
            }
            let lat10 = Int((loc.coordinate.latitude * 10).rounded())
            let lon10 = Int((loc.coordinate.longitude * 10).rounded())
            guard (-900...900).contains(lat10), (-1800...1800).contains(lon10) else {
                wxPlaceStatus = "That point is outside the valid grid."
                return
            }
            let combined = (lat10 + 900) * 4000 + (lon10 + 1800)
            try await GlassAPI.set("wx_loc", combined, at: base)
            wxPlaceStatus = "Stored as a coarse grid point."
            wxPlace = ""
            await load()
        } catch {
            wxPlaceStatus = "Didn't stick — \(error.localizedDescription)"
        }
    }
}

/// One knob, rendered by what KIND it is rather than by which key it is —
/// so a new setting needs a kind, not a screen.
struct GlassKnobRow: View {
    @Binding var knob: GlassKnob
    let commit: (GlassKnob) async -> Void

    var body: some View {
        switch knob.kind {
        case .toggle:
            Toggle(knob.title, isOn: Binding(
                get: { knob.value == 1 },
                set: { on in knob.value = on ? 1 : 0; Task { await commit(knob) } }))

        case .percent(let min, let max):
            VStack(alignment: .leading) {
                HStack {
                    Text(knob.title)
                    Spacer()
                    Text("\(knob.value)%").foregroundStyle(.secondary).monospacedDigit()
                }
                Slider(value: Binding(get: { Double(knob.value) },
                                      set: { knob.value = Int($0.rounded()) }),
                       in: Double(min)...Double(max), step: 1) { editing in
                    // Commit on release, not on every pixel: one key per
                    // request is the device's contract, and a slider that
                    // posted continuously would flood a debounced NVS write.
                    if !editing { Task { await commit(knob) } }
                }
            }

        case .minutes(let min, let max):
            VStack(alignment: .leading) {
                HStack {
                    Text(knob.title)
                    Spacer()
                    Text(minutesLabel(knob.value)).foregroundStyle(.secondary).monospacedDigit()
                }
                Slider(value: Binding(get: { Double(knob.value) },
                                      set: { knob.value = Int($0.rounded()) }),
                       in: Double(min)...Double(max), step: 1) { editing in
                    if !editing { Task { await commit(knob) } }
                }
            }

        case .choice(let labels):
            Picker(knob.title, selection: Binding(
                get: { knob.value },
                set: { knob.value = $0; Task { await commit(knob) } })) {
                ForEach(Array(labels.enumerated()), id: \.offset) { i, label in
                    Text(label).tag(i)
                }
            }

        case .hourOfDay:
            Picker(knob.title, selection: Binding(
                get: { knob.value },
                set: { knob.value = $0; Task { await commit(knob) } })) {
                ForEach(0..<24, id: \.self) { hour in
                    Text(Self.hourLabel(hour)).tag(hour)
                }
            }

        case .hue:
            EmptyView()      // the wheel has its own control
        }
    }

    private func minutesLabel(_ m: Int) -> String {
        m < 60 ? "\(m) min" : (m % 60 == 0 ? "\(m / 60) h" : "\(m / 60) h \(m % 60) m")
    }

    static func hourLabel(_ hour: Int) -> String {
        var comps = DateComponents()
        comps.hour = hour
        comps.minute = 0
        let cal = Calendar.current
        let date = cal.date(from: comps) ?? Date()
        return date.formatted(date: .omitted, time: .shortened)
    }
}
