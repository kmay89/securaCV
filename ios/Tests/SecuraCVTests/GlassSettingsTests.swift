// GlassSettingsTests.swift
//
// The settings surface is "the device describes, the app renders", so what
// gets tested is the rendering decisions — which knobs a given display
// offers, and what value actually goes on the wire. Both are places where a
// plausible-looking bug is invisible until somebody's glass ignores a tap.
//
// The lesson from the certificate PR applies here and shapes the last test:
// it is not enough that a value is well-formed. Something has to assert the
// app sends what the device's own contract accepts.

import XCTest
@testable import SecuraCV

final class GlassSettingsTests: XCTestCase {

    // MARK: - a display offers what it actually has

    func testADisplayWithoutALampOffersNoLampControls() {
        var s = GlassSettings()
        s.hasLamp = false
        let keys = GlassAPI.knobs(for: s).map(\.key)
        XCTAssertFalse(keys.contains { $0.hasPrefix("lamp_") },
                       "a Watch Station has no lamp — offering the control would be a tap that fails")
        XCTAssertTrue(keys.contains("day_pct"), "but every display has a screen")
        XCTAssertTrue(keys.contains("night_start_hh"), "and every display has a night")
    }

    func testADisplayWithALampOffersItsLampControls() {
        var s = GlassSettings()
        s.hasLamp = true
        let keys = GlassAPI.knobs(for: s).map(\.key)
        for key in ["lamp_pct", "lamp_minutes", "lamp_auto"] {
            XCTAssertTrue(keys.contains(key), "missing \(key)")
        }
    }

    func testTheCeilingShownIsTheOneTheGlassReported() {
        var s = GlassSettings()
        s.hasLamp = true
        s.lampMaxDutyPct = 35            // a stricter device than the default
        let lamp = GlassAPI.knobs(for: s).first { $0.key == "lamp_pct" }
        XCTAssertEqual(lamp?.blurb.contains("35%"), true,
                       "the heat ceiling is the device's fact, not a number the app decided")
    }

    // MARK: - what goes on the wire

    func testPeekIsSentInSecondsNotAsAPickerIndex() {
        // The control is a three-way picker; the device's contract is
        // `peek_s` in {3,5,10}. Sending the index would be silently rejected
        // — a 400 the user reads as "the setting didn't stick".
        var s = GlassSettings()
        s.peekSeconds = 10
        let knob = GlassAPI.knobs(for: s).first { $0.key == "peek_s" }
        XCTAssertEqual(knob?.value, 2, "10 seconds is the third choice")
        XCTAssertEqual(GlassAPI.wireValue(for: knob!), 10, "but 10 is what the glass accepts")

        var three = knob!
        three.value = 0
        XCTAssertEqual(GlassAPI.wireValue(for: three), 3)
        var five = knob!
        five.value = 1
        XCTAssertEqual(GlassAPI.wireValue(for: five), 5)
    }

    func testEveryOtherKnobSendsItsValueUnchanged() {
        var s = GlassSettings()
        s.hasLamp = true
        for knob in GlassAPI.knobs(for: s) where knob.key != "peek_s" {
            XCTAssertEqual(GlassAPI.wireValue(for: knob), knob.value, knob.key)
        }
    }

    // MARK: - every knob the app offers is one the firmware accepts

    func testTheAppOnlyOffersKnobsTheGlassValidates() {
        // The firmware's handle_settings_set accepts exactly this set (see
        // glass_web.cpp): it is test_settings_policy.cpp's kOrdinary minus
        // `tz`, which the app writes through /api/tz, never /api/set. A knob
        // the app renders but the device rejects is a control that silently
        // does nothing — so the two lists are pinned to each other here
        // rather than discovered on somebody's nightstand. The two keys the
        // glass REFUSES from the network (wx_direct and wx_loc, 403
        // on_glass_only — settings_policy.h) are deliberately not here:
        // they are rendered as facts, never offered (the tests below).
        let accepted: Set<String> = [
            "day_pct", "night_screen", "red_shift", "peek_s",
            "night_start_hh", "night_end_hh", "night_step",
            "lamp_scene", "lamp_auto", "lamp_pct", "lamp_hue", "lamp_minutes",
            "clock_12h", "orientation", "auto_rotate",
            // Served and accepted only by glass that dims by scrim — see
            // the CD_FLAVOR_DASH block in handle_settings_set — which is
            // also the glass that serves the look ring and the clock ring.
            "bright_pct", "character", "clock_style",
        ]
        var s = GlassSettings()
        s.hasLamp = true
        for knob in GlassAPI.knobs(for: s) {
            XCTAssertTrue(accepted.contains(knob.key),
                          "the app offers “\(knob.key)”, which the glass's settings engine would reject")
        }
        // And the same for the other kind of glass, whose brightness knob is
        // a different key entirely — and which carries the look ring and, on
        // the 7" builds, the read-only on_glass block with its refuse-list.
        var scrim = GlassSettings()
        scrim.hasRenderedDim = true
        scrim.hasLook = true
        scrim.characterNames = ["Quiet Glass"]
        scrim.clockStyleNames = ["Segment"]
        scrim.hasDirectWeather = true
        scrim.wxDirect = true
        scrim.onGlassKeys = ["wx_direct", "wx_loc"]
        for knob in GlassAPI.knobs(for: scrim) {
            XCTAssertTrue(accepted.contains(knob.key),
                          "the app offers “\(knob.key)”, which the glass's settings engine would reject")
        }
    }

    // MARK: - the look ring mirrors the on-glass settings

    /// The 7" gap, pinned from the other side: `character` (the face/color
    /// ring) and `clock_style` were on-glass-only, so the app could not
    /// change a display's face. The device now serves both BY NAME and the
    /// app renders its catalog — never a hard-coded list.
    func testGlassThatServesALookRingGetsFaceAndClockKnobs() {
        var s = GlassSettings()
        s.hasLook = true
        s.character = 2
        s.characterNames = ["Quiet Glass", "Heirloom", "Aqua"]
        s.clockStyle = 1
        s.clockStyleNames = ["Segment", "Slab", "Hairline", "Analog"]
        s.orientation = 1
        let byKey = Dictionary(uniqueKeysWithValues: GlassAPI.knobs(for: s).map { ($0.key, $0) })

        guard case .choice(let faces)? = byKey["character"]?.kind else {
            return XCTFail("character should be a choice")
        }
        XCTAssertEqual(faces, s.characterNames,
                       "the labels are the device's own names, not the app's guesses")
        XCTAssertEqual(byKey["character"]?.value, 2)

        guard case .choice(let clocks)? = byKey["clock_style"]?.kind else {
            return XCTFail("clock_style should be a choice")
        }
        XCTAssertEqual(clocks, s.clockStyleNames)
        XCTAssertEqual(byKey["clock_style"]?.value, 1)

        // The dash glass also mirrors its orientation editor.
        XCTAssertEqual(byKey["orientation"]?.value, 1)
    }

    // MARK: - the standalone-weather block: shown, never offered

    /// GET /api/settings from a 7" glass (dash7 / nightstand7,
    /// FEATURE_STANDALONE_WEATHER) on a same-site request, in the shape and
    /// key order glass_web.cpp handle_settings_get writes it: the ordinary
    /// knobs, the zone and the per-boot token (32 lowercase hex — the value
    /// is random per boot, so the fixture's is representative, the rest
    /// are a real glass's), the scrim block, the look ring, then `on_glass`.
    /// `keys` is the policy table itself (settings_policy.h); `wx_direct`
    /// goes to every caller; `wx_loc_set` and `wx_status` only to a caller
    /// that is not cross-site, which a URLSession GET from the app is not.
    static let dash7SameSiteBody = #"""
    {"day_pct":60,"night_screen":0,"red_shift":1,"peek_s":5,"night_start_hh":20,"night_end_hh":7,"night_step":2,"night_steps":10,"tz":"CST6CDT,M3.2.0,M11.1.0","csrf":"3fa9c0de1b2c4d5e6f708192a3b4c5d6","bright_pct":80,"bright_min_pct":50,"orientation":0,"character":0,"clock_style":0,"characters":["Quiet Glass"],"clock_styles":["Segment"],"on_glass":{"keys":["wx_direct","wx_loc"],"wx_direct":1,"wx_loc_set":0,"wx_status":1}}
    """#

    /// The same route on a nightlight (glass_web.cpp, CD_NIGHTLIGHT), in its
    /// key order: the lamp block and the scene catalog by display name
    /// (look_engine.cpp kScenes[].name, all eleven) — and no `on_glass`
    /// block, because a lamp never carries the standalone forecast.
    static let nightlightBody = #"""
    {"day_pct":60,"night_screen":0,"red_shift":1,"peek_s":5,"night_start_hh":20,"night_end_hh":7,"night_step":2,"night_steps":10,"tz":"UTC0","csrf":"3fa9c0de1b2c4d5e6f708192a3b4c5d6","lamp_scene":1,"lamp_auto":1,"lamp_pct":72,"lamp_max_duty_pct":50,"clock_12h":1,"orientation":0,"auto_rotate":1,"lamp_hue":-1,"lamp_minutes":15,"scenes":["Canary Dawn","Ember","Aurora","Deep Calm","Forest","Tropical","Lantern","Nocturne","Signal","Rainbow","Moonbeam"]}
    """#

    /// The keys the display refuses from the network for every caller —
    /// settings_policy.h's kOnGlassOnlyKeys, pinned to exactly these two by
    /// test_settings_policy.cpp. The Swift side of the same contract.
    static let refusedByTheGlass: Set<String> = ["wx_direct", "wx_loc"]

    /// The `on_glass` block is the tell, and everything in it is a fact the
    /// sheet shows — never a control. The handler answers 403 on_glass_only
    /// to these keys before it looks at anything else (glass_web.cpp
    /// handle_settings_set), so a knob keyed by one could only ever fail.
    func testTheOnGlassBlockIsTheTellAndIsReadOnly() throws {
        let s = try GlassSettings.decode(Data(Self.dash7SameSiteBody.utf8))
        XCTAssertTrue(s.hasDirectWeather, "the on_glass block is the tell")
        XCTAssertTrue(s.wxDirect)
        XCTAssertEqual(s.wxStatus, 1, "the verdict rides on a same-site request")
        XCTAssertEqual(s.wxLocSet, false, "and so does whether a grid point is stored")
        XCTAssertEqual(s.onGlassKeys, ["wx_direct", "wx_loc"],
                       "keys is the policy table itself (settings_policy.h)")

        let offered = Set(GlassAPI.knobs(for: s).map(\.key))
        XCTAssertTrue(offered.isDisjoint(with: Set(s.onGlassKeys)),
                      "the app drew a control the glass refuses: \(offered.intersection(s.onGlassKeys))")
        XCTAssertEqual(GlassAPI.weatherStatusText(s), "Needs a location")
        XCTAssertEqual(GlassAPI.weatherLocationText(s), "Not set")

        // The rest of the body still lands where it belongs.
        XCTAssertTrue(s.hasRenderedDim)
        XCTAssertEqual(s.brightPct, 80)
        XCTAssertEqual(s.brightMinPct, 50)
        XCTAssertTrue(s.hasLook)
        XCTAssertEqual(s.characterNames, ["Quiet Glass"])
        XCTAssertEqual(s.clockStyleNames, ["Segment"])
        XCTAssertTrue(offered.contains("bright_pct"))
        XCTAssertTrue(offered.contains("character"))
    }

    /// No firmware in this checkout's history (the commits back to b4a9083)
    /// ever served wx_direct at the top level — the only served shape is the
    /// nested block above. A top-level key is exactly what a client renders
    /// as a control, so the shape that was never served must not conjure
    /// the block: it would draw a switch the glass refuses.
    func testATopLevelWxDirectIsNotTheTell() throws {
        let json = #"{"day_pct":60,"wx_direct":1,"wx_loc_set":1}"#
        let s = try GlassSettings.decode(Data(json.utf8))
        XCTAssertFalse(s.hasDirectWeather, "a shape no firmware served is not a tell")
        XCTAssertFalse(s.wxDirect)
        XCTAssertNil(s.wxLocSet)
        XCTAssertTrue(s.onGlassKeys.isEmpty)
        XCTAssertFalse(GlassAPI.knobs(for: s).contains { $0.key == "wx_direct" })
        XCTAssertEqual(s.dayPct, 60, "the ordinary key beside it is still read")
    }

    /// A cross-site or foreign-host request gets the block without the two
    /// location-derived facts (glass_web.cpp — the same rule /api/fleet
    /// applies to presence). The sheet then says nothing about a location
    /// rather than guessing, and falls back to the on/off it was given —
    /// the mirror page's rule (mirror_html.h).
    func testTheCrossSiteShapeLeavesLocationUnknown() throws {
        let offJSON = #"{"on_glass":{"keys":["wx_direct","wx_loc"],"wx_direct":0}}"#
        let off = try GlassSettings.decode(Data(offJSON.utf8))
        XCTAssertTrue(off.hasDirectWeather)
        XCTAssertFalse(off.wxDirect)
        XCTAssertNil(off.wxStatus)
        XCTAssertNil(off.wxLocSet)
        XCTAssertEqual(GlassAPI.weatherLocationText(off), "—")
        XCTAssertEqual(GlassAPI.weatherStatusText(off), "Off")

        let onJSON = #"{"on_glass":{"keys":["wx_direct","wx_loc"],"wx_direct":1}}"#
        let on = try GlassSettings.decode(Data(onJSON.utf8))
        XCTAssertTrue(on.wxDirect)
        XCTAssertEqual(GlassAPI.weatherStatusText(on), "On")
        XCTAssertEqual(GlassAPI.weatherLocationText(on), "—")
    }

    /// wx_direct.h's verdict, rendered in the words the panel's own Weather
    /// page uses (settings_ui.cpp build_weather) — so the phone and the
    /// glass never disagree about what state the forecast is in.
    func testWeatherStatusWordsAreTheGlasssOwn() {
        var s = GlassSettings()
        s.hasDirectWeather = true
        let expected: [Int: String] = [
            0: "Off",
            1: "Needs a location",
            2: "Your hub provides weather",
            3: "On",
            4: "On — last fetch failed, retrying",
        ]
        for (status, words) in expected {
            s.wxStatus = status
            XCTAssertEqual(GlassAPI.weatherStatusText(s), words, "wx_status \(status)")
        }
        s.wxStatus = 2
        XCTAssertTrue(GlassAPI.weatherStatusText(s).contains("hub"),
                      "with a hub, the sheet says why the fetcher stands down")

        // A stored location is said to be stored — never where. The API
        // does not carry the grid point, so the sheet has nothing to show.
        s.wxLocSet = true
        XCTAssertEqual(GlassAPI.weatherLocationText(s), "Stored — a ~11 km grid point")
        s.wxLocSet = false
        XCTAssertEqual(GlassAPI.weatherLocationText(s), "Not set")
    }

    /// The Swift mirror of test_settings_policy.cpp: whatever shape the glass
    /// has, no offered knob is in the refused class. The filter in
    /// GlassAPI.knobs uses the device's own list, so this holds for a key
    /// the glass moves into the class tomorrow, without the app learning
    /// its name.
    func testNoOfferedKnobIsOneTheGlassRefuses() {
        var lamp = GlassSettings()
        lamp.hasLamp = true
        lamp.hasDirectWeather = true
        lamp.onGlassKeys = ["wx_direct", "wx_loc"]
        XCTAssertTrue(Set(GlassAPI.knobs(for: lamp).map(\.key))
                        .isDisjoint(with: Self.refusedByTheGlass))

        var dash7 = GlassSettings()
        dash7.hasRenderedDim = true
        dash7.hasLook = true
        dash7.characterNames = ["Quiet Glass"]
        dash7.clockStyleNames = ["Segment"]
        dash7.hasDirectWeather = true
        dash7.wxDirect = true
        dash7.onGlassKeys = ["wx_direct", "wx_loc"]
        XCTAssertTrue(Set(GlassAPI.knobs(for: dash7).map(\.key))
                        .isDisjoint(with: Self.refusedByTheGlass))

        // The list is the device's, not the app's.
        var future = dash7
        future.onGlassKeys = ["wx_direct", "wx_loc", "clock_12h"]
        let keys = GlassAPI.knobs(for: future).map(\.key)
        XCTAssertFalse(keys.contains("clock_12h"),
                       "a key the glass moves into the class leaves the sheet the same day")
        XCTAssertTrue(keys.contains("bright_pct"), "and the ordinary knobs stay")

        // The lamp's knobs leave through the OTHER return site (the one
        // after the lamp block), and nothing above reaches it with a key
        // it actually builds — so the same probe, on the lamp path. Drop
        // that one filter and this is the line that goes red.
        var lampFuture = lamp
        lampFuture.onGlassKeys = ["wx_direct", "wx_loc", "lamp_auto"]
        let lampKeys = GlassAPI.knobs(for: lampFuture).map(\.key)
        XCTAssertFalse(lampKeys.contains("lamp_auto"),
                       "the lamp path's knobs leave through the same filter")
        XCTAssertTrue(lampKeys.contains("lamp_pct"), "and the rest of the lamp stays")

        // And a glass that serves no block at all offers no weather control
        // either — there is nothing to filter and nothing to draw.
        XCTAssertTrue(Set(GlassAPI.knobs(for: GlassSettings()).map(\.key))
                        .isDisjoint(with: Self.refusedByTheGlass))
    }

    /// "Tolerant by hand" (the header of GlassSettings.swift), now that the
    /// decoder is reachable: a key this build never heard of is ignored and
    /// everything keeps its default — including the weather block, which is
    /// absent, not guessed from a look-alike.
    func testDecodeIgnoresUnknownAndKeepsDefaults() throws {
        let json = #"{"future_key":1,"on_glass_soon":{"keys":["wx_direct"],"wx_direct":1}}"#
        let s = try GlassSettings.decode(Data(json.utf8))
        XCTAssertEqual(s, GlassSettings())
        XCTAssertFalse(s.hasDirectWeather)
        XCTAssertEqual(GlassAPI.knobs(for: s).map(\.key),
                       GlassAPI.knobs(for: GlassSettings()).map(\.key))
    }

    /// The nightlight's own bytes through the same decoder: the lamp block
    /// lands, the scene catalog is the device's, and there is no weather
    /// block to show — the location row would read "—" if anything asked.
    func testDecodeReadsTheNightlightsOwnBytes() throws {
        let s = try GlassSettings.decode(Data(Self.nightlightBody.utf8))
        XCTAssertTrue(s.hasLamp)
        XCTAssertEqual(s.lampScene, 1)
        XCTAssertEqual(s.lampPct, 72)
        XCTAssertEqual(s.lampMaxDutyPct, 50)
        XCTAssertEqual(s.lampMinutes, 15)
        XCTAssertEqual(s.lampHue, -1)
        XCTAssertFalse(s.usesCustomHue)
        XCTAssertEqual(s.scenes.count, 11, "the look engine's catalog, by display name")
        XCTAssertEqual(s.scenes.first, "Canary Dawn")
        XCTAssertEqual(s.scenes.last, "Moonbeam")
        XCTAssertTrue(s.clock12h)
        XCTAssertTrue(s.autoRotate)
        XCTAssertFalse(s.hasRenderedDim)
        XCTAssertFalse(s.hasLook)
        XCTAssertFalse(s.hasDirectWeather)
        XCTAssertTrue(s.onGlassKeys.isEmpty)
        XCTAssertNil(s.wxStatus)
        XCTAssertEqual(GlassAPI.weatherLocationText(s), "—")
    }

    func testGlassWithoutALookRingOffersNoLookKnobs() {
        // A Watch Station or a nightlight never serves `characters`; the
        // knobs would be taps that 400. Same rule as the lamp block.
        var s = GlassSettings()
        s.hasLamp = true
        let keys = GlassAPI.knobs(for: s).map(\.key)
        XCTAssertFalse(keys.contains("character"))
        XCTAssertFalse(keys.contains("clock_style"))
    }

    // MARK: - one brightness control, and it is the one that works

    /// The 7" bug, pinned.
    ///
    /// That panel's backlight is a CH422G expander line — binary in hardware,
    /// so `backlight_set()` is `level > 0`. `day_pct` scales a value that can
    /// only ever be on or off, which is why dragging the app's brightness
    /// slider changed nothing an owner could see. Glass like that dims by
    /// drawing a scrim and says so by serving `bright_pct`.
    func testGlassThatDimsByScrimGetsTheKnobThatMovesIt() {
        var s = GlassSettings()
        s.hasRenderedDim = true
        s.brightPct = 70
        s.brightMinPct = 50
        let keys = GlassAPI.knobs(for: s).map(\.key)

        XCTAssertTrue(keys.contains("bright_pct"))
        XCTAssertFalse(keys.contains("day_pct"),
                       "day_pct does nothing on this hardware — offering it too would put two "
                       + "brightness sliders on one screen, one of which silently does nothing")
        // Exactly one control called brightness, wherever it came from.
        XCTAssertEqual(GlassAPI.knobs(for: s).filter { $0.title.contains("brightness") }.count, 1)

        guard let knob = GlassAPI.knobs(for: s).first(where: { $0.key == "bright_pct" }) else {
            return XCTFail("bright_pct should be offered")
        }
        XCTAssertEqual(knob.value, 70)
        // The floor is the glass's own, not one the app invented: below it the
        // scrim is Night's job, and the device says where that line is.
        if case .percent(let min, let max) = knob.kind {
            XCTAssertEqual(min, 50)
            XCTAssertEqual(max, 100)
        } else { XCTFail("bright_pct should be a percent") }
    }

    /// A display with a genuinely dimmable backlight keeps day_pct, and never
    /// grows a second brightness control.
    func testGlassWithARealBacklightKeepsDayPct() {
        var s = GlassSettings()
        s.dayPct = 60
        let keys = GlassAPI.knobs(for: s).map(\.key)
        XCTAssertTrue(keys.contains("day_pct"))
        XCTAssertFalse(keys.contains("bright_pct"),
                       "a device that didn't report bright_pct hasn't got one")
    }

    /// The tell is the device's own answer, never an inference from a product
    /// name — same rule as the lamp block beside it.
    func testTheScrimTellComesFromTheDeviceNotFromAGuess() {
        XCTAssertFalse(GlassSettings().hasRenderedDim,
                       "a display that said nothing about bright_pct hasn't got one")
        // Both brightness values go on the wire unchanged — only peek_s is
        // translated (see wireValue).
        var s = GlassSettings()
        s.hasRenderedDim = true
        s.brightPct = 80
        for knob in GlassAPI.knobs(for: s) where knob.key == "bright_pct" {
            XCTAssertEqual(GlassAPI.wireValue(for: knob), 80)
        }
    }

    func testTheKnobRangesMatchTheFirmwaresValidation() {
        // Ranges are the other half of the same contract: an app slider that
        // can reach 10% when the device floors at 20% produces a tap that
        // does nothing and a user who stops trusting the screen.
        var s = GlassSettings()
        s.hasLamp = true
        let byKey = Dictionary(uniqueKeysWithValues: GlassAPI.knobs(for: s).map { ($0.key, $0) })
        if case .percent(let min, let max)? = byKey["day_pct"]?.kind {
            XCTAssertEqual(min, 20); XCTAssertEqual(max, 100)
        } else { XCTFail("day_pct should be a percent") }
        if case .percent(let min, let max)? = byKey["lamp_pct"]?.kind {
            XCTAssertEqual(min, 10); XCTAssertEqual(max, 100)
        } else { XCTFail("lamp_pct should be a percent") }
        if case .minutes(let min, let max)? = byKey["lamp_minutes"]?.kind {
            XCTAssertEqual(max, 480, "the glass accepts up to 8 hours")
            XCTAssertEqual(min, 1,
                           "LanternModel clamps anything under a minute up to one, so a zero here "
                           + "would promise an untimed lamp and deliver a 60-second one")
        } else { XCTFail("lamp_minutes should be minutes") }
    }

    // MARK: - the device is the authority on which look is on

    func testACustomHueIsRecognizedFromTheDevicesAnswer() {
        var s = GlassSettings()
        s.lampHue = -1
        XCTAssertFalse(s.usesCustomHue, "-1 means a catalog scene is on")
        s.lampHue = 0
        XCTAssertTrue(s.usesCustomHue, "hue 0 is red, not 'no hue' — an off-by-one here loses red")
        s.lampHue = 359
        XCTAssertTrue(s.usesCustomHue)
    }
}

// ── The wiring, not just the client ─────────────────────────────────────────
//
// The gap this guards is the one the previous PR shipped: a correct API that
// no production code calls. A settings client with tests and no screen is a
// feature nobody can reach, and it passes every other test in this file.
extension GlassSettingsTests {

    func testEveryDisplayTypeOffersTheSettingsScreen() {
        // The whole point of the change: the controls used to appear only on
        // a nightlight, so a Watch Station or a Dash served a brightness and
        // a night window to nobody.
        XCTAssertTrue(DeviceType.display.servesGlassSettings)
        XCTAssertTrue(DeviceType.nightlight.servesGlassSettings)
    }

    func testAWitnessWithNoGlassOffersNoScreenSettings() {
        for type in [DeviceType.wap, .vision, .sense, .unknown] {
            XCTAssertFalse(type.servesGlassSettings,
                           "\(type) has no screen — the row would be a tap that 404s")
        }
    }
}
