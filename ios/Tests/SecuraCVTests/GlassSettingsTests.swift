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
        // glass_web.cpp). A knob the app renders but the device rejects is a
        // control that silently does nothing — so the two lists are pinned to
        // each other here rather than discovered on somebody's nightstand.
        let accepted: Set<String> = [
            "day_pct", "night_screen", "red_shift", "peek_s",
            "night_start_hh", "night_end_hh", "night_step",
            "lamp_scene", "lamp_auto", "lamp_pct", "lamp_hue", "lamp_minutes",
            "clock_12h", "orientation", "auto_rotate",
            // Served and accepted only by glass that dims by scrim — see
            // the CD_FLAVOR_DASH block in handle_settings_set — which is
            // also the glass that serves the look ring and the clock ring.
            "bright_pct", "character", "clock_style",
            // The hub-less standalone-weather pair: the opt-in toggle, and
            // the coarse location as ONE combined integer (atomic store).
            // Since the display's on-glass-only key class landed, the firmware
            // REFUSES both from the LAN (403 on_glass_only) and reports them
            // read-only under `on_glass`; they stay in this list only until the
            // weather sheet is reworked to read that block (roadmap item 17
            // follow-up), and against current firmware the sheet never shows
            // them because the top-level `wx_direct` key is no longer served.
            "wx_direct", "wx_loc",
        ]
        var s = GlassSettings()
        s.hasLamp = true
        for knob in GlassAPI.knobs(for: s) {
            XCTAssertTrue(accepted.contains(knob.key),
                          "the app offers “\(knob.key)”, which the glass's settings engine would reject")
        }
        // And the same for the other kind of glass, whose brightness knob is
        // a different key entirely — and which carries the look ring.
        var scrim = GlassSettings()
        scrim.hasRenderedDim = true
        scrim.hasLook = true
        scrim.characterNames = ["Quiet Glass"]
        scrim.clockStyleNames = ["Segment"]
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

    /// The standalone-weather switch appears only when the device serves it,
    /// and its story changes honestly when a hub owns weather.
    func testDirectWeatherKnobFollowsTheDevicesAnswer() {
        var s = GlassSettings()
        s.hasDirectWeather = true
        s.wxDirect = false
        s.wxHub = false
        let knob = GlassAPI.knobs(for: s).first { $0.key == "wx_direct" }
        XCTAssertNotNil(knob, "a glass that serves wx_direct gets the switch")
        XCTAssertEqual(knob?.value, 0, "off by default — the opt-in is real")

        var hub = GlassSettings()
        hub.hasDirectWeather = true
        hub.wxHub = true
        let hubKnob = GlassAPI.knobs(for: hub).first { $0.key == "wx_direct" }
        XCTAssertEqual(hubKnob?.blurb.contains("hub") , true,
                       "with a hub, the blurb says why the fetcher stands down")

        let none = GlassAPI.knobs(for: GlassSettings())
        XCTAssertFalse(none.contains { $0.key == "wx_direct" },
                       "a glass that never mentioned wx_direct gets no switch")
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
