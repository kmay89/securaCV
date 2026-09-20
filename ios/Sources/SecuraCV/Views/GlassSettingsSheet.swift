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
// THREE RULES THE INTERACTION FOLLOWS
//
//   1. WRITE THROUGH, THEN RE-READ. Every change posts one key and then
//      re-reads /api/settings, so what you see afterwards is the device's
//      state and not the app's memory of what it sent. The glass validates,
//      clamps and debounces on its own; if it disagrees with the slider, the
//      slider is what's wrong, and this screen shows the glass winning.
//   2. SAY WHEN IT DIDN'T LAND. A setting that fails goes back to the value
//      the device reports and says so — the silent optimistic update is how
//      a settings screen quietly lies about a device it can't reach.
//   3. SHOW WHAT THE GLASS REFUSES TO TAKE, AND NEVER OFFER IT. The device
//      lists the keys /api/set refuses for every caller under `on_glass`
//      (the standalone-weather opt-in and its coarse location — a hand on
//      the glass sets them, so nobody on the Wi-Fi can). The sheet renders
//      that block as facts and filters every knob against the list, so no
//      control on this screen can 403. No control, no post, no geocoder.

import SwiftUI

struct GlassSettingsSheet: View {
    let witness: Witness
    let base: URL

    @Environment(\.dismiss) private var dismiss
    @State private var settings: GlassSettings?
    @State private var knobs: [GlassKnob] = []
    @State private var problem: String?
    @State private var busy = false
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

                        if settings.hasDirectWeather {
                            // Shown, never written (rule 3). The two rows are
                            // the glass's own words for its state and whether
                            // a grid point is stored — never the point, which
                            // no route serves. A control here would only 403.
                            Section {
                                LabeledContent("Fetches weather itself",
                                               value: GlassAPI.weatherStatusText(settings))
                                LabeledContent("Coarse location",
                                               value: GlassAPI.weatherLocationText(settings))
                            } header: {
                                Text("Weather")
                            } footer: {
                                Text("Set on the glass, not here: Settings › Weather › Fetch Weather Itself, and the location under Settings › Weather › Location. The glass keeps a ~11 km grid point and never serves it on your network. Nobody on your Wi-Fi — this phone included — can switch on the one thing it would ever fetch. With a hub, the hub stays the one thing in the house that talks to the internet.")
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
