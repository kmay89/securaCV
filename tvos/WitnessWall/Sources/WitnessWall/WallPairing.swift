//  WallPairing.swift — what this Apple TV holds to read a hub's sealed log,
//  and the key it pinned when it did.
//
//  The kernel's `GET /api/sealed-log` wants a credential, and the one it
//  hands its own clients — the capability token — rotates every ten minutes
//  in a file a television cannot read. So the operator mints a VIEWER token
//  once (`witness_api mint-viewer-token`; docs/homeassistant_setup.md,
//  "Viewer tokens") and gives the Wall the one line it prints: a pairing
//  receipt carrying the token AND the kernel's verifying key. The key is the
//  point. "Verified" on this screen means an Ed25519 signature checked
//  against a key PINNED at pairing (AGENTS.md) — so the pin comes from the
//  receipt, the operator's own word, never from whatever log the hub serves
//  first. The token opens that one read and nothing else on the hub.
//
//  Both live in the Keychain, in ONE item per source, never in UserDefaults:
//  tvOS may purge an app's defaults (WallModel keeps only rebuildable state
//  there), and a purged pin would silently demote "Verified" to trust on
//  first use. One item also makes forgetting atomic — the token and the pin
//  go together or not at all, the iPhone's DeviceStore.remove rule.
//
//  Every decision here is plain data in, plain data out (VerificationStanding
//  .derive included), so the model's honesty is testable without a network,
//  a Keychain, or a Siri Remote.

import Foundation
import Security

// MARK: - The receipt

/// The pairing receipt, as `witness_api mint-viewer-token` prints it:
/// `{"kernel","base_url"?,"sealed_log_token","verifying_key","token_id"}`.
///
/// Tolerant the way the iPhone's ProvisioningReceipt is — `token` is taken
/// for `sealed_log_token`, unknown keys decode right past — with a floor: a
/// receipt without a 64-hex token, or without a 64-hex verifying key, is
/// refused. A token without a key would pair a TV that can read the log but
/// has nothing to pin, which is exactly the trust on first use the pairing
/// exists to replace.
struct ViewerReceipt: Decodable, Equatable, Sendable {
    /// The bearer for `GET /api/sealed-log`, 64 lower-case hex.
    let token: String
    /// The kernel's Ed25519 verifying key when the token was minted, 64
    /// lower-case hex — the pin.
    let verifyingKey: String
    /// Where the hub answers, when the operator told the minter
    /// (`--base-url`); nil means "the address this Wall already polls".
    let baseURL: String?
    /// The id `revoke-viewer-token` takes, kept so settings can name which
    /// token this TV holds.
    let tokenID: String?

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: ReceiptKey.self)
        func string(_ keys: [String]) -> String? {
            for key in keys {
                if let value = try? c.decode(String.self, forKey: ReceiptKey(key)) {
                    let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
                    if !trimmed.isEmpty { return trimmed }
                }
            }
            return nil
        }
        guard let token = Self.hex64(string(["sealed_log_token", "token"])) else {
            throw PairingError.missingToken
        }
        guard let key = Self.hex64(string(["verifying_key"])) else {
            throw PairingError.missingKey
        }
        self.token = token
        self.verifyingKey = key
        self.baseURL = string(["base_url"])
        self.tokenID = string(["token_id"])
    }

    /// Read what a person pasted or typed. Text entry on a television may
    /// "smarten" straight quotes; every string in a receipt is hex or a URL,
    /// so folding curly quotes back is lossless, and the alternative is a
    /// receipt that fails for a reason nobody can see on screen.
    static func parse(_ text: String) throws -> ViewerReceipt {
        let folded = text
            .replacingOccurrences(of: "\u{201C}", with: "\"")
            .replacingOccurrences(of: "\u{201D}", with: "\"")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard folded.hasPrefix("{") else { throw PairingError.notAReceipt }
        do {
            return try JSONDecoder().decode(ViewerReceipt.self, from: Data(folded.utf8))
        } catch let error as PairingError {
            throw error
        } catch {
            throw PairingError.notAReceipt
        }
    }

    /// 64 hex characters, lower-cased — or nil for anything else.
    static func hex64(_ raw: String?) -> String? {
        guard let raw, raw.count == 64, raw.allSatisfy(\.isHexDigit) else { return nil }
        return raw.lowercased()
    }
}

/// A coding key for a receipt's documented field names and their aliases.
private struct ReceiptKey: CodingKey {
    let stringValue: String
    init(_ name: String) { stringValue = name }
    init?(stringValue: String) { self.stringValue = stringValue }
    var intValue: Int? { nil }
    init?(intValue: Int) { return nil }
}

/// Why a pairing did not happen. The messages are the settings panel's copy.
enum PairingError: LocalizedError, Equatable {
    case notAReceipt
    case missingToken
    case missingKey
    case noSingleSource
    case badBaseURL(String)
    case keychain(Int32)

    var errorDescription: String? {
        switch self {
        case .notAReceipt:
            return "That isn't a pairing receipt. Paste the one line the hub's mint-viewer-token printed — it starts with {."
        case .missingToken:
            return "That receipt carries no viewer token (64 hex characters under \"sealed_log_token\")."
        case .missingKey:
            return "That receipt carries no verifying key (64 hex characters under \"verifying_key\"). The Wall pins that key at pairing, so it cannot pair without one."
        case .noSingleSource:
            return "Pair with one hub at a time: connect to your hub's address first, or mint the receipt with --base-url so it names the hub."
        case .badBaseURL(let raw):
            return "The receipt's address \"\(raw)\" isn't one the Wall can reach."
        case .keychain(let status):
            return "This Apple TV's Keychain refused the pairing (status \(status)). Nothing was saved."
        }
    }
}

// MARK: - What this TV keeps

/// What this TV holds for one source: the viewer token and the key it
/// pinned, stored as one Keychain item so neither can outlive the other.
struct PairedSource: Codable, Equatable, Sendable {
    let token: String
    let verifyingKey: String
    let tokenID: String?
    let pairedAt: Date

    /// The first 12 hex characters of the pinned key — enough for a person
    /// to hold against the hub's own `verifying_key`, never shown as if it
    /// were the whole key.
    var keyPrefix: String { String(verifyingKey.prefix(12)) }
}

/// Where pairings are kept. The app uses the Keychain; tests use memory,
/// because an unsigned simulator host may be refused the Keychain outright
/// (CODE_SIGNING_ALLOWED=NO in tvos.yml), and the model's decisions must be
/// provable either way.
protocol PairingSecrets: Sendable {
    func read(account: String) -> Data?
    func write(_ data: Data, account: String) throws
    func remove(account: String)
}

/// The Keychain, as the iPhone keeps its per-device tokens: generic
/// passwords, `AfterFirstUnlockThisDeviceOnly` — readable by a wall that
/// boots unattended after a power cut, never carried to another device by
/// sync or by a restored backup. No entitlement is needed for one app's own
/// items.
struct KeychainPairingSecrets: PairingSecrets {
    static let service = "com.securacv.witnesswall.viewer"

    private func item(_ account: String) -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: Self.service,
            kSecAttrAccount as String: account,
        ]
    }

    func read(account: String) -> Data? {
        var query = item(account)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &out) == errSecSuccess else { return nil }
        return out as? Data
    }

    func write(_ data: Data, account: String) throws {
        SecItemDelete(item(account) as CFDictionary)
        var add = item(account)
        add[kSecValueData as String] = data
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let status = SecItemAdd(add as CFDictionary, nil)
        guard status == errSecSuccess else { throw PairingError.keychain(status) }
    }

    func remove(account: String) {
        SecItemDelete(item(account) as CFDictionary)
    }
}

/// Pairings, one per source.
struct PairedSourceStore: Sendable {
    let secrets: any PairingSecrets

    init(secrets: any PairingSecrets = KeychainPairingSecrets()) {
        self.secrets = secrets
    }

    /// The account a source's pairing is filed under: its scheme, host and
    /// port, lower-cased — so `192.168.1.20:8799` and
    /// `http://192.168.1.20:8799/api/fleet` are one pairing, while another
    /// port (another service on the same host) is another. Nil for an
    /// address the Wall could not dial anyway.
    static func account(for source: String) -> String? {
        guard let url = try? FleetAddress.normalize(source),
              let scheme = url.scheme?.lowercased(),
              let host = url.host?.lowercased() else { return nil }
        let bracketed = host.contains(":") ? "[\(host)]" : host
        return "\(scheme)://\(bracketed)" + (url.port.map { ":\($0)" } ?? "")
    }

    func pairing(for source: String) -> PairedSource? {
        guard let account = Self.account(for: source),
              let data = secrets.read(account: account) else { return nil }
        return try? JSONDecoder().decode(PairedSource.self, from: data)
    }

    /// File `receipt` under `source`, replacing any earlier pairing there.
    @discardableResult
    func pair(_ receipt: ViewerReceipt, source: String, now: Date = Date()) throws -> PairedSource {
        guard let account = Self.account(for: source) else {
            throw PairingError.badBaseURL(source)
        }
        let record = PairedSource(token: receipt.token,
                                  verifyingKey: receipt.verifyingKey,
                                  tokenID: receipt.tokenID,
                                  pairedAt: now)
        try secrets.write(try JSONEncoder().encode(record), account: account)
        return record
    }

    /// Forget the pairing with `source` — the token AND the pin, one item.
    func forget(_ source: String) {
        guard let account = Self.account(for: source) else { return }
        secrets.remove(account: account)
    }
}

// MARK: - What the walk may claim

/// Where THIS TV's walk of the sealed log stands against the key it pinned
/// at pairing. The header banner, the footer, the character and the timeline
/// all read it; only `.verified` may wear the word.
enum VerificationStanding: Equatable, Sendable {
    /// No walk this cycle: the source served no sealed log to us (an
    /// unpaired Wall gets the kernel's 401), the Wall polls several sources,
    /// or the source is gone.
    case none
    /// A walk ran, against the key the log itself supplied; nothing is
    /// pinned for this source. Internal consistency, not provenance — the
    /// banner says "not yet pinned" and never "Verified".
    case unpaired
    /// Paired; the log is signed by the pinned key and the walk passed.
    case verified
    /// Paired; the log is signed by the pinned key and the walk FAILED — the
    /// report's alarm, under the key this TV was told to trust.
    case failedAgainstPin
    /// Paired, but the log is signed by another key. An alarm, never a
    /// quiet downgrade: either the hub was re-keyed (re-pair to follow it —
    /// witness-core deliberately does not follow rotations on its own) or
    /// something else is answering at the hub's address.
    case keyChanged(pinned: String, served: String)
    /// Paired, and the source refused this TV's token: revoked, or the hub
    /// lost its viewer-token file.
    case unauthorized

    /// The fold, as plain data. `servedKey` is the document's own
    /// `verifying_key` (lower-cased), read only when the walk produced a
    /// verdict; `pinnedKey` is the pairing's, or nil when unpaired.
    static func derive(pinnedKey: String?, fetch: SealedLogFetch,
                       report: VerifyReport?, servedKey: String?) -> VerificationStanding {
        if pinnedKey != nil, fetch == .unauthorized { return .unauthorized }
        guard let report else { return .none }
        guard let pinnedKey else { return .unpaired }
        guard let servedKey, servedKey == pinnedKey else {
            return .keyChanged(pinned: pinnedKey, served: servedKey ?? "")
        }
        return report.ok ? .verified : .failedAgainstPin
    }

    /// The standing is itself an alarm, whatever the walk said.
    var isAlarm: Bool {
        if case .keyChanged = self { return true }
        return false
    }

    /// The key a sealed-log document says signed it, lower-cased — the
    /// document's claim about itself, which is only ever COMPARED against a
    /// pin, never trusted on its own.
    static func servedKey(in sealedLogJSON: String) -> String? {
        struct Claim: Decodable {
            let verifyingKey: String
            enum CodingKeys: String, CodingKey { case verifyingKey = "verifying_key" }
        }
        guard let claim = try? JSONDecoder().decode(Claim.self, from: Data(sealedLogJSON.utf8)) else {
            return nil
        }
        return ViewerReceipt.hex64(claim.verifyingKey)
    }
}
