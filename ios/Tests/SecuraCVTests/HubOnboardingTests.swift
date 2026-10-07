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
}
