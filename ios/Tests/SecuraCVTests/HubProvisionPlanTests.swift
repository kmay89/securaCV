// HubProvisionPlanTests.swift
//
// The phone's hub plan, pinned to the bundle's: the same slugs and
// repositories, every action idempotent against a snapshot, the
// Supervisor paths each action calls, and the folds of the raw answers
// the snapshot is read from.

import XCTest
@testable import SecuraCV

final class HubProvisionPlanTests: XCTestCase {

    func testAFreshHubHasEveryActionToDoInTheBundlesOrder() {
        let steps = HubProvisionPlan.plan(observed: HubObserved())
        XCTAssertEqual(steps.map(\.id), ["add-repositories", "install-broker", "broker-login", "connect-mqtt",
                                         "install-frigate", "install-securacv", "connect-securacv"])
        XCTAssertTrue(steps.flatMap(\.actions).allSatisfy { !$0.already })
        let labels = steps.flatMap(\.actions).map(\.id)
        XCTAssertEqual(Set(labels).count, labels.count, "every action label must be unique — the UI keys on it")
        // The install order the kernel depends on: broker before kernel.
        let todo = HubProvisionPlan.todo(steps)
        let broker = todo.firstIndex(of: .installAddon(slug: HubProvisionPlan.mosquittoSlug, friendly: "Mosquitto broker"))
        let kernel = todo.firstIndex(of: .installAddon(slug: HubProvisionPlan.kernelSlug, friendly: "Privacy Witness Kernel"))
        XCTAssertNotNil(broker); XCTAssertNotNil(kernel)
        XCTAssertLessThan(broker!, kernel!)
    }

    func testAFinishedHubSkipsEverything() {
        let o = HubObserved(
            repositories: [HubProvisionPlan.normalizeRepository(HubProvisionPlan.frigateRepository),
                           HubProvisionPlan.normalizeRepository(HubProvisionPlan.securacvRepository + "/")],
            addons: [HubProvisionPlan.mosquittoSlug: "started", HubProvisionPlan.frigateSlug: "stopped",
                     HubProvisionPlan.kernelSlug: "started"],
            configEntryDomains: ["mqtt", "securacv"],
            brokerLogins: ["canary"],
            kernelMode: "frigate")
        let steps = HubProvisionPlan.plan(observed: o)
        XCTAssertTrue(HubProvisionPlan.todo(steps).isEmpty, "\(HubProvisionPlan.todo(steps))")
        XCTAssertTrue(steps.flatMap(\.actions).allSatisfy { !$0.reason.isEmpty })
    }

    func testRepositoryNormalizationForgivesTheUsualSpellings() {
        for spelling in ["https://github.com/kmay89/securaCV", "https://github.com/kmay89/securacv/",
                         "https://github.com/kmay89/securaCV.git"] {
            XCTAssertEqual(HubProvisionPlan.normalizeRepository(spelling), "https://github.com/kmay89/securacv")
        }
    }

    func testMintedPasswordsAreLongUrlSafeAndFresh() throws {
        let a = try XCTUnwrap(HubProvisionPlan.mintPassword())
        let b = try XCTUnwrap(HubProvisionPlan.mintPassword())
        XCTAssertEqual(a.count, 24)
        XCTAssertNotEqual(a, b)
        let allowed = Set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
        XCTAssertTrue(a.allSatisfy { allowed.contains($0) }, a)
    }

    func testLoginsAreAppendedNeverReplaced() {
        let current: [[String: Any]] = [["username": "alice", "password": "a"], ["username": "canary", "password": "old"]]
        let out = HubProvisionPlan.loginsAppending(username: "canary", password: "new", to: current)
        XCTAssertEqual(out.count, 2)
        XCTAssertEqual(out[0]["username"] as? String, "alice")
        XCTAssertEqual(out[1]["username"] as? String, "canary")
        XCTAssertEqual(out[1]["password"] as? String, "new")
    }

    func testObserveFoldsTheRawAnswers() {
        let repos = Data(#"{"result":"ok","data":[{"slug":"ccab4aaf","source":"https://github.com/blakeblackshear/frigate-hass-addons"}]}"#.utf8)
        let addons = Data(#"{"result":"ok","data":{"addons":[{"slug":"core_mosquitto","state":"started"},{"slug":"d0491a67_privacy_witness_kernel","state":"stopped"}]}}"#.utf8)
        let mosq = Data(#"{"result":"ok","data":{"options":{"logins":[{"username":"canary","password":"x"}]}}}"#.utf8)
        let kernel = Data(#"{"result":"ok","data":{"options":{"mode":"frigate"}}}"#.utf8)
        let entries = Data(#"[{"domain":"mqtt","title":"MQTT"},{"domain":"sun"}]"#.utf8)
        let o = HubProvisionPlan.observe(repositories: repos, addons: addons, mosquittoInfo: mosq,
                                         kernelInfo: kernel, configEntries: entries)
        XCTAssertEqual(o.repositories, ["https://github.com/blakeblackshear/frigate-hass-addons"])
        XCTAssertEqual(o.addons, ["core_mosquitto": "started", "d0491a67_privacy_witness_kernel": "stopped"])
        XCTAssertEqual(o.brokerLogins, ["canary"])
        XCTAssertEqual(o.kernelMode, "frigate")
        XCTAssertEqual(o.configEntryDomains, ["mqtt", "sun"])
        // Nothing readable reads as nothing there — which only ever errs
        // toward doing an idempotent step again.
        let empty = HubProvisionPlan.observe(repositories: nil, addons: Data("nope".utf8), mosquittoInfo: nil,
                                             kernelInfo: nil, configEntries: nil)
        XCTAssertEqual(empty, HubObserved())
    }

    func testEveryActionNamesItsCall() {
        let todo = HubProvisionPlan.todo(HubProvisionPlan.plan(observed: HubObserved()))
        for action in todo {
            switch action {
            case .connectMQTT, .connectSecuraCV:
                XCTAssertNil(HubProvisionPlan.supervisorPath(for: action))
            default:
                XCTAssertNotNil(HubProvisionPlan.supervisorPath(for: action), HubProvisionPlan.label(for: action))
            }
        }
        XCTAssertEqual(HubProvisionPlan.supervisorPath(for: .installAddon(slug: "x_y", friendly: "")), "store/addons/x_y/install")
        XCTAssertEqual(HubProvisionPlan.supervisorPath(for: .addBrokerLogin(username: "canary")), "addons/core_mosquitto/options")
    }

    func testTheDiscoveredMQTTCardIsFoundByItsSource() {
        let flows = Data(#"[{"flow_id":"f1","handler":"zha","context":{"source":"usb"}},{"flow_id":"f2","handler":"mqtt","context":{"source":"hassio"}}]"#.utf8)
        XCTAssertEqual(HubProvisionPlan.discoveredMQTTFlowID(in: flows), "f2")
        XCTAssertNil(HubProvisionPlan.discoveredMQTTFlowID(in: Data("[]".utf8)))
    }

    func testFlowOutcomesSpeakHomeAssistantsVocabulary() {
        XCTAssertEqual(HubProvisionPlan.flowOutcome(Data(#"{"type":"create_entry"}"#.utf8), status: 200), .created)
        XCTAssertEqual(HubProvisionPlan.flowOutcome(Data(#"{"type":"abort","reason":"already_configured"}"#.utf8), status: 200), .alreadyConfigured)
        XCTAssertEqual(HubProvisionPlan.flowOutcome(Data(#"{"type":"form","step_id":"broker"}"#.utf8), status: 200), .needsForm(step: "broker"))
        XCTAssertEqual(HubProvisionPlan.flowOutcome(Data(#"{"message":"Invalid handler specified"}"#.utf8), status: 404),
                       .failed("Home Assistant doesn't have this integration installed yet."))
        if case .failed = HubProvisionPlan.flowOutcome(Data(#"{"type":"abort","reason":"cannot_connect"}"#.utf8), status: 200) {} else {
            XCTFail("an abort for any other reason is a failure, not silence")
        }
    }
}
