// NightlightSection.swift
//
// The Nightlight's config card inside DeviceDetailView: lamp color (the
// device's OWN scene list — warm Lantern orange, Rainbow, Moonbeam white,
// whatever the firmware ships next), lamp strength, lamp-through-the-night,
// the 12-hour clock, and quiet hours. Everything writes through the same
// one-knob `/api/set` contract the on-glass settings surface uses, so the
// phone and the glass can never disagree about what a setting means.
//
// Honesty note carried up from the firmware: the brightness slider is a
// percentage of the lamp's DRAWN strength. The panel's backlight duty is
// hard-capped in the device's HAL (self-reported here as lampMaxDutyPct) —
// a heat budget for the closed pocket case that no slider can climb over.

import SwiftUI

struct NightlightSection: View {
    /// The device's LAN address (mDNS .local preferred), or nil while we
    /// have no route to it — the section then says so instead of guessing.
    let base: URL?

    @State private var settings: NightlightSettings?
    @State private var loadFailed = false

    var body: some View {
        Section {
            if let base {
                if let s = settings {
                    Picker("Lamp color", selection: intBinding(\.lampScene, "lamp_scene", at: base)) {
                        ForEach(Array(s.scenes.enumerated()), id: \.offset) { i, name in
                            Text(name).tag(i)
                        }
                    }

                    Toggle("Lamp on through the night",
                           isOn: boolBinding(\.lampAuto, "lamp_auto", at: base))

                    VStack(alignment: .leading) {
                        Text("Lamp brightness")
                        Slider(value: Binding(
                            get: { Double(settings?.lampPct ?? 70) },
                            set: { v in
                                let pct = max(10, min(100, Int((v / 10).rounded() * 10)))
                                guard pct != settings?.lampPct else { return }
                                settings?.lampPct = pct
                                Task { try? await NightlightAPI.set("lamp_pct", pct, at: base) }
                            }
                        ), in: 10...100, step: 10)
                    }

                    Toggle("12-hour clock",
                           isOn: boolBinding(\.clock12h, "clock_12h", at: base))

                    // The IMU follows real movement: stand it on any edge
                    // and the clock rights itself (and the canary tumbles).
                    // Picking an orientation by hand parks auto — same as
                    // the device's triple-press — and this toggle brings
                    // it back.
                    Toggle("Turn with the room",
                           isOn: boolBinding(\.autoRotate, "auto_rotate", at: base))
                    if !s.autoRotate {
                        Picker("Orientation", selection: Binding(
                            get: { settings?.orientation ?? 0 },
                            set: { v in
                                settings?.orientation = v
                                Task { try? await NightlightAPI.set("orientation", v, at: base) }
                            }
                        )) {
                            Text("Upright").tag(0)
                            Text("On its left side").tag(1)
                            Text("Upside down").tag(2)
                            Text("On its right side").tag(3)
                        }
                    }

                    Picker("Night starts",
                           selection: intBinding(\.nightStartHH, "night_start_hh", at: base)) {
                        ForEach(0..<24, id: \.self) { h in
                            Text(hourLabel(h, twelveHour: s.clock12h)).tag(h)
                        }
                    }
                    Picker("Night ends",
                           selection: intBinding(\.nightEndHH, "night_end_hh", at: base)) {
                        ForEach(0..<24, id: \.self) { h in
                            Text(hourLabel(h, twelveHour: s.clock12h)).tag(h)
                        }
                    }
                } else if loadFailed {
                    Button("Couldn't reach the nightlight — try again") {
                        loadFailed = false
                        Task { await load(base) }
                    }
                } else {
                    HStack { Text("Reading its settings"); Spacer(); ProgressView() }
                        .task { await load(base) }
                }
            } else {
                Text("Not reachable right now — the nightlight configures here the moment it shows up on your network.")
                    .foregroundStyle(.secondary)
            }
        } header: {
            Text("Nightlight")
        } footer: {
            if let s = settings {
                Text("The panel keeps itself cool: backlight power is capped at \(s.lampMaxDutyPct)% in the firmware, underneath every setting here.")
            }
        }
    }

    private func load(_ base: URL) async {
        do { settings = try await NightlightAPI.settings(at: base) }
        catch { loadFailed = true }
    }

    /// One knob, one request — the binding mutates local state for a live
    /// UI and posts the same value the on-glass engine validates.
    private func intBinding(_ keyPath: WritableKeyPath<NightlightSettings, Int>,
                            _ key: String, at base: URL) -> Binding<Int> {
        Binding(
            get: { settings?[keyPath: keyPath] ?? 0 },
            set: { v in
                settings?[keyPath: keyPath] = v
                Task { try? await NightlightAPI.set(key, v, at: base) }
            }
        )
    }

    private func boolBinding(_ keyPath: WritableKeyPath<NightlightSettings, Bool>,
                             _ key: String, at base: URL) -> Binding<Bool> {
        Binding(
            get: { settings?[keyPath: keyPath] ?? false },
            set: { v in
                settings?[keyPath: keyPath] = v
                Task { try? await NightlightAPI.set(key, v ? 1 : 0, at: base) }
            }
        )
    }

    private func hourLabel(_ h: Int, twelveHour: Bool) -> String {
        guard twelveHour else { return String(format: "%02d:00", h) }
        let base = h % 12 == 0 ? 12 : h % 12
        return "\(base) \(h < 12 ? "AM" : "PM")"
    }
}
