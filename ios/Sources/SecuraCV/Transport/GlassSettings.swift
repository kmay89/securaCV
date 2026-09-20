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

    // Served only by glass whose backlight cannot dim (the 4.3" and 7" panels
    // drive theirs through a CH422G expander line, which is on/off in
    // hardware). There, sustained brightness is a RENDERED dim — a black scrim
    // over the face — and `bright_pct` is the knob that moves it.
    //
    // The presence of this block is the tell, exactly like the lamp block
    // below. It matters because `day_pct` is served by every display and does
    // nothing visible on these boards: it scales a backlight level that can
    // only ever be on or off. Offering both would put two brightness sliders
    // on one screen, one of which silently does nothing.
    var hasRenderedDim = false
    var brightPct = 100
    var brightMinPct = 50        // the floor the glass sets; darker is Night's job

    // Served only by the dash-family glass (the 4.3" and 7" panels): the
    // curated Character ring (face + colors) and the clock-face ring, BY
    // NAME — the device describes its own catalog exactly like the lamp
    // scenes below, so a new look ships to the phone with no app update.
    // These used to be on-glass-only settings; owning a 7" display and
    // being unable to change its face from the phone was the gap.
    var hasLook = false
    var character = 0
    var characterNames: [String] = []
    var clockStyle = 0
    var clockStyleNames: [String] = []
    var clock12h = true

    // Served only by glass that carries the standalone forecast
    // (FEATURE_STANDALONE_WEATHER — the 7" dash7 / nightstand7 builds), and
    // served NESTED under `on_glass` on purpose: these are the keys
    // POST /api/set refuses for every caller, token or not, with
    // 403 on_glass_only (the policy table in canary/net/settings_policy.h,
    // handed back as `keys` so the phone and the handler cannot disagree
    // about which keys those are). The switch and the location take a hand
    // on the glass (Settings › Weather), so the app SHOWS this block and
    // never writes it — a control here could only ever fail.
    //
    // The block's presence is the tell, like the lamp block below. The two
    // location-derived facts ride only on requests that are not cross-site
    // (glass_web.cpp handle_settings_get), so they stay optional: nil means
    // "the glass did not say", never a guess. The grid point itself is never
    // served by any route, so there is nothing here that could show it.
    var hasDirectWeather = false
    /// The keys the glass refuses from the network — its own list. Every
    /// offered knob is filtered against it (GlassAPI.knobs), so the app
    /// structurally cannot draw a control the handler would 403.
    var onGlassKeys: [String] = []
    var wxDirect = false
    /// The fetcher's verdict (wx_direct.h): 0 off, 1 needs a location,
    /// 2 a hub owns weather, 3 on, 4 on but the last fetch failed.
    var wxStatus: Int?
    /// Whether a coarse ~11 km grid point is stored — never where.
    var wxLocSet: Bool?

    // Served by displays with a lamp (the nightlight today).
    var hasLamp = false
    var lampScene = 0
    var lampAuto = true
    var lampPct = 72
    var lampMaxDutyPct = 50      // the HAL's heat ceiling, self-reported
    var lampMinutes = 15
    /// -1 when a catalog scene is on; 0…359 when the owner picked a color.
    var lampHue = -1
    var orientation = 0
    var autoRotate = true
    var scenes: [String] = []

    /// True when the owner's own color is the current look rather than a
    /// scene. The device is the authority on this — the app never infers it
    /// from a value it sent earlier.
    var usesCustomHue: Bool { lampHue >= 0 }
}

extension GlassSettings {
    /// The body of GET /api/settings, as the device described itself. Split
    /// from the request (GlassAPI.settings) so a test can feed it the exact
    /// bytes glass_web.cpp emits — the same shape FleetSelfReport.decode
    /// takes for /api/fleet. Every field optional by hand: this same body
    /// serves a Watch Station, a Dash and a nightlight, and each answers
    /// with what it actually has.
    static func decode(_ data: Data) throws -> GlassSettings {
        let obj = try JSONSerialization.jsonObject(with: data) as? [String: Any] ?? [:]
        var s = GlassSettings()
        if let v = obj["day_pct"] as? Int { s.dayPct = v }
        if let v = obj["night_screen"] as? Int { s.nightScreen = v }
        if let v = obj["red_shift"] as? Int { s.redShift = v == 1 }
        if let v = obj["peek_s"] as? Int { s.peekSeconds = v }
        if let v = obj["night_start_hh"] as? Int { s.nightStartHH = v }
        if let v = obj["night_end_hh"] as? Int { s.nightEndHH = v }
        if let v = obj["night_step"] as? Int { s.nightStep = v }
        // The rendered-dim block: present only on glass that dims by scrim.
        if let v = obj["bright_pct"] as? Int { s.brightPct = v; s.hasRenderedDim = true }
        if let v = obj["bright_min_pct"] as? Int { s.brightMinPct = v }
        // The look block: the tell is the device's own catalog of names.
        if let v = obj["character"] as? Int { s.character = v }
        if let v = obj["characters"] as? [String], !v.isEmpty {
            s.characterNames = v
            s.hasLook = true
        }
        if let v = obj["clock_style"] as? Int { s.clockStyle = v }
        if let v = obj["clock_styles"] as? [String] { s.clockStyleNames = v }
        if let v = obj["clock_12h"] as? Int { s.clock12h = v == 1 }
        // The standalone-weather block: the `on_glass` object is the tell, and
        // it is read from the block and nowhere else. No firmware in this
        // checkout's history (the commits back to b4a9083) ever served these
        // keys at the top level, and a top-level key is exactly what a client
        // renders as a control — so a fallback would invent a switch the
        // glass refuses. Hub ownership is the verdict (wx_status 2), not a
        // key of its own.
        if let g = obj["on_glass"] as? [String: Any] {
            s.hasDirectWeather = true
            s.onGlassKeys = (g["keys"] as? [String]) ?? []
            if let v = g["wx_direct"] as? Int { s.wxDirect = v == 1 }
            if let v = g["wx_status"] as? Int { s.wxStatus = v }
            if let v = g["wx_loc_set"] as? Int { s.wxLocSet = v == 1 }
        }
        // The lamp block is the tell: a display without one simply doesn't
        // send these, and the app then offers no lamp controls rather than
        // offering ones that would fail.
        if let v = obj["lamp_scene"] as? Int { s.lampScene = v; s.hasLamp = true }
        if let v = obj["lamp_auto"] as? Int { s.lampAuto = v == 1 }
        if let v = obj["lamp_pct"] as? Int { s.lampPct = v }
        if let v = obj["lamp_max_duty_pct"] as? Int { s.lampMaxDutyPct = v }
        if let v = obj["lamp_minutes"] as? Int { s.lampMinutes = v }
        if let v = obj["lamp_hue"] as? Int { s.lampHue = v }
        if let v = obj["orientation"] as? Int { s.orientation = v }
        if let v = obj["auto_rotate"] as? Int { s.autoRotate = v == 1 }
        if let v = obj["scenes"] as? [String] { s.scenes = v }
        return s
    }
}

enum GlassAPI {
    /// GET /api/settings — the request; GlassSettings.decode reads the body.
    static func settings(at base: URL, session: URLSession = .shared) async throws -> GlassSettings {
        guard DeviceAPI.isPrivate(base) else { throw DeviceError.notPrivateAddress }
        var req = URLRequest(url: base.appendingPathComponent("/api/settings"))
        req.timeoutInterval = 4
        let (data, resp) = try await session.data(for: req)
        guard let http = resp as? HTTPURLResponse, http.statusCode == 200 else {
            throw DeviceError.http((resp as? HTTPURLResponse)?.statusCode ?? 0, "settings")
        }
        return try GlassSettings.decode(data)
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
        // THE DEVICE'S OWN REFUSE-LIST HAS THE LAST WORD. GET /api/settings
        // lists the keys POST /api/set refuses for every caller under
        // `on_glass.keys` (the settings_policy.h table), and every knob built
        // below leaves through this filter — so the app structurally cannot
        // draw a control the glass would 403, including one a future
        // firmware moves into the class, and no key name is special-cased.
        func offered(_ knobs: [GlassKnob]) -> [GlassKnob] {
            knobs.filter { !s.onGlassKeys.contains($0.key) }
        }
        // ONE brightness control, and it is whichever one this glass can
        // actually obey.
        //
        // Every display serves `day_pct`, so the app offered it everywhere —
        // but on a panel whose backlight is a binary expander line it scales a
        // value that is only ever on or off, and dragging the slider changed
        // nothing the owner could see. Those boards dim by drawing a scrim
        // instead and say so by serving `bright_pct`; that is the knob to
        // show them. Rendering both would be two sliders that disagree, and
        // rendering only day_pct is what made the 7" look broken.
        let brightness: GlassKnob = s.hasRenderedDim
            ? GlassKnob(key: "bright_pct", title: "Daytime brightness",
                        blurb: "How bright the face is during the day. This screen's backlight "
                             + "is on or off in hardware, so the glass dims by drawing darker "
                             + "rather than by drawing less power — it bottoms out at "
                             + "\(s.brightMinPct)%, and going darker than that is the night "
                             + "window's job.",
                        kind: .percent(min: s.brightMinPct, max: 100), value: s.brightPct)
            : GlassKnob(key: "day_pct", title: "Daytime brightness",
                        blurb: "How bright the glass is during the day.",
                        kind: .percent(min: 20, max: 100), value: s.dayPct)
        var out: [GlassKnob] = [brightness]
        if s.hasLook {
            // The look ring, by the device's own names — same catalog the
            // on-glass flip-through walks, so phone and panel can never
            // disagree about what a look is called.
            out.append(GlassKnob(
                key: "character", title: "Face & colors",
                blurb: "The glass's curated look ring — ground, colors, type and the "
                     + "bird's temperament travel together as one named style. Alarms "
                     + "keep their own colors in every look, and night still outranks "
                     + "the style.",
                kind: .choice(labels: s.characterNames), value: s.character))
            if !s.clockStyleNames.isEmpty {
                out.append(GlassKnob(
                    key: "clock_style", title: "Clock face",
                    blurb: "How the time is drawn — the segment family or the analog "
                         + "dial. A face changes the drawing, never the message: the "
                         + "honesty lines and night behavior are unchanged.",
                    kind: .choice(labels: s.clockStyleNames), value: s.clockStyle))
            }
            out.append(GlassKnob(
                key: "clock_12h", title: "12-hour clock",
                blurb: "Twelve-hour digits with a quiet AM/PM beside the clock; "
                     + "off shows 24-hour time.",
                kind: .toggle, value: s.clock12h ? 1 : 0))
            out.append(GlassKnob(
                key: "orientation", title: "Orientation",
                blurb: "How the glass is turned. Portrait swaps the wall poster for "
                     + "the tall column face.",
                kind: .choice(labels: ["Landscape", "Portrait",
                                       "Landscape flipped", "Portrait flipped"]),
                value: s.orientation))
        }
        out.append(contentsOf: [
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
        ])
        guard s.hasLamp else { return offered(out) }
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
        return offered(out)
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

    // MARK: - the standalone-weather block, rendered as facts

    /// The state of the glass's own forecast, in the glass's own words — the
    /// Weather page's Status row and the Settings row on the panel
    /// (settings_ui.cpp), keyed by wx_direct.h's verdict. A body without a
    /// verdict (the cross-site shape) falls back to the on/off it did carry,
    /// the mirror page's rule (mirror_html.h). Never a control: the sheet
    /// says where the switch is, and it is not on the network.
    static func weatherStatusText(_ s: GlassSettings) -> String {
        switch s.wxStatus {
        case .some(0): return "Off"
        case .some(1): return "Needs a location"
        case .some(2): return "Your hub provides weather"
        case .some(3): return "On"
        case .some(4): return "On — last fetch failed, retrying"
        default:       return s.wxDirect ? "On" : "Off"
        }
    }

    /// Whether a coarse location is stored — never where. No route serves
    /// the grid point (it is an in-room disclosure on the glass alone), so
    /// there is nothing here to render; nil is "the glass did not say".
    static func weatherLocationText(_ s: GlassSettings) -> String {
        guard let stored = s.wxLocSet else { return "—" }
        return stored ? "Stored — a ~11 km grid point" : "Not set"
    }
}
