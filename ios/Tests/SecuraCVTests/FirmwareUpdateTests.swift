// FirmwareUpdateTests.swift
//
// The software-update model (FirmwareUpdate.swift) against the bodies the
// two firmware families actually print: the display's hand-built JSON
// (glass_web.cpp handle_ota_status) and the WAP's ArduinoJson document
// (canary_wap.ino handle_ota_status). scripts/tests/test_ios_firmware_update.py
// holds the same field names and state words to those sources on Linux.

import XCTest
@testable import SecuraCV

final class FirmwareUpdateTests: XCTestCase {

    private func status(_ json: String) throws -> FirmwareStatus {
        try JSONDecoder().decode(FirmwareStatus.self, from: Data(json.utf8))
    }

    // MARK: - decode

    func testADisplayStatusDecodes() throws {
        let s = try status("""
        {"ok":true,"installed_version":"2.4.18","state":"Idle","state_text":"","progress":0,\
        "error":"No error","error_text":"","update_available":true,"auto_update":false,\
        "latest_version":"2.4.19"}
        """)
        XCTAssertEqual(s.installed, "2.4.18")
        XCTAssertEqual(s.latest, "2.4.19")
        XCTAssertTrue(s.updateAvailable)
        XCTAssertEqual(s.autoUpdate, false)
        XCTAssertNil(s.releaseNotes, "a display serves no notes")
        XCTAssertEqual(FirmwareUpdate.phase(s), .available("2.4.19"))
        XCTAssertNil(FirmwareUpdate.lingeringNote(s), "No error is not a note")
    }

    func testAWapStatusDecodesWithItsNotes() throws {
        let s = try status("""
        {"ok":true,"installed_version":"2.4.18","state":"Idle","state_text":"","progress":0,\
        "error":"No error","error_text":"","update_available":true,"auto_update":true,\
        "local_http_allowed":false,"last_check":1730000000,\
        "manifest_url":"https://example.invalid/m.json","manifest_url_is_override":false,\
        "latest_version":"2.4.19","release_notes":"Faster boot.","release_url":"https://example.invalid/r"}
        """)
        XCTAssertEqual(s.releaseNotes, "Faster boot.")
        XCTAssertEqual(FirmwareUpdate.phase(s), .available("2.4.19"),
                       "unknown keys (manifest_url, last_check) are ignored, never fatal")
    }

    func testAnOlderOrSparseBodyStillDecodes() throws {
        let s = try status(#"{"installed_version":"2.4.10"}"#)
        XCTAssertEqual(s.state, "Idle")
        XCTAssertEqual(FirmwareUpdate.phase(s), .notChecked,
                       "no latest_version yet means it hasn't checked this boot")
    }

    // MARK: - phase

    func testIdleAfterACheckWithNothingNewerIsUpToDate() throws {
        // securacv_ota.cpp: up to date sets last_error NONE and state IDLE.
        let s = try status(#"{"installed_version":"2.4.19","latest_version":"2.4.19","state":"Idle","error":"No error","update_available":false}"#)
        XCTAssertEqual(FirmwareUpdate.phase(s), .upToDate)
    }

    func testAFailedFirstCheckIsAFailureNotNeverChecked() throws {
        // ota_set_error() records the reason and drops the engine straight
        // back to Idle, and there is no manifest — review on the first cut:
        // that read as "Check for update", with the reason dropped.
        let s = try status(#"{"installed_version":"2.4.18","state":"Idle","error":"Manifest fetch failed","error_text":"Could not reach the update server. Check the network and try again.","update_available":false}"#)
        XCTAssertEqual(FirmwareUpdate.phase(s),
                       .failed("Could not reach the update server. Check the network and try again."))
        XCTAssertNil(FirmwareUpdate.lingeringNote(s), "said once, by the failure line")
    }

    func testTheNoUpdateWordIsUpToDateEvenWithoutAManifest() throws {
        let s = try status(#"{"installed_version":"2.4.19","state":"Idle","error":"No update available","error_text":"Your Canary is up to date."}"#)
        XCTAssertEqual(FirmwareUpdate.phase(s), .upToDate)
        XCTAssertNil(FirmwareUpdate.lingeringNote(s), "up to date is not a note")
    }

    func testWorkingStepsCarryProgressOnlyWhereTheEngineReportsIt() throws {
        let dl = try status(#"{"state":"Downloading","progress":42}"#)
        XCTAssertEqual(FirmwareUpdate.phase(dl), .working(.downloading, progress: 42))
        let fl = try status(#"{"state":"Flashing","progress":140}"#)
        XCTAssertEqual(FirmwareUpdate.phase(fl), .working(.flashing, progress: 100), "clamped to 100")
        let ck = try status(#"{"state":"Checking","progress":7}"#)
        XCTAssertEqual(FirmwareUpdate.phase(ck), .working(.checking, progress: nil))
        let vf = try status(#"{"state":"Verifying"}"#)
        XCTAssertEqual(FirmwareUpdate.phase(vf), .working(.verifying, progress: nil))
        XCTAssertEqual(FirmwareUpdate.phase(try status(#"{"state":"Rebooting"}"#)), .restarting)
    }

    func testAFailedRunShowsTheDevicesOwnSentence() throws {
        let s = try status(#"{"state":"Error","error":"SHA-256 mismatch","error_text":"The update file failed its security check, so it was not installed."}"#)
        XCTAssertEqual(FirmwareUpdate.phase(s),
                       .failed("The update file failed its security check, so it was not installed."))
        XCTAssertNil(FirmwareUpdate.lingeringNote(s), "the failure line already says it")
        let bare = try status(#"{"state":"Error","error":"Unknown error"}"#)
        if case .failed(let line) = FirmwareUpdate.phase(bare) {
            XCTAssertTrue(line.contains("still on its current software"), "a plain fallback, never blank")
        } else {
            XCTFail("an Error state is a failure")
        }
    }

    func testAnIdleStateKeepsTheLastRefusalAsANote() throws {
        let s = try status(#"{"state":"Idle","latest_version":"2.4.19","update_available":false,"error":"Manifest is for another board or chip","error_text":"That update is for a different kind of Canary, so it was not installed."}"#)
        XCTAssertEqual(FirmwareUpdate.lingeringNote(s),
                       "That update is for a different kind of Canary, so it was not installed.")
    }

    // MARK: - kicks

    func testBothFamiliesBusyAnswersAreBusy() {
        XCTAssertEqual(FirmwareUpdate.kick(status: 409, body: Data(#"{"ok":false,"error":"ota_busy"}"#.utf8)),
                       .busy, "the display's 409")
        XCTAssertEqual(FirmwareUpdate.kick(status: 400, body: Data(#"{"ok":false,"error":"ota_busy"}"#.utf8)),
                       .busy, "the WAP's 400 ota_busy")
        if case .refused = FirmwareUpdate.kick(status: 400, body: Data(#"{"ok":false,"error":"bad_json"}"#.utf8)) {
        } else {
            XCTFail("a 400 that is not ota_busy is a refusal")
        }
    }

    func testStartedUnsupportedAndRefusals() {
        XCTAssertEqual(FirmwareUpdate.kick(status: 200, body: Data(#"{"ok":true,"message":"Checking for updates..."}"#.utf8)), .started)
        XCTAssertEqual(FirmwareUpdate.kick(status: 404, body: Data("not here".utf8)), .unsupported,
                       "firmware older than the routes")
        for code in [401, 403, 500] {
            if case .refused(let line) = FirmwareUpdate.kick(status: code, body: Data()) {
                XCTAssertFalse(line.isEmpty, "\(code)")
            } else {
                XCTFail("\(code) is a refusal")
            }
        }
        XCTAssertEqual(FirmwareUpdate.errorWord(Data(#"{"ok":false,"err":"origin"}"#.utf8)), "origin",
                       "the display's guard says err, not error")
    }

    // MARK: - polling and the restart

    func testThePhonePollsOnlyWhileARunIsInFlight() {
        XCTAssertEqual(FirmwareUpdate.pollInterval(.working(.downloading, progress: 3)), .seconds(1))
        XCTAssertEqual(FirmwareUpdate.pollInterval(.restarting), .seconds(3))
        XCTAssertNil(FirmwareUpdate.pollInterval(.upToDate))
        XCTAssertNil(FirmwareUpdate.pollInterval(.available("2.4.19")))
        XCTAssertNil(FirmwareUpdate.pollInterval(.notChecked))
        XCTAssertNil(FirmwareUpdate.pollInterval(.failed("x")))
    }

    func testARestartIsAnUpdateOnlyWhenTheDeviceAnswersWithTheNewVersion() throws {
        let after = try status(#"{"installed_version":"2.4.19","state":"Idle","latest_version":"2.4.19"}"#)
        XCTAssertEqual(FirmwareUpdate.outcome(before: "2.4.18", after: after), .updated(to: "2.4.19"))
    }

    func testARestartBackOnTheOldVersionIsSaidOutLoud() throws {
        let same = try status(#"{"installed_version":"2.4.18","state":"Idle","error":"Self-test failed","error_text":"The update did not pass its health check, so the previous software was restored."}"#)
        XCTAssertEqual(FirmwareUpdate.outcome(before: "2.4.18", after: same),
                       .rolledBack(still: "2.4.18",
                                   reason: "The update did not pass its health check, so the previous software was restored."))
        let silent = try status(#"{"installed_version":"2.4.18","state":"Idle","error":"No error"}"#)
        if case .rolledBack(_, let reason) = FirmwareUpdate.outcome(before: "2.4.18", after: silent) {
            XCTAssertTrue(reason.contains("health check"), "a plain reason when the device left none")
        } else {
            XCTFail("the same version after a restart is never an update")
        }
    }

    func testEveryStepHasATitle() {
        for step in FirmwareStep.allCases {
            XCTAssertFalse(step.title.isEmpty, step.rawValue)
        }
    }
}
