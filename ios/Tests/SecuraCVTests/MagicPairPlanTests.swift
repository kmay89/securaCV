// MagicPairPlanTests.swift
//
// Every branch of what the nearby-Canary sheet does after the Canary said
// it joined — the plan is pure, so each rule is a line here: which
// Canaries pair at all, what is never dialed, how many times the claim is
// dialed and where, and what the person reads when it did not pair.

import XCTest
@testable import SecuraCV

final class MagicPairPlanTests: XCTestCase {

    private let hex = "0123456789abcdef0123456789abcdef"
    private let fp = String(repeating: "cd", count: 32)

    private var httpURL: String { "http://canary-ab12.local/api/provisioning-receipt?claim=\(hex)" }
    private var httpsURL: String { "https://canary-ab12.local/api/provisioning-receipt?claim=\(hex)" }

    private func claim(url: String? = nil, fp: String? = nil, ip: String? = "192.168.1.23") -> ImprovWire.Claim {
        ImprovWire.Claim(deviceID: "wap-ab12", claim: hex,
                         claimURL: (url ?? httpURL).isEmpty ? nil : URL(string: url ?? httpURL),
                         tlsCertFingerprint: fp, staIP: ip, mdnsHost: "canary-ab12", expiresIn: 180)
    }

    // MARK: - after the join

    func testNoCompanionServiceWatchesTheFleet() {
        // A Sense or Vision: nothing to pair over HTTP, the card's ending stands.
        XCTAssertEqual(MagicPairPlan.afterJoin(hasClaimService: false, claim: nil, isPrivateHost: DeviceAPI.isPrivate),
                       .watchFleet)
        XCTAssertEqual(MagicPairPlan.afterJoin(hasClaimService: false, claim: claim(), isPrivateHost: DeviceAPI.isPrivate),
                       .watchFleet, "a claim without the service is not a thing the plan dials")
    }

    func testAClaimThatWasNotOursIsJoinedNotPaired() {
        XCTAssertEqual(MagicPairPlan.afterJoin(hasClaimService: true, claim: nil, isPrivateHost: DeviceAPI.isPrivate),
                       .notPairedTapBoot(MagicPairPlan.Copy.noClaim),
                       "\"{}\": the Canary joined, but the door was somebody else's")
        XCTAssertEqual(MagicPairPlan.afterJoin(hasClaimService: true, claim: claim(url: ""), isPrivateHost: DeviceAPI.isPrivate),
                       .notPairedTapBoot(MagicPairPlan.Copy.noClaim), "a claim with nowhere to spend it")
    }

    func testAnHTTPClaimIsSpentAtItsURLWithNoPin() throws {
        let c = claim()
        XCTAssertEqual(MagicPairPlan.afterJoin(hasClaimService: true, claim: c, isPrivateHost: DeviceAPI.isPrivate),
                       .spendClaim(try XCTUnwrap(c.claimURL), pin: nil))
    }

    func testAnHTTPSClaimCarriesItsPin() throws {
        let c = claim(url: httpsURL, fp: fp)
        XCTAssertEqual(MagicPairPlan.afterJoin(hasClaimService: true, claim: c, isPrivateHost: DeviceAPI.isPrivate),
                       .spendClaim(try XCTUnwrap(c.claimURL), pin: fp))
    }

    func testAnHTTPSClaimWithNoPinIsNeverDialed() {
        // PairView's rule: a TLS Canary the app cannot check is not dialed.
        XCTAssertEqual(MagicPairPlan.afterJoin(hasClaimService: true, claim: claim(url: httpsURL, fp: nil),
                                               isPrivateHost: DeviceAPI.isPrivate),
                       .refused(MagicPairPlan.Copy.tlsNoPin))
    }

    func testAClaimOffTheLocalNetworkIsNeverDialed() throws {
        let c = claim(url: "https://securacv.com/api/provisioning-receipt?claim=\(hex)", fp: fp)
        XCTAssertEqual(MagicPairPlan.afterJoin(hasClaimService: true, claim: c, isPrivateHost: DeviceAPI.isPrivate),
                       .refused(MagicPairPlan.Copy.notPrivate))
        // The gate is asked about the URL that would be dialed, exactly —
        // and its word is final, whatever the URL looks like.
        var asked: [URL] = []
        _ = MagicPairPlan.afterJoin(hasClaimService: true, claim: claim(),
                                    isPrivateHost: { asked.append($0); return true })
        XCTAssertEqual(asked, [try XCTUnwrap(claim().claimURL)])
        XCTAssertEqual(MagicPairPlan.afterJoin(hasClaimService: true, claim: claim(), isPrivateHost: { _ in false }),
                       .refused(MagicPairPlan.Copy.notPrivate))
    }

    // MARK: - after a dial

    func testAReceiptPairs() {
        XCTAssertEqual(MagicPairPlan.afterFetch(.receipt, claim: claim(), triedIP: false, isPrivateHost: DeviceAPI.isPrivate),
                       .paired)
        XCTAssertEqual(MagicPairPlan.afterFetch(.receipt, claim: claim(), triedIP: true, isPrivateHost: DeviceAPI.isPrivate),
                       .paired)
    }

    func testUnreachableTriesTheReportedAddressOnce() throws {
        let c = claim()
        let viaIP = try XCTUnwrap(URL(string: "http://192.168.1.23/api/provisioning-receipt?claim=\(hex)"))
        XCTAssertEqual(MagicPairPlan.afterFetch(.unreachable, claim: c, triedIP: false, isPrivateHost: DeviceAPI.isPrivate),
                       .retryViaIP(viaIP, pin: nil),
                       "the .local name may not resolve yet; the address the Canary reported stands in")
        XCTAssertEqual(MagicPairPlan.afterFetch(.unreachable, claim: c, triedIP: true, isPrivateHost: DeviceAPI.isPrivate),
                       .notPairedTapBoot(MagicPairPlan.Copy.unreachable), "no third host")
        XCTAssertEqual(MagicPairPlan.afterFetch(.unreachable, claim: claim(ip: nil), triedIP: false, isPrivateHost: DeviceAPI.isPrivate),
                       .notPairedTapBoot(MagicPairPlan.Copy.unreachable), "no address reported")
        XCTAssertEqual(MagicPairPlan.afterFetch(.unreachable, claim: claim(ip: ""), triedIP: false, isPrivateHost: DeviceAPI.isPrivate),
                       .notPairedTapBoot(MagicPairPlan.Copy.unreachable))
        XCTAssertEqual(MagicPairPlan.afterFetch(.unreachable, claim: claim(ip: "8.8.8.8"), triedIP: false, isPrivateHost: DeviceAPI.isPrivate),
                       .notPairedTapBoot(MagicPairPlan.Copy.unreachable),
                       "a public address is not dialed, whoever reported it")
        // The address is the host already: there is no second host to try.
        let byIP = claim(url: "http://192.168.1.23/api/provisioning-receipt?claim=\(hex)")
        XCTAssertEqual(MagicPairPlan.afterFetch(.unreachable, claim: byIP, triedIP: false, isPrivateHost: DeviceAPI.isPrivate),
                       .notPairedTapBoot(MagicPairPlan.Copy.unreachable))
    }

    func testTheAddressRetryKeepsThePin() throws {
        let c = claim(url: httpsURL, fp: fp)
        let viaIP = try XCTUnwrap(URL(string: "https://192.168.1.23/api/provisioning-receipt?claim=\(hex)"))
        XCTAssertEqual(MagicPairPlan.afterFetch(.unreachable, claim: c, triedIP: false, isPrivateHost: DeviceAPI.isPrivate),
                       .retryViaIP(viaIP, pin: fp))
    }

    func testARefusalIsFinal() {
        // 403: spent, expired, or a wrong claim — the firmware burned it, so
        // an address on hand earns no second dial.
        XCTAssertEqual(MagicPairPlan.afterFetch(.refused, claim: claim(), triedIP: false, isPrivateHost: DeviceAPI.isPrivate),
                       .notPairedTapBoot(MagicPairPlan.Copy.refused))
    }

    func testAnyOtherFailureIsFinalAndNamed() {
        let step = MagicPairPlan.afterFetch(.other("Device error 500: "), claim: claim(), triedIP: false,
                                            isPrivateHost: DeviceAPI.isPrivate)
        guard case .notPairedTapBoot(let note) = step else { return XCTFail("\(step)") }
        XCTAssertTrue(note.contains("Device error 500"), "the error's own words")
        XCTAssertTrue(note.contains("BOOT button"), "and the way to pair it later")
        XCTAssertEqual(note, MagicPairPlan.Copy.other("Device error 500: "))
    }

    // MARK: - the receipt's own gates

    func testTheReceiptGatesAreTheOnesPairViewApplies() throws {
        let local = try XCTUnwrap(URL(string: "http://canary-ab12.local"))
        let secure = try XCTUnwrap(URL(string: "https://canary-ab12.local"))
        let far = try XCTUnwrap(URL(string: "http://securacv.com"))
        XCTAssertEqual(MagicPairPlan.afterReceipt(baseURL: local, tlsCertFingerprint: nil, isPrivateHost: DeviceAPI.isPrivate),
                       .paired)
        XCTAssertEqual(MagicPairPlan.afterReceipt(baseURL: secure, tlsCertFingerprint: fp, isPrivateHost: DeviceAPI.isPrivate),
                       .paired)
        XCTAssertEqual(MagicPairPlan.afterReceipt(baseURL: secure, tlsCertFingerprint: nil, isPrivateHost: DeviceAPI.isPrivate),
                       .refused(MagicPairPlan.Copy.receiptTLSNoPin))
        XCTAssertEqual(MagicPairPlan.afterReceipt(baseURL: far, tlsCertFingerprint: nil, isPrivateHost: DeviceAPI.isPrivate),
                       .refused(MagicPairPlan.Copy.receiptNotPrivate))
    }

    // MARK: - the words

    func testEveryNoteSaysHowToPairLaterAndKeepsTheVoice() {
        // (US spellings and the fleet word are the repo-wide lints' job —
        // scripts/lint_spelling.py and scripts/lint_fleet_word.py sweep
        // this file too, so nothing here spells what they forbid.)
        for s in MagicPairPlan.Copy.all {
            XCTAssertFalse(s.isEmpty)
            XCTAssertTrue(s.hasSuffix("."), "a sentence: \(s)")
        }
        let joinedNotPaired = [MagicPairPlan.Copy.noClaim, MagicPairPlan.Copy.unreachable,
                               MagicPairPlan.Copy.refused, MagicPairPlan.Copy.tlsNoPin,
                               MagicPairPlan.Copy.receiptTLSNoPin, MagicPairPlan.Copy.other("x")]
        for s in joinedNotPaired {
            XCTAssertTrue(s.contains("Fleet tab") && s.contains("BOOT button"),
                          "joined but not paired names the next door: \(s)")
        }
        XCTAssertFalse(MagicPairPlan.Copy.notPrivate.contains("BOOT"),
                       "a refusal before any dial is not a pairing instruction")
    }
}
