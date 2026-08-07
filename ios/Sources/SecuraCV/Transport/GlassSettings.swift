// GlassSettings.swift
//
// EVERY knob a display serves, as one value — not the handful the nightlight
// screen happened to need.
//
// The gap this closes: `/api/settings` and `/api/set` are served by every
// display in the fleet (glass_web.cpp), but the app only ever spoke to them
// through NightlightAPI, which reads a nightlight's lamp keys and ignores the
// rest. So a Watch Station or a Dash served a screen brightness, a night
// window, a red shift and a peek duration that nothing in the app could
// touch. Owning a device and being unable to change its settings from the
// app you hold is the kind of gap that makes people go looking for a web page.
//
// THE DEVICE DESCRIBES, THE APP RENDERS. Everything here is read back from
// the device — including the scene catalog BY NAME and the backlight ceiling
// the HAL enforces — so new firmware capabilities appear on the phone without
// an App Store update, and the app never asserts a limit the glass didn't
// state.
//
// TOLERANT BY HAND, both directions. Unknown fields are ignored; missing ones
// keep their defaults. An older display talking to a newer app simply offers
// less, and a newer display offers more than this build knows about without
// breaking anything.

import Foundation

/// One setting the glass exposes, described well enough to render a control
/// for it without hard-coding a screen per key.
struct GlassKnob: Identifiable, Hashable, Sendable {
    enum Kind: Hashable, Sendable {
        case percent(min: Int, max: Int)
        case toggle
        case choice(labels: [String])       // index-valued
        case hourOfDay
        case minutes(min: Int, max: Int)
        case hue                            // -1 = off, else 0…359
    }

    let key: String
    let title: String
    /// What this knob does, in the user's terms. Shown under the control —
    /// a settings screen that lists knobs without saying what they cost is
    /// how people end up afraid to touch any of them.
    let blurb: String
    let kind: Kind
    var value: Int

    var id: String { key }
}

/// Everything a display told us about itself.
struct GlassSettings: Sendable, Equatable {
    // Served by every display.
    var dayPct = 60
    var nightScreen = 0          // 0 = glow, 1 = off
    var redShift = true
    var peekSeconds = 5
    var nightStartHH = 20
    var nightEndHH = 7
    var nightStep = 2

    // Served by displays with a lamp (the nightlight today).
    var hasLamp = false
    var lampScene = 0
    var lampAuto = true
    var lampPct = 72
    var lampMaxDutyPct = 50      // the HAL's heat ceiling, self-reported
    var lampMinutes = 15
    /// -1 when a catalog scene is on; 0…359 when the owner picked a color.
    var lampHue = -1
    var clock12h = true
    var orientation = 0
    var autoRotate = true
    var scenes: [String] = []

    /// True when the owner's own color is the current look rather than a
    /// scene. The device is the authority on this — the app never infers it
    /// from a value it sent earlier.
    var usesCustomHue: Bool { lampHue >= 0 }
}

enum GlassAPI {
    /// GET /api/settings. Every field optional by hand: this same call serves
    /// a Watch Station, a Dash and a nightlight, and each answers with what
    /// it actually has.
    static func settings(at base: URL, session: URLSession = .shared) async throws -> GlassSettings {
        guard DeviceAPI.isPrivate(base) else { throw DeviceError.notPrivateAddress }
        var req = URLRequest(url: base.appendingPathComponent("/api/settings"))
        req.timeoutInterval = 4
        let (data, resp) = try await session.data(for: req)
        guard let http = resp as? HTTPURLResponse, http.statusCode == 200 else {
            throw DeviceError.http((resp as? HTTPURLResponse)?.statusCode ?? 0, "settings")
        }
        let obj = try JSONSerialization.jsonObject(with: data) as? [String: Any] ?? [:]
        var s = GlassSettings()
        if let v = obj["day_pct"] as? Int { s.dayPct = v }
        if let v = obj["night_screen"] as? Int { s.nightScreen = v }
        if let v = obj["red_shift"] as? Int { s.redShift = v == 1 }
        if let v = obj["peek_s"] as? Int { s.peekSeconds = v }
        if let v = obj["night_start_hh"] as? Int { s.nightStartHH = v }
        if let v = obj["night_end_hh"] as? Int { s.nightEndHH = v }
        if let v = obj["night_step"] as? Int { s.nightStep = v }
        // The lamp block is the tell: a display without one simply doesn't
        // send these, and the app then offers no lamp controls rather than
        // offering ones that would fail.
        if let v = obj["lamp_scene"] as? Int { s.lampScene = v; s.hasLamp = true }
        if let v = obj["lamp_auto"] as? Int { s.lampAuto = v == 1 }
        if let v = obj["lamp_pct"] as? Int { s.lampPct = v }
        if let v = obj["lamp_max_duty_pct"] as? Int { s.lampMaxDutyPct = v }
        if let v = obj["lamp_minutes"] as? Int { s.lampMinutes = v }
        if let v = obj["lamp_hue"] as? Int { s.lampHue = v }
        if let v = obj["clock_12h"] as? Int { s.clock12h = v == 1 }
        if let v = obj["orientation"] as? Int { s.orientation = v }
        if let v = obj["auto_rotate"] as? Int { s.autoRotate = v == 1 }
        if let v = obj["scenes"] as? [String] { s.scenes = v }
        return s
    }

    /// POST /api/set?k=&v= — one knob per request, the contract the on-glass
    /// settings engine already validates and debounces. Shared with
    /// NightlightAPI rather than reimplemented, so there is one place that
    /// knows how to write a setting.
    static func set(_ key: String, _ value: Int, at base: URL,
                    session: URLSession = .shared) async throws {
        try await NightlightAPI.set(key, value, at: base, session: session)
    }

    // MARK: - the knobs, described from what the device answered

    /// The controls to render for this device, in the order they matter.
    /// Built from the settings the glass actually reported, so a display
    /// without a lamp shows no lamp section and nothing has to know which
    /// product it is talking to.
    static func knobs(for s: GlassSettings) -> [GlassKnob] {
        var out: [GlassKnob] = [
            GlassKnob(key: "day_pct", title: "Daytime brightness",
                      blurb: "How bright the glass is during the day.",
                      kind: .percent(min: 20, max: 100), value: s.dayPct),
            GlassKnob(key: "night_screen", title: "At night",
                      blurb: "A glow keeps the face readable in the dark; off blanks it until you look.",
                      kind: .choice(labels: ["Keep a glow", "Go dark"]), value: s.nightScreen),
            GlassKnob(key: "night_step", title: "Night dimming",
                      blurb: "How far the glass dims once night starts.",
                      kind: .percent(min: 1, max: 5), value: s.nightStep),
            GlassKnob(key: "red_shift", title: "Red shift at night",
                      blurb: "Shifts night colors out of the blue band so the glass doesn't wake you.",
                      kind: .toggle, value: s.redShift ? 1 : 0),
            GlassKnob(key: "night_start_hh", title: "Night starts",
                      blurb: "", kind: .hourOfDay, value: s.nightStartHH),
            GlassKnob(key: "night_end_hh", title: "Night ends",
                      blurb: "", kind: .hourOfDay, value: s.nightEndHH),
            GlassKnob(key: "peek_s", title: "Peek length",
                      blurb: "How long the face stays lit when you glance at it in the dark.",
                      kind: .choice(labels: ["3 seconds", "5 seconds", "10 seconds"]),
                      value: s.peekSeconds == 3 ? 0 : (s.peekSeconds == 10 ? 2 : 1)),
        ]
        guard s.hasLamp else { return out }
        out.append(contentsOf: [
            GlassKnob(key: "lamp_pct", title: "Lamp brightness",
                      blurb: "The lamp's own strength, up to this device's \(s.lampMaxDutyPct)% ceiling — a limit the glass sets for heat, not one the app invented.",
                      kind: .percent(min: 10, max: 100), value: s.lampPct),
            GlassKnob(key: "lamp_minutes", title: "Lamp runs for",
                      blurb: "It turns itself off after this. The glass has no always-on setting, so the shortest is a minute — the app doesn't offer one the device can't keep.",
                      kind: .minutes(min: 1, max: 480), value: max(1, s.lampMinutes)),
            GlassKnob(key: "lamp_auto", title: "Lamp follows the night",
                      blurb: "The lamp comes on by itself when night starts.",
                      kind: .toggle, value: s.lampAuto ? 1 : 0),
            GlassKnob(key: "clock_12h", title: "12-hour clock",
                      blurb: "", kind: .toggle, value: s.clock12h ? 1 : 0),
            GlassKnob(key: "auto_rotate", title: "Follow how it's standing",
                      blurb: "The face turns with the device when you stand it up or lay it down.",
                      kind: .toggle, value: s.autoRotate ? 1 : 0),
        ])
        return out
    }

    /// `peek_s` is stored as real seconds, not an index — so the picker's
    /// choice has to be translated back on the way out. One place does it.
    static func wireValue(for knob: GlassKnob) -> Int {
        guard knob.key == "peek_s" else { return knob.value }
        switch knob.value {
        case 0: return 3
        case 2: return 10
        default: return 5
        }
    }
}
