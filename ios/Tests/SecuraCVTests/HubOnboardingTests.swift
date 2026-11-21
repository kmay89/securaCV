// HubOnboardingTests.swift
//
// The hub walkthrough's judgment calls, pinned: which hosts may ever be
// dialed, what "ready" means (the landing page is not it), the exact
// request shapes Home Assistant's onboarding API expects, and the converge
// split between pages this build finishes and pages it must report.
// Mirrors desktop/hub-io/src/onboarding.rs's tests — the two companions
// must agree on the contract.

import XCTest
@testable import SecuraCV

final class HubOnboardingTests: XCTestCase {

    func testBaseURLAppendsTheDefaultPortOnlyWhenMissing() {
        XCTAssertEqual(HubOnboarding.baseURL(forHost: "homeassistant.local:8123")?.absoluteString,
                       "http://homeassistant.local:8123")
        XCTAssertEqual(HubOnboarding.baseURL(forHost: "10.0.0.5")?.absoluteString, "http://10.0.0.5:8123")
        XCTAssertEqual(HubOnboarding.baseURL(forHost: " hub-2.local ")?.absoluteString, "http://hub-2.local:8123")
        // A pasted URL is reduced to its host — people paste what the
        // browser shows.
        XCTAssertEqual(HubOnboarding.baseURL(forHost: "http://192.168.1.20:8123/")?.absoluteString,
                       "http://192.168.1.20:8123")
    }

    func testBaseURLRefusesHostsOffThisNetwork() {
        // The typed owner password goes to this URL, so a well-formed
        // public host is exactly what must not pass.
        for far in ["example.com", "example.com:8123", "203.0.113.5", "8.8.8.8:8123", "", "host name", "[::1]"] {
            XCTAssertNil(HubOnboarding.baseURL(forHost: far), "\(far) must be refused")
        }
    }

    func testClientIdentityIsTheInstanceItself() {
        let base = URL(string: "http://hub.local:8123")!
        XCTAssertEqual(HubOnboarding.clientID(base: base), "http://hub.local:8123/")
        XCTAssertEqual(HubOnboarding.redirectURI(base: base), "http://hub.local:8123/?auth_callback=1")
    }

    func testParseStepsReadsTheShapeAndRefusesOthers() {
        let good = Data(#"[{"step":"user","done":false},{"step":"core_config","done":true}]"#.utf8)
        XCTAssertEqual(HubOnboarding.parseSteps(good),
                       [OnboardingStep(step: "user", done: false), OnboardingStep(step: "core_config", done: true)])
        XCTAssertNil(HubOnboarding.parseSteps(Data(#"{"nope":1}"#.utf8)))
        XCTAssertNil(HubOnboarding.parseSteps(Data(#"[{"step":"user"}]"#.utf8)))
        XCTAssertNil(HubOnboarding.parseSteps(Data("<!DOCTYPE html>".utf8)))
    }

    func testProbeIsReadyOnlyForTheOnboardingStepList() {
        XCTAssertEqual(HubProbe.classify(status: nil, body: Data()), .offline)
        // HAOS's "Preparing Home Assistant" page answers 200 with HTML for
        // minutes before Core exists — the answer that used to read as up.
        XCTAssertEqual(HubProbe.classify(status: 200, body: Data("<!DOCTYPE html><title>Preparing</title>".utf8)),
                       .preparing)
        XCTAssertEqual(HubProbe.classify(status: 404, body: Data()), .preparing)
        XCTAssertEqual(HubProbe.classify(status: 200, body: Data(#"{"message":"API running."}"#.utf8)), .preparing)
        XCTAssertEqual(HubProbe.classify(status: 200, body: Data(#"[{"step":"user","done":false}]"#.utf8)), .ready)
        // Served after setup too; the companions decide from the flags.
        XCTAssertEqual(HubProbe.classify(status: 200, body: Data(#"[{"step":"user","done":true}]"#.utf8)), .ready)
    }

    func testTheOwnerRequestCarriesTheNormalizedLoginAndTheClientID() {
        let base = URL(string: "http://hub.local:8123")!
        let login = HubOwnerLogin(name: " Alex ", username: " Alex ", password: "correct horse")
        let body = HubOnboarding.userRequest(base: base, login: login)
        XCTAssertEqual(body["client_id"] as? String, "http://hub.local:8123/")
        XCTAssertEqual(body["name"] as? String, "Alex")
        XCTAssertEqual(body["username"] as? String, "alex")
        XCTAssertEqual(body["password"] as? String, "correct horse")
        XCTAssertEqual(body["language"] as? String, "en")
        let integration = HubOnboarding.integrationRequest(base: base)
        XCTAssertEqual(integration["redirect_uri"] as? String, "http://hub.local:8123/?auth_callback=1")
    }

    func testLoginRulesMatchHomeAssistantsOwn() {
        XCTAssertNil(HubOwnerLogin(name: "A", username: "a", password: "12345678").problem())
        XCTAssertNotNil(HubOwnerLogin(name: "", username: "a", password: "12345678").problem())
        XCTAssertNotNil(HubOwnerLogin(name: "A", username: "", password: "12345678").problem())
        XCTAssertNotNil(HubOwnerLogin(name: "A", username: "a b", password: "12345678").problem())
        XCTAssertNotNil(HubOwnerLogin(name: "A", username: "a", password: "1234567").problem())
    }

    func testPendingSplitsIntoFinishableAndUnknown() {
        let steps = [OnboardingStep(step: "user", done: false),
                     OnboardingStep(step: "core_config", done: false),
                     OnboardingStep(step: "analytics", done: true),
                     OnboardingStep(step: "integration", done: false),
                     OnboardingStep(step: "future_page", done: false)]
        let pending = HubOnboarding.pending(steps)
        XCTAssertEqual(pending, ["user", "core_config", "integration", "future_page"])
        let (finishable, unknown) = HubOnboarding.split(pending: pending)
        XCTAssertEqual(finishable, ["core_config", "integration"])
        XCTAssertEqual(unknown, ["future_page"])
    }

    func testFormEncodingSurvivesThePasswordCharacters() {
        XCTAssertEqual(HubOnboarding.formEncode([("a", "x&y=z+ w")]), "a=x%26y%3Dz%2B%20w")
        XCTAssertEqual(HubOnboarding.tokenExchangeForm(base: URL(string: "http://h.local:8123")!, code: "c0de"),
                       "grant_type=authorization_code&code=c0de&client_id=http%3A%2F%2Fh.local%3A8123%2F")
        XCTAssertEqual(HubOnboarding.revokeForm(refreshToken: "r/t"), "token=r%2Ft&action=revoke")
    }

    func testAuthCodeReadsBothShapesHomeAssistantUses() {
        XCTAssertEqual(HubOnboarding.authCode(from: Data(#"{"auth_code":"abc"}"#.utf8)), "abc")
        XCTAssertEqual(HubOnboarding.authCode(from: Data(#"{"type":"create_entry","result":"xyz"}"#.utf8)), "xyz")
        XCTAssertNil(HubOnboarding.authCode(from: Data(#"{"message":"no"}"#.utf8)))
    }

    func testOutcomeIsOkOnlyWhenNothingRemainsAndTheLoginIsProven() {
        var out = HubOnboardOutcome()
        XCTAssertFalse(out.ok)
        out.loginVerified = true
        XCTAssertTrue(out.ok)
        out.remaining = ["integration"]
        XCTAssertFalse(out.ok)
    }

    // MARK: - the walkthrough finishes by itself

    func testThePhoneFinishesTheHubByItselfOnlyOnACompleteForm() {
        // The 10–20 minute boot ends with the hub answering; nobody should
        // have to be watching for the moment. A complete, valid form lets the
        // phone go; anything less keeps the button as the fallback.
        XCTAssertTrue(HubWalkthrough.finishesByItself(ready: true, loginProblem: nil, passwordsMatch: true))
        XCTAssertFalse(HubWalkthrough.finishesByItself(ready: false, loginProblem: nil, passwordsMatch: true),
                       "not before the onboarding API answers")
        XCTAssertFalse(HubWalkthrough.finishesByItself(ready: true, loginProblem: "Pick a username.", passwordsMatch: true))
        XCTAssertFalse(HubWalkthrough.finishesByItself(ready: true, loginProblem: nil, passwordsMatch: false),
                       "a half-typed confirm is not a form")
        let complete = HubOwnerLogin(name: "A", username: "a", password: "12345678")
        XCTAssertTrue(HubWalkthrough.finishesByItself(ready: true, loginProblem: complete.problem(), passwordsMatch: true))
    }

    func testTheReadyNoticeTellsTheTruthAboutWhoFinishes() {
        XCTAssertEqual(HubWalkthrough.readyNoticeTitle, "Your hub is ready")
        XCTAssertEqual(HubWalkthrough.readyNoticeBody(finishing: true), "This phone is finishing it.")
        XCTAssertEqual(HubWalkthrough.readyNoticeBody(finishing: false),
                       "Fill in your account in SecuraCV and this phone finishes it.")
        XCTAssertFalse(HubWalkthrough.readyNoticeThreadID.isEmpty, "the tap routes to the walkthrough by thread")
    }

    // MARK: - the watch outlives the screen (G1-14, G1-16)

    @MainActor
    func testARecordedWatchResumesWithItsClockAndTheDraftsStayOnTheRunner() {
        defer { HubSetupRunner.forgetRecordedWatch() }
        let started = Date().addingTimeInterval(-12 * 60)
        UserDefaults.standard.set(started, forKey: HubSetupRunner.watchStartedKey)
        UserDefaults.standard.set("10.0.0.5", forKey: HubSetupRunner.watchHostKey)
        let runner = HubSetupRunner()
        runner.draftUsername = "owner"
        runner.resumeAfterRelaunch()
        XCTAssertEqual(runner.watchStartedAt, started, "the 25-minute clock counts from the power-on, not the relaunch")
        XCTAssertEqual(runner.draftTypedHost, "10.0.0.5")
        XCTAssertEqual(runner.draftUsername, "owner", "the typed account lives on the runner, not the screen")
        if case .watching = runner.phase {} else { XCTFail("the watch is running again: \(runner.phase)") }
        XCTAssertFalse(runner.discovery.isBrowsing, "no browse without the discovery consent")
        runner.cancelWatching()
        XCTAssertNil(UserDefaults.standard.object(forKey: HubSetupRunner.watchStartedKey), "a stopped watch is not resumed")
        XCTAssertEqual(runner.phase, .idle)
    }

    @MainActor
    func testAnOldRecordedWatchIsDropped() {
        defer { HubSetupRunner.forgetRecordedWatch() }
        UserDefaults.standard.set(Date().addingTimeInterval(-HubSetupRunner.resumeWithin - 60),
                                  forKey: HubSetupRunner.watchStartedKey)
        let runner = HubSetupRunner()
        runner.resumeAfterRelaunch()
        XCTAssertEqual(runner.phase, .idle)
        XCTAssertNil(UserDefaults.standard.object(forKey: HubSetupRunner.watchStartedKey))
    }

    @MainActor
    func testARotationAfterAFinishedHubReturnsTheRunnerToTheStart() {
        // Set up → Rotate broker login on a runner that already finished in
        // this app session: `.done` offers no Start or Finish, so the runner
        // is reset; a watch or a run in progress honors the rotation itself.
        XCTAssertTrue(HubSetupRunner.resetsForRotation(phase: .done, rotationRequested: true))
        XCTAssertFalse(HubSetupRunner.resetsForRotation(phase: .done, rotationRequested: false))
        XCTAssertFalse(HubSetupRunner.resetsForRotation(phase: .watching(.offline), rotationRequested: true))
        XCTAssertFalse(HubSetupRunner.resetsForRotation(phase: .provisioning, rotationRequested: true))
        XCTAssertFalse(HubSetupRunner.resetsForRotation(phase: .idle, rotationRequested: true))

        defer { HubSetupRunner.forgetRecordedWatch() }
        UserDefaults.standard.set(Date().addingTimeInterval(-60), forKey: HubSetupRunner.watchStartedKey)
        let runner = HubSetupRunner()
        runner.draftUsername = "owner"
        runner.resumeAfterRelaunch()
        runner.reset()
        XCTAssertEqual(runner.phase, .idle, "the walkthrough can run again")
        XCTAssertNil(runner.watchStartedAt)
        XCTAssertNil(runner.hubURL)
        XCTAssertTrue(runner.steps.isEmpty)
        XCTAssertTrue(runner.log.isEmpty)
        XCTAssertNil(runner.onboardOutcome)
        XCTAssertNil(UserDefaults.standard.object(forKey: HubSetupRunner.watchStartedKey))
        XCTAssertEqual(runner.draftUsername, "owner", "the typed drafts are kept")
    }

    func testOpeningTheWalkthroughStartsNoWatch() throws {
        // The Local Network alert follows "Start watching", never the open.
        let url = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("Sources/SecuraCV/Views/HubSetupView.swift")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: url.path), "ios checkout not visible from the test host")
        let view = try String(contentsOf: url, encoding: .utf8)
        let start = try XCTUnwrap(view.range(of: ".onAppear {"))
        let after = view[start.upperBound...]
        let end = try XCTUnwrap(after.range(of: "\n        }\n"))
        XCTAssertFalse(after[..<end.lowerBound].contains("startWatching"), "opening the screen starts nothing")
        XCTAssertFalse(view.contains(".onDisappear"), "leaving the screen does not stop the watch")
        XCTAssertFalse(view.contains("HubSetupRunner()"), "the runner is the store's, not the screen's")
    }
}
