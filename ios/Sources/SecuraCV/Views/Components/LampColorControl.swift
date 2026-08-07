// LampColorControl.swift
//
// Pick a color for the lamp: a wheel, a row of presets, and the device's own
// scene catalog — three ways to answer one question, with the device as the
// authority on which answer is currently in force.
//
// WHY THE WHEEL IS REAL RATHER THAN DECORATIVE. The glass takes an arbitrary
// hue (`lamp_hue`, 0…359) and builds a look around it, so what this control
// sends is what the lamp shows. It deliberately offers HUE ONLY — not
// saturation, not value — because those two are the device's to decide: the
// look engine drifts them across the four stops so a color breathes, and the
// HAL caps brightness for heat. A picker with three sliders would imply
// control the glass doesn't hand over.
//
// SCENES AND THE WHEEL ARE ONE CHOICE. The firmware clears the hue when a
// scene is picked and vice versa, and `/api/settings` reports which is live —
// so this screen never has to remember, and can never show two answers to
// "what color is it?".

import SwiftUI

struct LampColorControl: View {
    let settings: GlassSettings
    let base: URL
    /// Reports whether the write landed, so the sheet can re-read and speak up.
    let onChange: (Bool) async -> Void

    @State private var hue: Double = 0
    @State private var sending = false

    /// Named colors worth one tap. Kept few and obvious — a preset row that
    /// tries to be a palette is just a worse wheel.
    private static let presets: [(String, Int)] = [
        ("Red", 0), ("Amber", 35), ("Gold", 50), ("Green", 120),
        ("Teal", 175), ("Blue", 215), ("Indigo", 260), ("Violet", 290), ("Pink", 325),
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.m) {
            // What's on right now, from the device's own answer.
            HStack(spacing: Theme.s) {
                Circle()
                    .fill(settings.usesCustomHue
                          ? Color(hue: Double(settings.lampHue) / 360, saturation: 0.85, brightness: 1)
                          : Color.secondary.opacity(0.3))
                    .frame(width: 22, height: 22)
                    .overlay(Circle().strokeBorder(.secondary.opacity(0.35)))
                Text(currentLabel).font(.subheadline)
                Spacer()
                if sending { ProgressView().controlSize(.small) }
            }

            // The wheel. Hue only — see the header for why.
            HueSlider(hue: $hue) { chosen in
                Task { await send(hue: Int(chosen.rounded())) }
            }
            .frame(height: 34)

            // Presets.
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: Theme.s) {
                    ForEach(Self.presets, id: \.1) { name, value in
                        Button {
                            hue = Double(value)
                            Task { await send(hue: value) }
                        } label: {
                            VStack(spacing: 4) {
                                Circle()
                                    .fill(Color(hue: Double(value) / 360, saturation: 0.85, brightness: 1))
                                    .frame(width: 30, height: 30)
                                    .overlay(Circle().strokeBorder(
                                        settings.usesCustomHue && settings.lampHue == value
                                            ? Color.primary : .clear, lineWidth: 2))
                                Text(name).font(.caption2).foregroundStyle(.secondary)
                            }
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(name)
                    }
                }
                .padding(.vertical, 2)
            }

            // The device's own catalog, by name. New firmware scenes appear
            // here with no app update — the app never carries this list.
            if !settings.scenes.isEmpty {
                Divider()
                Text("Or a scene").font(.caption).foregroundStyle(.secondary)
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: Theme.s) {
                        ForEach(Array(settings.scenes.enumerated()), id: \.offset) { index, name in
                            Button {
                                Task { await send(scene: index) }
                            } label: {
                                Text(name)
                                    .font(.caption)
                                    .padding(.horizontal, Theme.m)
                                    .padding(.vertical, Theme.s)
                                    .background(
                                        Capsule().fill(!settings.usesCustomHue && settings.lampScene == index
                                                       ? Theme.color(.info).opacity(0.25)
                                                       : Color.secondary.opacity(0.12)))
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    .padding(.vertical, 2)
                }
            }
        }
        .onAppear { hue = Double(settings.usesCustomHue ? settings.lampHue : 40) }
    }

    private var currentLabel: String {
        guard settings.usesCustomHue else {
            let name = settings.scenes.indices.contains(settings.lampScene)
                ? settings.scenes[settings.lampScene] : "a scene"
            return "Showing \(name)"
        }
        return "Your color · hue \(settings.lampHue)°"
    }

    private func send(hue value: Int) async {
        sending = true
        defer { sending = false }
        let ok = (try? await GlassAPI.set("lamp_hue", value, at: base)) != nil
        await onChange(ok)
    }

    private func send(scene index: Int) async {
        sending = true
        defer { sending = false }
        // Only the scene is sent: the firmware clears the hue itself, so the
        // app doesn't have to send two keys and hope both land.
        let ok = (try? await GlassAPI.set("lamp_scene", index, at: base)) != nil
        await onChange(ok)
    }
}

/// A hue strip, dragged. Full-spectrum so the color under your thumb is the
/// color you get — a wheel that needed a legend would be a worse control.
struct HueSlider: View {
    @Binding var hue: Double
    /// Fired on release, not per pixel: one key per request is the device's
    /// contract, and the glass debounces its own NVS write.
    let commit: (Double) -> Void

    var body: some View {
        GeometryReader { geo in
            let width = max(geo.size.width, 1)
            ZStack(alignment: .leading) {
                Capsule().fill(LinearGradient(
                    colors: stride(from: 0, through: 360, by: 30).map {
                        Color(hue: Double($0) / 360, saturation: 0.85, brightness: 1)
                    },
                    startPoint: .leading, endPoint: .trailing))
                Circle()
                    .strokeBorder(.white, lineWidth: 3)
                    .background(Circle().fill(Color(hue: hue / 360, saturation: 0.85, brightness: 1)))
                    .frame(width: 28, height: 28)
                    .shadow(radius: 2)
                    .offset(x: min(max(0, hue / 360 * width - 14), width - 28))
            }
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        hue = min(359, max(0, value.location.x / width * 360))
                    }
                    .onEnded { _ in commit(hue) }
            )
            .accessibilityElement()
            .accessibilityLabel("Lamp color")
            .accessibilityValue("Hue \(Int(hue)) degrees")
            .accessibilityAdjustableAction { direction in
                hue = min(359, max(0, hue + (direction == .increment ? 10 : -10)))
                commit(hue)
            }
        }
    }
}
