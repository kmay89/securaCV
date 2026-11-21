//  WallPairingTests.swift — the pairing receipt, the store, and the one fold
//  that decides what a walk may claim.
//
//  The rules these pin: a receipt without a key to pin is refused (a token
//  alone would be trust on first use by another name); a source is one
//  pairing however its address is spelled; forgetting takes the token AND
//  the pin; VerificationStanding.derive gives "verified" to exactly one
//  situation — the pinned key signing a log whose walk passed — and a
//  paired Wall served no log at all is told so (pairedNoLog), after enough
//  cycles in a row that one timeout cannot flap the banner.

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

    private func report(ok: Bool, verified: UInt64 = 3) -> VerifyReport {
        VerifyReport(ok: ok, verified: verified, head: "abc", failedAt: ok ? nil : 2,
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
        XCTAssertEqual(S.derive(pinnedKey: key, fetch: .absent, report: nil, servedKey: nil), .pairedNoLog,
                       "paired and served nothing is news, never the calm self-report")
        XCTAssertTrue(S.keyChanged(pinned: key, served: other).isAlarm)
        XCTAssertFalse(S.verified.isAlarm)
    }

    func testAPairedWallServedNoLogIsToldSoAndAnUnpairedOneIsNot() {
        // Something at the hub's address answers the roll-call but not the
        // sealed log: nothing at all (.absent — a timeout, a 404, a 3xx the
        // Wall refuses to follow), or a 200 whose body is not a sealed log
        // (the core reports that walk as malformed with no failing entry,
        // and the model drops the verdict). Paired, both are the check this
        // TV was paired to make NOT happening, so both read pairedNoLog —
        // a warning in its own right, never folded into .none. Unpaired,
        // the same answers stay .none: the Wall promised nothing.
        typealias S = VerificationStanding
        let notALog = SealedLogFetch.document("<html>Sign in</html>")

        XCTAssertEqual(S.derive(pinnedKey: key, fetch: .absent, report: nil, servedKey: nil), .pairedNoLog)
        XCTAssertEqual(S.derive(pinnedKey: key, fetch: notALog, report: nil, servedKey: nil), .pairedNoLog,
                       "a 200 that is not a sealed log is no log served")
        XCTAssertEqual(S.derive(pinnedKey: nil, fetch: .absent, report: nil, servedKey: nil), .none,
                       "unpaired and served nothing is today's no-verdict")
        XCTAssertEqual(S.derive(pinnedKey: nil, fetch: notALog, report: nil, servedKey: nil), .none)
        XCTAssertFalse(S.pairedNoLog.isAlarm, "no record served is a warning about the check, not an alarm about the record")
        XCTAssertNotEqual(S.pairedNoLog, .unauthorized, "a refusal is said as a refusal; this is the quieter absence")
    }

    // MARK: - the model says it only after several cycles in a row

    private let viewerToken = String(repeating: "7", count: 64)
    private let oneRowFleet = #"{"kernel":"kitchen-hub","devices":[{"name":"Front Door"}]}"#

    /// A single-source model over a sealed-log transport, pairings in
    /// memory — the model's cycle counting is under test, not the Keychain.
    @MainActor
    private func pairedModel(_ transport: any FleetTransport) -> WallModel {
        let defaults = UserDefaults(suiteName: UUID().uuidString)!
        defaults.set(["canary.local:8099"], forKey: "SecuraCVWallSources")
        let m = WallModel(transport: transport, defaults: defaults, pollInterval: 0.01,
                          pairings: PairedSourceStore(secrets: MemoryPairingSecrets()), discover: { _ in [] })
        XCTAssertNil(m.pair(receiptText: receiptJSON(token: viewerToken, key: key)))
        return m
    }

    @MainActor
    func testAPairedHubThatStopsServingTheLogIsSaidAfterSeveralCyclesNotOne() async {
        // The hub answers the roll-call every cycle and the sealed log
        // never (a firmware board's shape, at an address this TV paired
        // with). One such cycle is a timeout's worth of doubt and reads as
        // before; the Nth in a row is the warning.
        let transport = SealedLogTransport(fleet: [.success(oneRowFleet)], sealedLog: nil)
        let m = pairedModel(transport)
        let said = WallModel.noLogCyclesBeforeSaid
        XCTAssertGreaterThan(said, 1, "one missed fetch must not flap the banner")

        for cycle in 1..<said {
            await m.refreshOnce()
            XCTAssertEqual(m.standing, .none, "cycle \(cycle) of \(said): not yet said")
            XCTAssertNil(m.report)
        }
        await m.refreshOnce()
        XCTAssertEqual(m.standing, .pairedNoLog, "cycle \(said): the check stopped, and the Wall says so")
        XCTAssertNil(m.report, "no walk ran; nothing is drawn as a verdict")
        XCTAssertTrue(m.timeline.isEmpty)
        XCTAssertEqual(transport.tokensSent.count, said, "asked every cycle — an absence is not a refusal, so no lockout guard")
        XCTAssertTrue(transport.tokensSent.allSatisfy { $0 == viewerToken }, "the paired token goes to the paired source")

        await m.refreshOnce()
        XCTAssertEqual(m.standing, .pairedNoLog, "and keeps saying so while it lasts")
    }

    @MainActor
    func testANonLogAnswerCountsAsNoLogAndAWalkResetsTheCount() async {
        // A 200 that is not a sealed log is the same absence. Then the hub
        // serves a real log, under another key: a walk ran, so the standing
        // is that walk's (keyChanged) and the count starts over — the next
        // run of absences is said after the full N again, not after N minus
        // what came before the walk.
        let transport = SwitchingSealedLogTransport(fleet: oneRowFleet, sealedLog: "<html>Sign in</html>")
        let m = pairedModel(transport)
        let said = WallModel.noLogCyclesBeforeSaid
        for _ in 0..<said { await m.refreshOnce() }
        XCTAssertEqual(m.standing, .pairedNoLog, "a login page in the log's place is no log served")

        transport.sealedLog = tamperedLogUnderAnotherKey
        await m.refreshOnce()
        XCTAssertNotNil(m.report, "a sealed log was served, and walked")
        XCTAssertEqual(m.standing, .keyChanged(pinned: key, served: tamperedLogKey),
                       "the walk's own standing, not a leftover no-log warning")

        transport.sealedLog = nil
        for cycle in 1..<said {
            await m.refreshOnce()
            XCTAssertEqual(m.standing, .none, "cycle \(cycle) after a walk: the count started over")
        }
        await m.refreshOnce()
        XCTAssertEqual(m.standing, .pairedNoLog)

        m.forgetPairing()
        XCTAssertEqual(m.standing, .none, "forgetting the pairing drops the standing with the pin")
        await m.refreshOnce()
        XCTAssertEqual(m.standing, .none, "unpaired, the same absence is today's no-verdict, not news")
    }

    /// The Ed25519 base point — a valid key — so the core reaches the chain
    /// walk and fails THERE (WallModelTests' tampered log), which is a
    /// verdict, not a non-answer.
    private let tamperedLogKey = "5866666666666666666666666666666666666666666666666666666666666666"
    private var tamperedLogUnderAnotherKey: String {
        """
        {
          "verifying_key": "\(tamperedLogKey)",
          "entries": [
            {"id": 7, "payload": "{}",
             "prev_hash": "\(String(repeating: "a", count: 64))",
             "entry_hash": "\(String(repeating: "b", count: 64))",
             "signature": "\(String(repeating: "c", count: 128))"}
          ]
        }
        """
    }

    func testAPinnedWalkThatCheckedNoSignatureIsNotVerified() {
        // `{"verifying_key": <pin>, "entries": []}` walks clean with nothing
        // checked — and the key is public, so anything answering at the
        // hub's address can serve it. A genuine hub just after a checkpoint
        // serves the same shape. Neither has earned "Verified".
        let doc = SealedLogFetch.document("{}")
        typealias S = VerificationStanding

        let empty = S.derive(pinnedKey: key, fetch: doc, report: report(ok: true, verified: 0), servedKey: key)
        XCTAssertEqual(empty, .pinnedNothingToCheck)
        XCTAssertNotEqual(empty, .verified, "zero signatures checked is not a signature checked against the pin")
        XCTAssertFalse(empty.isAlarm, "nothing to check is not a broken record either")
        XCTAssertEqual(S.derive(pinnedKey: key, fetch: doc, report: report(ok: true, verified: 1), servedKey: key),
                       .verified, "one signature checked against the pin is the floor")
        XCTAssertEqual(S.derive(pinnedKey: key, fetch: doc, report: report(ok: false, verified: 0), servedKey: key),
                       .failedAgainstPin, "a walk that failed on its first entry still alarms")
    }

    func testTheServedKeyIsReadButNeverTrusted() {
        XCTAssertEqual(VerificationStanding.servedKey(in: #"{"verifying_key":"\#(key.uppercased())","entries":[]}"#), key)
        XCTAssertNil(VerificationStanding.servedKey(in: #"{"entries":[]}"#))
        XCTAssertNil(VerificationStanding.servedKey(in: "<html>"))
    }
}


/// A transport whose sealed log can change between cycles — the seam for a
/// hub that stops, or starts, serving one. The fleet answer is constant.
private final class SwitchingSealedLogTransport: FleetTransport, @unchecked Sendable {
    let fleet: String
    var sealedLog: String?

    init(fleet: String, sealedLog: String?) {
        self.fleet = fleet
        self.sealedLog = sealedLog
    }

    func fetchFleet(from base: URL) async throws -> String { fleet }

    func fetchSealedLog(from base: URL, token: String?) async -> SealedLogFetch {
        guard let sealedLog else { return .absent }
        return .document(sealedLog)
    }
}
