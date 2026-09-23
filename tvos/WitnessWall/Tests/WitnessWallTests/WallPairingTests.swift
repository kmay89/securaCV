//  WallPairingTests.swift — the pairing receipt, the store, and the one fold
//  that decides what a walk may claim.
//
//  The rules these pin: a receipt without a key to pin is refused (a token
//  alone would be trust on first use by another name); a source is one
//  pairing however its address is spelled; forgetting takes the token AND
//  the pin; and VerificationStanding.derive gives "verified" to exactly one
//  situation — the pinned key signing a log whose walk passed.

import Security
import XCTest
@testable import WitnessWall

final class WallPairingTests: XCTestCase {
    private let token = String(repeating: "ab", count: 32)
    private let key = String(repeating: "cd", count: 32)

    // MARK: - the receipt

    func testTheMintersReceiptParsesAsPrinted() throws {
        let line = #"{"kernel":"witness-kernel","base_url":"http://192.168.1.20:8799","sealed_log_token":"\#(token)","verifying_key":"\#(key)","token_id":"abababab"}"#
        let receipt = try ViewerReceipt.parse(line)
        XCTAssertEqual(receipt.token, token)
        XCTAssertEqual(receipt.verifyingKey, key)
        XCTAssertEqual(receipt.baseURL, "http://192.168.1.20:8799")
        XCTAssertEqual(receipt.tokenID, "abababab")
    }

    func testTheReceiptIsToleratedTheWayAPastedLineArrives() throws {
        // `token` for `sealed_log_token`, upper-case hex, whitespace and a
        // trailing newline around it, curly quotes from a television's text
        // entry, and a key this Wall does not know — all read.
        let line = "\n {\u{201C}token\u{201D}: \u{201C}\(token.uppercased())\u{201D}, "
            + "\u{201C}verifying_key\u{201D}: \u{201C}\(key.uppercased())\u{201D}, "
            + "\u{201C}minted_by\u{201D}: \u{201C}someone\u{201D}}\n"
        let receipt = try ViewerReceipt.parse(line)
        XCTAssertEqual(receipt.token, token, "hex is lower-cased")
        XCTAssertEqual(receipt.verifyingKey, key)
        XCTAssertNil(receipt.baseURL)
    }

    func testAReceiptWithNothingToPinIsRefused() {
        XCTAssertThrowsError(try ViewerReceipt.parse(#"{"sealed_log_token":"\#(token)"}"#)) {
            XCTAssertEqual($0 as? PairingError, .missingKey)
        }
        XCTAssertThrowsError(try ViewerReceipt.parse(#"{"sealed_log_token":"\#(token)","verifying_key":"abcd"}"#)) {
            XCTAssertEqual($0 as? PairingError, .missingKey, "a key that is not 32 bytes pins nothing")
        }
    }

    func testAReceiptWithNoTokenOrNoJSONIsRefused() {
        XCTAssertThrowsError(try ViewerReceipt.parse(#"{"verifying_key":"\#(key)"}"#)) {
            XCTAssertEqual($0 as? PairingError, .missingToken)
        }
        for junk in ["", "hello", "[1,2]", "{not json"] {
            XCTAssertThrowsError(try ViewerReceipt.parse(junk), junk) {
                XCTAssertEqual($0 as? PairingError, .notAReceipt, junk)
            }
        }
    }

    // MARK: - the store

    func testASourceIsOnePairingHoweverItsAddressIsSpelled() {
        let a = PairedSourceStore.account(for: "192.168.1.20:8799")
        XCTAssertEqual(a, "http://192.168.1.20:8799")
        XCTAssertEqual(PairedSourceStore.account(for: "http://192.168.1.20:8799/api/fleet"), a)
        XCTAssertEqual(PairedSourceStore.account(for: "HTTP://192.168.1.20:8799"), a)
        XCTAssertNotEqual(PairedSourceStore.account(for: "192.168.1.20:8099"), a,
                          "another port is another service, and another pairing")
        XCTAssertNil(PairedSourceStore.account(for: "   "))
    }

    func testPairThenForgetTakesTheTokenAndThePinTogether() throws {
        let secrets = MemoryPairingSecrets()
        let store = PairedSourceStore(secrets: secrets)
        let receipt = try ViewerReceipt.parse(receiptJSON(token: token, key: key))
        let when = Date(timeIntervalSince1970: 1_800_000_000)

        let saved = try store.pair(receipt, source: "canary.local:8799", now: when)
        XCTAssertEqual(store.pairing(for: "http://canary.local:8799"), saved)
        XCTAssertEqual(saved.pairedAt, when)
        XCTAssertEqual(saved.keyPrefix, String(key.prefix(12)))
        XCTAssertEqual(secrets.accounts.count, 1, "token and pin are ONE item")

        store.forget("canary.local:8799")
        XCTAssertNil(store.pairing(for: "canary.local:8799"))
        XCTAssertEqual(secrets.accounts, [])
    }

    func testTheKeychainRoundTripsAPairingOnThisDevice() throws {
        // The app's real store. An unsigned simulator host (tvos.yml builds
        // with CODE_SIGNING_ALLOWED=NO) can be refused the Keychain outright
        // (errSecMissingEntitlement, typically), which says nothing about a
        // signed Apple TV — so a refused FIRST write skips, naming the
        // status, rather than failing; every step after it must hold.
        let secrets = KeychainPairingSecrets()
        let account = "http://wall-test-\(UUID().uuidString.lowercased()).local"
        defer { secrets.remove(account: account) }
        do {
            try secrets.write(Data("pin".utf8), account: account)
        } catch PairingError.keychain(let status) {
            throw XCTSkip("the Keychain refused this test host (status \(status)); "
                          + "the model's pairing rules are proven over MemoryPairingSecrets")
        }
        XCTAssertEqual(secrets.read(account: account), Data("pin".utf8))
        try secrets.write(Data("repinned".utf8), account: account)
        XCTAssertEqual(secrets.read(account: account), Data("repinned".utf8), "a re-pair replaces, never duplicates")
        secrets.remove(account: account)
        XCTAssertNil(secrets.read(account: account))
    }

    // MARK: - what a walk may claim

    private func report(ok: Bool) -> VerifyReport {
        VerifyReport(ok: ok, verified: 3, head: "abc", failedAt: ok ? nil : 2,
                     kind: ok ? nil : .signatureMismatch, detail: nil,
                     message: ok ? "chain ok" : "chain broke")
    }

    func testVerifiedIsThePinnedKeySigningAPassingWalkAndNothingElse() {
        let other = String(repeating: "ef", count: 32)
        let doc = SealedLogFetch.document("{}")
        typealias S = VerificationStanding

        XCTAssertEqual(S.derive(pinnedKey: key, fetch: doc, report: report(ok: true), servedKey: key), .verified)
        XCTAssertEqual(S.derive(pinnedKey: key, fetch: doc, report: report(ok: false), servedKey: key), .failedAgainstPin)
        XCTAssertEqual(S.derive(pinnedKey: key, fetch: doc, report: report(ok: true), servedKey: other),
                       .keyChanged(pinned: key, served: other))
        XCTAssertEqual(S.derive(pinnedKey: key, fetch: doc, report: report(ok: true), servedKey: nil),
                       .keyChanged(pinned: key, served: ""),
                       "a log that does not state its key cannot match a pin")
        XCTAssertEqual(S.derive(pinnedKey: nil, fetch: doc, report: report(ok: true), servedKey: key), .unpaired,
                       "unpaired, even a passing walk is only 'not yet pinned'")
        XCTAssertEqual(S.derive(pinnedKey: key, fetch: .unauthorized, report: nil, servedKey: nil), .unauthorized)
        XCTAssertEqual(S.derive(pinnedKey: nil, fetch: .unauthorized, report: nil, servedKey: nil), .none,
                       "an unpaired TV refused by a gated kernel is today's no-verdict, not news")
        XCTAssertEqual(S.derive(pinnedKey: key, fetch: .absent, report: nil, servedKey: nil), .none)
        XCTAssertTrue(S.keyChanged(pinned: key, served: other).isAlarm)
        XCTAssertFalse(S.verified.isAlarm)
    }

    func testTheServedKeyIsReadButNeverTrusted() {
        XCTAssertEqual(VerificationStanding.servedKey(in: #"{"verifying_key":"\#(key.uppercased())","entries":[]}"#), key)
        XCTAssertNil(VerificationStanding.servedKey(in: #"{"entries":[]}"#))
        XCTAssertNil(VerificationStanding.servedKey(in: "<html>"))
    }
}
