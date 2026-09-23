// SnapshotVault.swift
//
// The phone's half of the sealed-snapshot format: parse a `.svlt` file's
// 64-byte header and open it with the operator's X25519 key, exactly as
// tools/unseal_snapshot.py does and as the canary-wap firmware seals it
// (vault_snapshot.cpp; docs/sealed_snapshot_vault.md). Pure CryptoKit and
// Foundation — no UI, no Keychain, no network — so every byte of it is
// host-testable, and SnapshotVaultTests holds it to the SAME golden header
// hex as the C++ and python tests plus a byte-exact parity fixture
// (tools/fixtures/vault/svlt_parity.json): a construction that differs
// anywhere fails at the tag, so it cannot "succeed wrong".
//
// Construction (mirrors vault_snapshot.cpp / unseal_snapshot.py exactly):
//   shared = X25519(operator_priv, ephemeral_pub)
//   key    = HKDF-SHA256(salt = ephemeral_pub ‖ operator_pub,
//                        ikm = shared, info = "securacv/vault/seal/v1", 32)
//   plain  = ChaCha20-Poly1305.open(key, nonce, ct ‖ tag, aad = header)
//
// Header layout (little-endian), 64 bytes:
//   0..3  magic "SVLT"     4  version (1)     5  trigger     6  time bucket
//   7     reserved         8..15 key id = SHA-256(operator_pub)[0..<8]
//   16..47 ephemeral X25519 public key   48..59 nonce   60..63 ct_len u32
//
// Vocabulary: a "sealed snapshot" is ONE camera frame encrypted to ONE
// recipient by the Canary that took it. It is not the kernel's break-glass
// evidence vault, which opens only by quorum (Invariant V) and which this
// app has no path into. Keep the two words apart in every string.

import CryptoKit
import Foundation

/// The file as the system knows it. Info.plist exports the type
/// (UTExportedTypeDeclarations) and claims it (CFBundleDocumentTypes);
/// SnapshotVaultTests reads the plist off disk and holds it to these two
/// strings, so the importer's filter and the document door cannot drift.
enum SvltFile {
    static let typeIdentifier = "com.securacv.svlt"
    static let fileExtension = "svlt"
}

/// What made the Canary take the frame — the header's trigger byte, and
/// the tag the firmware puts in the ring filename (`vault_logic.h`).
enum SvltTrigger: UInt8, CaseIterable, Sendable {
    case smoke = 1
    case co = 2
    case glass = 3
    case motion = 4
    case mesh = 5
    case test = 9

    /// The filename / wire tag (`seal_00000001_smoke.svlt`, `/api/vault/list`).
    var tag: String {
        switch self {
        case .smoke: return "smoke"
        case .co: return "co"
        case .glass: return "glass"
        case .motion: return "motion"
        case .mesh: return "mesh"
        case .test: return "test"
        }
    }

    init?(tag: String) {
        guard let hit = Self.allCases.first(where: { $0.tag == tag }) else { return nil }
        self = hit
    }

    /// Plain words for a row. Coarse on purpose: the trigger is the only
    /// "why" the file carries, and this is all of it.
    var label: String {
        switch self {
        case .smoke: return "Smoke alarm"
        case .co: return "CO alarm"
        case .glass: return "Glass break"
        case .motion: return "Presence change"
        case .mesh: return "Mesh peer alarm"
        case .test: return "Test capture"
        }
    }
}

enum SnapshotVaultError: Error, LocalizedError, Equatable {
    case tooShort
    case badMagic
    case unsupportedVersion(UInt8)
    case unknownTrigger(UInt8)
    case bucketOutOfRange(Int)
    case ciphertextLengthOutOfRange(Int)
    /// The body is not exactly ct_len + 16 bytes — truncated or trailing data.
    case lengthMismatch(expected: Int, actual: Int)
    /// Sealed for a different key. Both ids, so the user can see which.
    case keyIDMismatch(file: String, key: String)
    /// Wrong key OR the file was tampered with — the tag cannot tell which,
    /// and neither can we (the same wording as the repo's unseal tool).
    case authenticationFailed
    /// Seal helper only: nothing, or more than the firmware would ever seal.
    case plaintextOutOfRange(Int)

    var errorDescription: String? {
        switch self {
        case .tooShort:
            return "This file is shorter than a sealed-snapshot header."
        case .badMagic:
            return "This isn't a sealed snapshot (.svlt) file."
        case .unsupportedVersion(let v):
            return "This sealed snapshot uses format version \(v), which this app doesn't read."
        case .unknownTrigger(let t):
            return "This sealed snapshot names a trigger (\(t)) this app doesn't know."
        case .bucketOutOfRange(let b):
            return "This sealed snapshot's time bucket (\(b)) is out of range."
        case .ciphertextLengthOutOfRange(let n):
            return "This sealed snapshot's length (\(n) bytes) is out of range."
        case .lengthMismatch(let expected, let actual):
            return "This sealed snapshot is \(actual) bytes but its header says \(expected) — "
                + "it may be truncated or have trailing data."
        case .keyIDMismatch(let file, let key):
            return "This snapshot was sealed for key \(file); the key on this phone is \(key). "
                + "It can only be opened by the key it was sealed to."
        case .authenticationFailed:
            return "Couldn't open this snapshot — wrong key, or the file was changed after it "
                + "was sealed (the authentication tag did not verify)."
        case .plaintextOutOfRange(let n):
            return "Nothing to seal, or too much (\(n) bytes)."
        }
    }
}

/// The 64-byte `.svlt` header. `parse` applies exactly the bounds
/// unseal_snapshot.py's parse_header does; `bytes` builds what build_header
/// builds (reserved byte 0). A header that parses is not a header that
/// verifies — the file's raw header bytes are the AAD, and the tag check in
/// `SnapshotSealer.unseal` is what proves them.
struct SvltHeader: Equatable, Sendable {
    static let size = 64
    static let magic = Data("SVLT".utf8)
    static let version: UInt8 = 1
    static let tagSize = 16
    /// 512 KiB — the firmware's MAX_CIPHERTEXT, a single JPEG's worth.
    static let maxCiphertext = 512 * 1024
    static let maxBucket = 143

    // `let`, all of them: a header is validated once, in `init`, and a
    // field that could be reassigned afterwards could be reassigned past
    // the bounds `bytes` relies on.
    let trigger: SvltTrigger
    /// 0…143 ten-minute buckets — the ONLY time information stored
    /// (Invariant III: coarse, never a timestamp).
    let bucket: Int
    let keyID: Data
    let ephemeralPub: Data
    let nonce: Data
    let ctLen: Int

    init(trigger: SvltTrigger, bucket: Int, keyID: Data, ephemeralPub: Data,
         nonce: Data, ctLen: Int) throws {
        guard (0...Self.maxBucket).contains(bucket) else {
            throw SnapshotVaultError.bucketOutOfRange(bucket)
        }
        guard ctLen > 0, ctLen <= Self.maxCiphertext else {
            throw SnapshotVaultError.ciphertextLengthOutOfRange(ctLen)
        }
        precondition(keyID.count == 8 && ephemeralPub.count == 32 && nonce.count == 12,
                     "SvltHeader field sizes are fixed by the format")
        self.trigger = trigger
        self.bucket = bucket
        self.keyID = Data(keyID)
        self.ephemeralPub = Data(ephemeralPub)
        self.nonce = Data(nonce)
        self.ctLen = ctLen
    }

    /// Parse the first 64 bytes of a file. Slices are copied into a plain
    /// array first: a `Data` slice keeps its parent's indices, and a
    /// zero-based subscript on one is the classic silent off-by-N.
    static func parse(_ raw: Data) throws -> SvltHeader {
        guard raw.count >= size else { throw SnapshotVaultError.tooShort }
        let b = [UInt8](raw.prefix(size))
        guard Data(b[0..<4]) == magic else { throw SnapshotVaultError.badMagic }
        guard b[4] == version else { throw SnapshotVaultError.unsupportedVersion(b[4]) }
        guard let trigger = SvltTrigger(rawValue: b[5]) else {
            throw SnapshotVaultError.unknownTrigger(b[5])
        }
        let bucket = Int(b[6])
        guard bucket <= maxBucket else { throw SnapshotVaultError.bucketOutOfRange(bucket) }
        let ctLen = Int(b[60]) | Int(b[61]) << 8 | Int(b[62]) << 16 | Int(b[63]) << 24
        guard ctLen > 0, ctLen <= maxCiphertext else {
            throw SnapshotVaultError.ciphertextLengthOutOfRange(ctLen)
        }
        return try SvltHeader(trigger: trigger, bucket: bucket,
                              keyID: Data(b[8..<16]), ephemeralPub: Data(b[16..<48]),
                              nonce: Data(b[48..<60]), ctLen: ctLen)
    }

    /// The header as the firmware writes it — the AAD when sealing. (An
    /// unseal authenticates the FILE's bytes instead; see `unseal`.)
    var bytes: Data {
        var out = Data(capacity: Self.size)
        out.append(Self.magic)
        out.append(contentsOf: [Self.version, trigger.rawValue, UInt8(bucket), 0])
        out.append(keyID)
        out.append(ephemeralPub)
        out.append(nonce)
        let n = UInt32(ctLen)
        out.append(contentsOf: [UInt8(n & 0xFF), UInt8((n >> 8) & 0xFF),
                                UInt8((n >> 16) & 0xFF), UInt8((n >> 24) & 0xFF)])
        return out
    }

    var keyIDHex: String { ChainVerifier.hex(keyID) }

    /// The bucket as the ten-minute window it names — "14:30 – 14:40-ish".
    /// Coarse by construction: the file carries no clock, so nothing finer
    /// could be true. (Buckets count the Canary's own day, on its clock —
    /// csi_event_current_bucket.) Nil for anything outside 0…143: the
    /// firmware's list answers 255 for a file whose header it could not
    /// read, and a clamp would print that as "23:50" — a time that is not
    /// true. Never guess one.
    static func bucketRange(_ bucket: Int) -> String? {
        guard (0...maxBucket).contains(bucket) else { return nil }
        let start = bucket * 10
        let end = (start + 10) % (24 * 60)
        return String(format: "%02d:%02d – %02d:%02d-ish",
                      start / 60, start % 60, end / 60, end % 60)
    }

    /// Always a string: a parsed header's bucket is in range by `init`.
    var bucketRange: String { Self.bucketRange(bucket) ?? "time unknown" }
}

/// One opened frame. Lives in memory for exactly as long as the screen
/// shows it — never written to disk, Photos, the pasteboard or the cloud.
struct UnsealedFrame: Sendable {
    let jpeg: Data
    let header: SvltHeader
}

enum SnapshotSealer {
    /// The HKDF info string — the same bytes on every side of this format.
    static let hkdfInfo = Data("securacv/vault/seal/v1".utf8)

    /// The recipient id the header carries: SHA-256(operator_pub)[0..<8].
    static func keyID(of publicKey: Curve25519.KeyAgreement.PublicKey) -> Data {
        Data(SHA256.hash(data: publicKey.rawRepresentation).prefix(8))
    }

    static func keyID(ofRawPublicKey raw: Data) -> Data {
        Data(SHA256.hash(data: raw).prefix(8))
    }

    /// HKDF-SHA256 over the X25519 shared secret, salted with
    /// ephemeral_pub ‖ operator_pub — the ORDER is part of the format.
    static func deriveKey(shared: SharedSecret, ephemeralPub: Data,
                          operatorPub: Data) -> SymmetricKey {
        shared.hkdfDerivedSymmetricKey(using: SHA256.self,
                                       salt: ephemeralPub + operatorPub,
                                       sharedInfo: hkdfInfo,
                                       outputByteCount: 32)
    }

    /// Every check an unseal makes BEFORE it needs the private key, in
    /// unseal_snapshot.py's cmd_unseal order: header bounds, body length,
    /// key id. Public-key only, so the Unseal screen runs it before asking
    /// for Face ID — a file sealed to some other key never prompts.
    @discardableResult
    static func precheck(file: Data, recipientPublicKey operatorPub: Data) throws -> SvltHeader {
        let header = try SvltHeader.parse(file)
        let expectedBody = header.ctLen + SvltHeader.tagSize
        let actualBody = file.count - SvltHeader.size
        guard actualBody == expectedBody else {
            throw SnapshotVaultError.lengthMismatch(expected: SvltHeader.size + expectedBody,
                                                    actual: file.count)
        }
        let ourID = keyID(ofRawPublicKey: operatorPub)
        guard ourID == header.keyID else {
            throw SnapshotVaultError.keyIDMismatch(file: header.keyIDHex,
                                                   key: ChainVerifier.hex(ourID))
        }
        return header
    }

    /// Open a sealed file with the operator's private key: `precheck`, then
    /// the tag.
    static func unseal(file: Data, key: Curve25519.KeyAgreement.PrivateKey) throws -> UnsealedFrame {
        let operatorPub = key.publicKey.rawRepresentation
        let header = try precheck(file: file, recipientPublicKey: operatorPub)
        let ephemeral: Curve25519.KeyAgreement.PublicKey
        let shared: SharedSecret
        do {
            ephemeral = try Curve25519.KeyAgreement.PublicKey(rawRepresentation: header.ephemeralPub)
            shared = try key.sharedSecretFromKeyAgreement(with: ephemeral)
        } catch {
            // A low-order or malformed ephemeral point: CryptoKit refuses
            // the agreement, and a file whose ephemeral key cannot agree is
            // one nothing could have sealed — the tag would never verify.
            throw SnapshotVaultError.authenticationFailed
        }
        let symmetric = deriveKey(shared: shared, ephemeralPub: header.ephemeralPub,
                                  operatorPub: operatorPub)
        // The AAD is the file's OWN 64 bytes, exactly as read — never
        // `header.bytes`, which is rebuilt from the parsed fields and so
        // writes the reserved byte as 0 whatever the file says. Rebuilding
        // would let a flipped reserved byte open (and would refuse a future
        // firmware that used it); unseal_snapshot.py authenticates
        // `data[:HEADER_SIZE]` for the same reason.
        let aad = Data([UInt8](file.prefix(SvltHeader.size)))
        let body = [UInt8](file.dropFirst(SvltHeader.size))
        let ciphertext = Data(body[0..<header.ctLen])
        let tag = Data(body[header.ctLen..<body.count])
        let plain: Data
        do {
            let box = try ChaChaPoly.SealedBox(nonce: ChaChaPoly.Nonce(data: header.nonce),
                                               ciphertext: ciphertext, tag: tag)
            plain = try ChaChaPoly.open(box, using: symmetric, authenticating: aad)
        } catch {
            throw SnapshotVaultError.authenticationFailed
        }
        return UnsealedFrame(jpeg: plain, header: header)
    }

    /// Seal exactly the way the firmware / cmd_seal does. A TEST helper —
    /// the app never seals anything (the Canary does) — with the ephemeral
    /// key and nonce injectable so a fixture can be reproduced byte for
    /// byte; production randomness is `Curve25519.KeyAgreement.PrivateKey()`
    /// and `ChaChaPoly.Nonce()` when a caller passes nothing.
    static func seal(plain: Data,
                     recipient: Curve25519.KeyAgreement.PublicKey,
                     trigger: SvltTrigger,
                     bucket: Int,
                     ephemeral: Curve25519.KeyAgreement.PrivateKey = .init(),
                     nonce: Data = Data(ChaChaPoly.Nonce())) throws -> Data {
        guard plain.count > 0, plain.count <= SvltHeader.maxCiphertext else {
            throw SnapshotVaultError.plaintextOutOfRange(plain.count)
        }
        // First, so a wrong-size nonce is a thrown error here and never
        // reaches the header's fixed-size precondition.
        let chachaNonce = try ChaChaPoly.Nonce(data: nonce)
        let operatorPub = recipient.rawRepresentation
        let ephemeralPub = ephemeral.publicKey.rawRepresentation
        let shared = try ephemeral.sharedSecretFromKeyAgreement(with: recipient)
        let symmetric = deriveKey(shared: shared, ephemeralPub: ephemeralPub,
                                  operatorPub: operatorPub)
        let header = try SvltHeader(trigger: trigger, bucket: bucket,
                                    keyID: keyID(ofRawPublicKey: operatorPub),
                                    ephemeralPub: ephemeralPub, nonce: nonce,
                                    ctLen: plain.count)
        let box = try ChaChaPoly.seal(plain, using: symmetric,
                                      nonce: chachaNonce,
                                      authenticating: header.bytes)
        return header.bytes + box.ciphertext + box.tag
    }
}

/// The ring filename the firmware serves and accepts —
/// `seal_<8 digits>_<smoke|co|glass|motion|mesh|test>.svlt`, exactly
/// `vault_logic::filename_parse`. This is the traversal gate on BOTH ends:
/// the Canary refuses any other `?name=`, and DeviceAPI refuses to put any
/// other name into a URL, so a list row that somehow carried `../` never
/// becomes a request.
enum VaultFilename {
    static func parse(_ name: String) -> (seq: Int, trigger: SvltTrigger)? {
        let s = Array(name.utf8)
        // "seal_" + 8 digits + "_" + tag + ".svlt"; the shortest tag is "co".
        guard s.count >= 5 + 8 + 1 + 2 + 5, s.count <= 5 + 8 + 1 + 6 + 5 else { return nil }
        guard String(decoding: s[0..<5], as: UTF8.self) == "seal_" else { return nil }
        var seq = 0
        for i in 5..<13 {
            guard s[i] >= UInt8(ascii: "0"), s[i] <= UInt8(ascii: "9") else { return nil }
            seq = seq * 10 + Int(s[i] - UInt8(ascii: "0"))
        }
        guard s[13] == UInt8(ascii: "_") else { return nil }
        let rest = String(decoding: s[14...], as: UTF8.self)
        guard rest.hasSuffix(".svlt") else { return nil }
        let tag = String(rest.dropLast(".svlt".count))
        guard let trigger = SvltTrigger(tag: tag) else { return nil }
        return (seq, trigger)
    }

    static func isValid(_ name: String) -> Bool { parse(name) != nil }
}
