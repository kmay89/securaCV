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
        ]
        var s = GlassSettings()
        s.hasLamp = true
        for knob in GlassAPI.knobs(for: s) {
            XCTAssertTrue(accepted.contains(knob.key),
                          "the app offers “\(knob.key)”, which the glass's settings engine would reject")
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
