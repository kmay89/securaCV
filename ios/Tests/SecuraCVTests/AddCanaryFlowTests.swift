// AddCanaryFlowTests.swift
//
// The small decisions behind "add a Canary" (Model/AddCanaryFlow.swift and
// the transports' halves of them): no Bluetooth prompt before the consent
// that explains it, Today's first-run card, the words for a radio that is
// off or not allowed, the camera ask behind the QR button, and the Local
// Network denial NWBrowser reports by waiting. Every one of these used to
// be a dead end — an alert with no context, a card that could not appear,
// a spinner with no reason, a button that never showed.

import XCTest
import Network
import CoreBluetooth
@testable import SecuraCV

@MainActor
final class AddCanaryFlowTests: XCTestCase {

    // MARK: - no prompt before the consent

    func testNoBluetoothManagerUntilAScanIsRequested() {
        // Creating a CBCentralManager is what raises iOS's Bluetooth alert;
        // the store builds its BLEConsole at launch, so the console must
        // build no manager until asked.
        XCTAssertFalse(BLEConsole().hasManager, "launch must not raise the Bluetooth prompt")
        let console = BLEConsole()
        console.stopScan()
        XCTAssertFalse(console.hasManager, "stopping (every backgrounding does) never builds one")
        console.pruneStaleSightings()
        XCTAssertFalse(console.hasManager)
    }

    func testOnlyTheConsoleBuildsAManagerAndOnlyOnDemand() throws {
        // The guard behind the guard: one place in the app constructs a
        // central manager, and it is the on-demand one.
        let source = try self.source("Sources/SecuraCV/Transport/BLEConsole.swift")
        XCTAssertEqual(source.components(separatedBy: "CBCentralManager(delegate:").count - 1, 1,
                       "exactly one construction site")
        let initBody = try XCTUnwrap(source.range(of: "override init() {"))
        let afterInit = source[initBody.upperBound...]
        let initEnd = try XCTUnwrap(afterInit.range(of: "}"))
        XCTAssertFalse(afterInit[..<initEnd.lowerBound].contains("CBCentralManager"),
                       "init builds nothing")
    }

    func testTheRadioReportsMapToTheirPlainAnswers() {
        XCTAssertEqual(BLEConsole.standing(state: .poweredOn), .ready)
        XCTAssertEqual(BLEConsole.standing(state: .poweredOff), .off)
        XCTAssertEqual(BLEConsole.standing(state: .unauthorized), .denied)
        XCTAssertEqual(BLEConsole.standing(state: .unsupported), .unsupported)
        XCTAssertEqual(BLEConsole.standing(state: .resetting), .unknown, "a report is coming")
        XCTAssertEqual(BLEConsole.standing(authorization: .denied), .denied)
        XCTAssertEqual(BLEConsole.standing(authorization: .restricted), .denied)
        XCTAssertEqual(BLEConsole.standing(authorization: .notDetermined), .unknown,
                       "never asked is not a refusal — and reading it never asks")
    }

    // MARK: - a reason, not a spinner

    func testARadioThatCannotHearSaysWhyAndWhereToFixIt() {
        XCTAssertNil(SetupRadioAdvice.bluetooth(.ready))
        XCTAssertNil(SetupRadioAdvice.bluetooth(.unknown), "before the first report, assume it can hear")
        XCTAssertEqual(SetupRadioAdvice.bluetooth(.off)?.opensSettings, false,
                       "an off radio is Control Center's switch, not this app's page")
        XCTAssertEqual(SetupRadioAdvice.bluetooth(.denied)?.opensSettings, true)
        XCTAssertNotNil(SetupRadioAdvice.bluetooth(.unsupported))
        XCTAssertEqual(SetupRadioAdvice.localNetwork(blocked: true)?.opensSettings, true)
        XCTAssertNil(SetupRadioAdvice.localNetwork(blocked: false))
        XCTAssertEqual(SetupRadioAdvice.either(bluetooth: .denied, localNetworkBlocked: true),
                       SetupRadioAdvice.bluetoothDenied, "Bluetooth first: it is what finds a new Canary")
        XCTAssertEqual(SetupRadioAdvice.either(bluetooth: .ready, localNetworkBlocked: true),
                       SetupRadioAdvice.localNetworkBlocked)
        for advice in [SetupRadioAdvice.bluetoothOff, SetupRadioAdvice.bluetoothDenied,
                       SetupRadioAdvice.bluetoothUnsupported, SetupRadioAdvice.localNetworkBlocked] {
            XCTAssertTrue(advice.text.hasSuffix("."), advice.text)
        }
    }

    func testALocalNetworkDenialIsRecognizedFromTheBrowsersWait() {
        // NWBrowser never fails on a denial: it waits with DNS-SD's
        // policy-denied code (kDNSServiceErr_PolicyDenied, -65570).
        XCTAssertTrue(Discovery.isPolicyDenied(.dns(-65570)))
        XCTAssertFalse(Discovery.isPolicyDenied(.dns(-65537)), "another DNS-SD error is not a denial")
        XCTAssertFalse(Discovery.isPolicyDenied(.posix(.ENETDOWN)))
    }

    // MARK: - Today's way in

    func testAFreshInstallIsAskedOnTodayItself() {
        XCTAssertEqual(FirstRunCard.decide(consent: nil, hasRealFleet: false, hearsNearby: false), .ask,
                       "the nearby card needs consent, and Today is where a fresh install lands")
        XCTAssertEqual(FirstRunCard.decide(consent: true, hasRealFleet: false, hearsNearby: false), .listening)
        XCTAssertEqual(FirstRunCard.decide(consent: false, hasRealFleet: false, hearsNearby: false), .invite)
        XCTAssertNil(FirstRunCard.decide(consent: true, hasRealFleet: false, hearsNearby: true),
                     "a Canary is heard: the nearby card has the moment")
        XCTAssertNil(FirstRunCard.decide(consent: nil, hasRealFleet: true, hearsNearby: false),
                     "a real fleet: Today is about the fleet")
    }

    // MARK: - the camera, asked at the tap

    func testTheQRButtonAsksForTheCameraInsteadOfHiding() {
        XCTAssertEqual(CameraGate.action(for: .notDetermined), .askThenScan,
                       "a fresh install sees the button and the ask, not nothing")
        XCTAssertEqual(CameraGate.action(for: .authorized), .scan)
        XCTAssertEqual(CameraGate.action(for: .denied), .explainDenied)
        XCTAssertTrue(CameraGate.deniedNote.contains("Settings"))
    }

    func testTheQRButtonIsGatedOnTheHardwareNotThePermission() throws {
        let walkthrough = try source("Sources/SecuraCV/Views/CanarySetupView.swift")
        XCTAssertTrue(walkthrough.contains("SetupQRScannerSheet.deviceCanScan"))
        XCTAssertFalse(walkthrough.contains("SetupQRScannerSheet.isSupported"),
                       "isSupported includes isAvailable, which is false until the camera is allowed")
    }

    // MARK: - helpers

    /// #filePath → ios/Tests/SecuraCVTests/… → the ios tree is three up.
    /// Skips (rather than fails) when the checkout isn't visible from the
    /// test host.
    private func source(_ relative: String) throws -> String {
        let url = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // SecuraCVTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // ios
            .appendingPathComponent(relative)
        try XCTSkipUnless(FileManager.default.fileExists(atPath: url.path),
                          "ios checkout not visible from the test host")
        return try String(contentsOf: url, encoding: .utf8)
    }
}
