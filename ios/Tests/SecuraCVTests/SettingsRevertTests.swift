// SettingsRevertTests.swift
//
// The undo under the glass settings sheet, held still: only drifted knobs
// are written back, knobs that appeared or vanished mid-session are left
// alone, and a clean session offers nothing to undo.

import XCTest
@testable import SecuraCV

final class SettingsRevertTests: XCTestCase {
    private func knob(_ key: String, _ value: Int) -> GlassKnob {
        GlassKnob(key: key, title: key, blurb: "", kind: .toggle, value: value)
    }

    func testACleanSessionHasNothingToUndo() {
        let knobs = [knob("day_pct", 60), knob("red_shift", 1)]
        XCTAssertTrue(SettingsRevert.writes(toRestore: knobs, from: knobs).isEmpty)
    }

    func testOnlyDriftedKnobsAreWrittenBack() {
        let before = [knob("day_pct", 60), knob("red_shift", 1), knob("clock_12h", 0)]
        let after = [knob("day_pct", 90), knob("red_shift", 1), knob("clock_12h", 1)]
        let writes = SettingsRevert.writes(toRestore: before, from: after)
        XCTAssertEqual(writes.map(\.key), ["day_pct", "clock_12h"])
        // The values written are the SNAPSHOT's — where the session began.
        XCTAssertEqual(writes.map(\.value), [60, 0])
    }

    func testAKnobTheDeviceStoppedReportingIsSkipped() {
        let before = [knob("day_pct", 60), knob("lamp_pct", 40)]
        let after = [knob("day_pct", 60)]
        XCTAssertTrue(SettingsRevert.writes(toRestore: before, from: after).isEmpty)
    }

    func testAKnobTheDeviceGainedMidSessionIsSkipped() {
        let before = [knob("day_pct", 60)]
        let after = [knob("day_pct", 60), knob("lamp_pct", 40)]
        XCTAssertTrue(SettingsRevert.writes(toRestore: before, from: after).isEmpty,
                      "a knob with no snapshot value has nothing honest to restore")
    }

    func testSnapshotOrderIsPreserved() {
        // The replay walks the device back in the order the sheet listed
        // things — deterministic, so a failure names a predictable knob.
        let before = [knob("a", 1), knob("b", 2), knob("c", 3)]
        let after = [knob("c", 9), knob("b", 8), knob("a", 7)]
        XCTAssertEqual(SettingsRevert.writes(toRestore: before, from: after).map(\.key),
                       ["a", "b", "c"])
    }
}
