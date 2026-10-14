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
                       .notPaired(MagicPairPlan.Copy.noClaim),
                       "\"{}\": the Canary joined, but the door was somebody else's")
        XCTAssertEqual(MagicPairPlan.afterJoin(hasClaimService: true, claim: claim(url: ""), isPrivateHost: DeviceAPI.isPrivate),
                       .notPaired(MagicPairPlan.Copy.noClaim), "a claim with nowhere to spend it")
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
                       .notPaired(MagicPairPlan.Copy.unreachable), "no third host")
        XCTAssertEqual(MagicPairPlan.afterFetch(.unreachable, claim: claim(ip: nil), triedIP: false, isPrivateHost: DeviceAPI.isPrivate),
                       .notPaired(MagicPairPlan.Copy.unreachable), "no address reported")
        XCTAssertEqual(MagicPairPlan.afterFetch(.unreachable, claim: claim(ip: ""), triedIP: false, isPrivateHost: DeviceAPI.isPrivate),
                       .notPaired(MagicPairPlan.Copy.unreachable))
        XCTAssertEqual(MagicPairPlan.afterFetch(.unreachable, claim: claim(ip: "8.8.8.8"), triedIP: false, isPrivateHost: DeviceAPI.isPrivate),
                       .notPaired(MagicPairPlan.Copy.unreachable),
                       "a public address is not dialed, whoever reported it")
        // The address is the host already: there is no second host to try.
        let byIP = claim(url: "http://192.168.1.23/api/provisioning-receipt?claim=\(hex)")
        XCTAssertEqual(MagicPairPlan.afterFetch(.unreachable, claim: byIP, triedIP: false, isPrivateHost: DeviceAPI.isPrivate),
                       .notPaired(MagicPairPlan.Copy.unreachable))
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
                       .notPaired(MagicPairPlan.Copy.refused))
    }

    func testAnyOtherFailureIsFinalAndNamed() {
        let step = MagicPairPlan.afterFetch(.other("Device error 500: "), claim: claim(), triedIP: false,
                                            isPrivateHost: DeviceAPI.isPrivate)
        guard case .notPaired(let note) = step else { return XCTFail("\(step)") }
        XCTAssertTrue(note.contains("Device error 500"), "the error's own words")
        XCTAssertTrue(note.contains("Ready to pair"), "and where to finish pairing it")
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
        // Joined, claim spent: the next door is a real one — the device's
        // row under Ready to pair (PairView: its recovery kit, or a fresh
        // setup). The Fleet tab never had a BOOT-tap pairing flow, so no
        // sentence may send anyone looking for one.
        let finishOnTheFleetTab = [MagicPairPlan.Copy.noClaim, MagicPairPlan.Copy.refused,
                                   MagicPairPlan.Copy.claimExpired, MagicPairPlan.Copy.other("x")]
        for s in finishOnTheFleetTab {
            XCTAssertTrue(s.contains("Fleet tab") && s.contains("Ready to pair"),
                          "joined but not paired names the next door: \(s)")
        }
        // Nothing reached the Canary: the claim is unspent, so the next step
        // is right here.
        for s in [MagicPairPlan.Copy.unreachable, MagicPairPlan.Copy.phoneOffWiFi] {
            XCTAssertTrue(s.contains("Try again"), "an unspent claim is retried in place: \(s)")
        }
        XCTAssertTrue(MagicPairPlan.Copy.phoneOffWiFi.contains("off Wi-Fi"), "the cause, named")
        for s in [MagicPairPlan.Copy.tlsNoPin, MagicPairPlan.Copy.receiptTLSNoPin] {
            XCTAssertTrue(s.contains("set it up from this phone again"), "firmware first, then the card: \(s)")
        }
        for s in MagicPairPlan.Copy.all {
            XCTAssertFalse(s.contains("tap on its BOOT button"),
                           "no promise of a BOOT-tap pairing the app does not have: \(s)")
        }
        XCTAssertFalse(MagicPairPlan.Copy.notPrivate.contains("Fleet tab"),
                       "a refusal before any dial is not a pairing instruction")
    }

    // MARK: - trying again

    func testOnlyAnUnspentLiveClaimIsTriedAgain() {
        let c = claim()   // expires_in 180
        let t0 = Date(timeIntervalSince1970: 1_800_000_000)
        XCTAssertTrue(MagicPairPlan.mayRetry(after: .unreachable, claim: c, readAt: t0, now: t0.addingTimeInterval(30)),
                      "nothing reached the Canary: the claim is unspent")
        XCTAssertFalse(MagicPairPlan.mayRetry(after: .unreachable, claim: c, readAt: t0, now: t0.addingTimeInterval(178)),
                       "too close to the expiry to be worth sending")
        XCTAssertFalse(MagicPairPlan.mayRetry(after: .refused, claim: c, readAt: t0, now: t0.addingTimeInterval(5)),
                       "a 403 spent it")
        XCTAssertFalse(MagicPairPlan.mayRetry(after: .other("x"), claim: c, readAt: t0, now: t0.addingTimeInterval(5)),
                       "an answer that was not a receipt may have spent it")
        XCTAssertFalse(MagicPairPlan.mayRetry(after: .receipt, claim: c, readAt: t0, now: t0))
        XCTAssertFalse(MagicPairPlan.mayRetry(after: nil, claim: c, readAt: t0, now: t0), "never dialed, nothing to retry")
        var short = c
        short.expiresIn = 20
        XCTAssertFalse(MagicPairPlan.mayRetry(after: .unreachable, claim: short, readAt: t0, now: t0.addingTimeInterval(16)),
                       "the claim's own lifetime wins over the default")
        short.expiresIn = nil
        XCTAssertTrue(MagicPairPlan.mayRetry(after: .unreachable, claim: short, readAt: t0, now: t0.addingTimeInterval(100)),
                      "no lifetime named: the firmware's 180 s")
    }

    func testNothingAnsweredNamesThePhoneBeingOffWiFi() {
        XCTAssertEqual(MagicPairPlan.unreachableNote(phoneOnWiFi: false), MagicPairPlan.Copy.phoneOffWiFi)
        XCTAssertEqual(MagicPairPlan.unreachableNote(phoneOnWiFi: true), MagicPairPlan.Copy.unreachable)
    }
}
