//  WallDiscoveryTests.swift — an advert is a claim, and the Wall checks it.
//
//  NWBrowser cannot be fed in a test, so `WallDiscovery.browse` stays thin and
//  the judgment sits in `pollableHosts`, which runs against the REAL Rust core
//  (witness-core/src/host.rs) exactly as WitnessCoreTests does — a mock of the
//  gate would prove nothing about the gate. The rule is the iPhone's
//  DeviceAPI.isPrivate, restated once in Rust; if these and the Rust unit
//  tests ever disagree, the two surfaces have drifted.

import XCTest
@testable import WitnessWall

final class WallDiscoveryTests: XCTestCase {

    /// The shape the firmware's make_hostname() actually writes: one salted
    /// label, no domain. It has to come back resolvable.
    func testTheFirmwaresBareLabelIsQualifiedToLocal() {
        XCTAssertEqual(WallDiscovery.pollableHosts(["canary-nightstand7-001-a1b2c3"]),
                       ["canary-nightstand7-001-a1b2c3.local"])
    }

    func testAQualifiedLocalNameIsKeptAndAPrivateAddressIsNotDecorated() {
        XCTAssertEqual(WallDiscovery.pollableHosts(["canary-dash-001-d4e5f6.local", "192.168.1.20"]),
                       ["canary-dash-001-d4e5f6.local", "192.168.1.20"],
                       "an address must not become 192.168.1.20.local — the old code did exactly that")
    }

    /// The hole this suite was born from: anyone on the LAN can publish
    /// `_securacv._tcp` with any TXT `host`, and the Wall used to poll it.
    func testAnAdvertNamingAStrangersServerIsSkipped() {
        XCTAssertEqual(WallDiscovery.pollableHosts([
            "evil.example.com",
            "8.8.8.8",
            "10.0.0.1.attacker.com",
            "user@canary.local",
            "canary.local:8099",
            "-canary",
            "canary_dash_001",
            "",
        ]), [], "none of these is a host on this LAN, and none may become a source")
    }

    func testOneBadAdvertDoesNotCostTheGoodOnes() {
        XCTAssertEqual(WallDiscovery.pollableHosts(["evil.example.com", "porch-aa11bb", "172.16.4.9"]),
                       ["porch-aa11bb.local", "172.16.4.9"])
    }

    /// The wrapper itself, so a null from the core reads as "skip" and a
    /// string reads as the host — never a throw, never a crash.
    func testTheCoreWrapperAnswersDirectly() {
        XCTAssertEqual(WitnessCore.normalizeSourceHost("Canary-WAP-01"), "canary-wap-01.local")
        XCTAssertEqual(WitnessCore.normalizeSourceHost("10.0.0.7"), "10.0.0.7")
        XCTAssertNil(WitnessCore.normalizeSourceHost("securacv.com"))
        XCTAssertNil(WitnessCore.normalizeSourceHost(""))
    }
}
