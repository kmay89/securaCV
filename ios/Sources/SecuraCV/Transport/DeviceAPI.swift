// DeviceAPI.swift
//
// A thin async client for a WAP-class Canary's local HTTP API
// (canary-vision/docs/api.md). Every request carries the per-device
// X-Canary-Token; the base URL is validated to be a private/.local address
// before any request goes out (the SPA does the same — public addresses are
// rejected, matching the "nothing phones home" promise). Decoding is tolerant:
// new firmware fields are ignored, missing ones default, so the app never
// hard-fails on a version it predates.
//
// TLS (roadmap row 5): a WAP that serves https presents a self-signed
// certificate the system trust store can never accept, so URLSession.shared
// simply failed every request to one. The pairing receipt carries that
// certificate's SHA-256 (`tls_cert_fp`); DeviceAPI keeps it and answers the
// server-trust challenge with it (PinnedTrustDelegate, bottom of this file).
// An https device WITHOUT a pin is refused, not trusted — a TLS device the
// app cannot check is not "verified" in any sense this project uses.

import CryptoKit
import Foundation
import Security

struct DeviceInfo: Codable, Sendable {
    var deviceID: String
    var name: String
    var model: String
    var firmwareVersion: String
    var uptimeS: Int?
    var wifiRSSI: Int?
    var ip: String?
    var capabilities: [String]

    enum CodingKeys: String, CodingKey {
        case deviceID = "device_id", name, model
        case firmwareVersion = "firmware_version"
        case uptimeS = "uptime_s"
        case wifiRSSI = "wifi_rssi"
        case ip, capabilities
    }
}

/// The one-shot receipt handed over after a physical BOOT-tap (Trust-on-Pair).
struct ProvisioningReceipt: Codable, Sendable {
    var deviceID: String
    var baseURL: URL
    var token: String
    /// `tls_cert_fp`: the SHA-256 (64 hex) of the Canary's TLS certificate
    /// DER, as canary_wap.ino writes it. Nil when the receipt carried none
    /// or an empty string (an http-only device). The pin an https base URL
    /// needs before DeviceAPI will dial it.
    var tlsCertFingerprint: String?

    // Accept the documented alternate field names too.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: DynamicKey.self)
        func str(_ keys: [String]) -> String? {
            for k in keys { if let v = try? c.decode(String.self, forKey: DynamicKey(k)) { return v } }
            return nil
        }
        deviceID = str(["device_id"]) ?? ""
        token = str(["token", "api_token"]) ?? ""
        tlsCertFingerprint = TLSPin.normalize(str(["tls_cert_fp"]))
        let urlString = str(["base_url", "host"]) ?? ""
        // Tolerance has a floor: a receipt without a token would "pair" a
        // device whose every authenticated call then 401s with no explanation.
        // (deviceID may stay empty — PairView falls back to the discovered id.)
        guard !token.isEmpty, let url = URL(string: urlString) else {
            throw DeviceError.badReceipt
        }
        baseURL = url
    }
}

/// `GET /api/wifi`, the fields the rollout cares about. Tolerant: anything
/// the firmware adds later decodes right past us.
struct WiFiStatus: Codable, Sendable {
    var ok: Bool?
    var state: String?
    var staConnected: Bool?
    var staSSID: String?
    var rssi: Int?
    var configured: Bool?
    var failReason: String?

    enum CodingKeys: String, CodingKey {
        case ok, state, rssi, configured
        case staConnected = "sta_connected"
        case staSSID = "sta_ssid"
        case failReason = "fail_reason"
    }
}

/// The `{"ok": …, "error": …}` envelope the WAP Wi-Fi routes answer with.
struct WiFiReply: Codable, Sendable {
    var ok: Bool?
    var error: String?
}

/// `GET /api/status` on a WAP — the subset of its (large) body the poll
/// needs to stand a paired row up: identity, firmware, and the chain head.
/// Tolerant like every wire struct here; the WAP serves no `name` key at
/// all (the pairing receipt's name is the display name), and no field in
/// this body is a verification — `fingerprint` is the device's own claim,
/// so the poll keeps deriving the one it trusts from the TOFU-pinned key.
struct WapStatus: Codable, Sendable {
    var ok: Bool?
    var deviceID: String?
    var deviceType: String?
    var firmware: String?
    var chainSeq: UInt64?

    enum CodingKeys: String, CodingKey {
        case ok, firmware
        case deviceID = "device_id"
        case deviceType = "device_type"
        case chainSeq = "chain_seq"
    }
}

enum DeviceError: Error, LocalizedError {
    case notPrivateAddress
    case http(Int, String)
    case badReceipt
    case notPairable
    /// An https Canary presented a certificate whose SHA-256 is not the one
    /// its pairing receipt named. The connection was cut before any request.
    case certificateMismatch
    /// An https Canary with no certificate fingerprint on record — refused,
    /// because a TLS device the app cannot check is not a checked device.
    case tlsPinMissing

    var errorDescription: String? {
        switch self {
        case .notPrivateAddress: return "That address isn't on your local network."
        case .http(let code, let msg): return "Device error \(code): \(msg)"
        case .badReceipt: return "That pairing receipt couldn't be read."
        case .notPairable: return "This Canary is paired through Home Assistant, not here."
        case .certificateMismatch:
            return "This Canary's secure certificate doesn't match the one in its pairing "
                + "receipt, so the connection was refused. Something may be answering in its "
                + "place — check the device itself. If you reset or replaced it, re-pair it "
                + "to get its new receipt."
        case .tlsPinMissing:
            return "This Canary uses a secure (https) connection, but its pairing receipt "
                + "carried no certificate fingerprint, so the connection can't be checked and "
                + "was refused. Update the Canary's firmware and pair it again from its setup page."
        }
    }
}

actor DeviceAPI {
    private let base: URL
    private let token: String
    private let session: URLSession
    /// The trust delegate behind `session` when this device speaks TLS —
    /// asked, after a cut connection, whether it was the pin that cut it.
    private let pinDelegate: PinnedTrustDelegate?

    /// - tlsFingerprint: the pairing receipt's `tls_cert_fp`. Required when
    ///   `base` is https (a TLS Canary without a pin is refused with
    ///   `.tlsPinMissing`); ignored for http.
    /// - session: tests only — a caller-supplied session skips pinning.
    init(base: URL, token: String, tlsFingerprint: String? = nil,
         session: URLSession? = nil) throws {
        guard Self.isPrivate(base) else { throw DeviceError.notPrivateAddress }
        self.base = base
        self.token = token
        if let session {
            self.session = session
            self.pinDelegate = nil
        } else if Self.isTLS(base) {
            guard let pin = TLSPin.normalize(tlsFingerprint) else {
                throw DeviceError.tlsPinMissing
            }
            let pinned = PinnedSessions.shared.session(pinnedTo: pin)
            self.session = pinned.session
            self.pinDelegate = pinned.delegate
        } else {
            self.session = .shared
            self.pinDelegate = nil
        }
    }

    /// Does this base URL speak TLS? The one place the scheme is inspected.
    static func isTLS(_ url: URL) -> Bool { url.scheme?.lowercased() == "https" }

    func info() async throws -> DeviceInfo { try await get("/api/v1/info") }

    func witness(last: Int = 20) async throws -> WitnessChainPage {
        try await get("/api/v1/witness",
                      query: [URLQueryItem(name: "last", value: String(max(1, min(100, last))))])
    }

    func config() async throws -> Data { try await getRaw("/api/v1/config") }

    // MARK: - the WAP dialect (the device-api this repo's firmware actually serves)

    /// `GET /api/status` — the WAP's Bearer-gated status body, decoded down
    /// to what the poll needs. This is the liveness-plus-identity read for
    /// WAP-class devices: no firmware in this repo serves `/api/v1/info`,
    /// so a poll that insists on the v1 contract marks every real WAP dark.
    func wapStatus() async throws -> WapStatus { try await get("/api/status") }

    /// `GET /api/events/today` — the WAP's sensing-event feed (open bundles
    /// first, then committed ring rows newest-first, ≤72 rows, Bearer-gated;
    /// Wire/WapEvents.swift documents the row). An empty array is normal —
    /// the ring is RAM-only and empties on reboot.
    func wapEventsToday() async throws -> WapEventsToday {
        try await get("/api/events/today")
    }

    /// `GET /api/csi/stream` — the 1 Hz sensing snapshot (a plain GET, not
    /// SSE; Bearer-gated; Wire/WapEvents.swift documents its three variant
    /// bodies). The Sensing-now tile polls this while visible.
    func wapStream() async throws -> WapStream { try await get("/api/csi/stream") }

    // MARK: - the fleet-wide surface

    /// `GET /api/fleet` — the coarse self-report EVERY networked Canary answers
    /// (`firmware/common/fleet_selfreport`), as opposed to the `/api/v1/*`
    /// device-api contract only WAP-class boards serve. Deliberately:
    ///
    ///   * **static** — it needs no paired device, only a reachable address;
    ///   * **unauthenticated** — the firmware serves it with no token check and
    ///     `Access-Control-Allow-Origin: *`, because the body is coarse presence
    ///     and health, never anything extractive. This is the Wi-Fi twin of the
    ///     BLE presence beacon: what a Canary will tell anyone who asks.
    ///
    /// Still gated on `isPrivate` — the app never dials a public address.
    static func fleetSelfReport(at base: URL, session: URLSession = .shared) async throws -> FleetSelfReport {
        guard isPrivate(base) else { throw DeviceError.notPrivateAddress }
        var req = URLRequest(url: base.appendingPathComponent("/api/fleet"))
        // Probing several discovered hosts must not stall the refresh loop: a
        // board that is off, asleep, or serving no HTTP should fail fast.
        req.timeoutInterval = 4
        let (data, resp) = try await session.data(for: req)
        if let http = resp as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            throw DeviceError.http(http.statusCode, "")
        }
        return try FleetSelfReport.decode(data)
    }

    /// `GET /api/fleet` over THIS device's session — the pinned one when it
    /// speaks TLS — so a paired https Canary's own self row is reachable.
    /// The static form above stays for discovered, unpaired hosts, which
    /// are dialed over plain http.
    func fleetSelfReport() async throws -> FleetSelfReport {
        var req = URLRequest(url: base.appendingPathComponent("/api/fleet"))
        req.timeoutInterval = 4
        let data = try await send(req)
        return try FleetSelfReport.decode(data)
    }

    /// Physical-presence confirm for gated settings (camera peek, etc.).
    func confirm() async throws { _ = try await postRaw("/api/v1/confirm", body: Data()) }

    /// `POST /api/identify` — ask the Canary to make itself known: ~15 s of
    /// LED blink plus its chirp (Hue-style). Returns true when the device
    /// says it is set to VISUAL-ONLY (its chirp is disabled), so the Find
    /// screen can say "watch for the blink" instead of promising a sound
    /// that will not come.
    func identify(durationMS: Int = 15_000) async throws -> Bool {
        struct Reply: Codable {
            var ok: Bool?
            var visualOnly: Bool?
            enum CodingKeys: String, CodingKey { case ok; case visualOnly = "visual_only" }
        }
        let body = try JSONEncoder().encode(["duration_ms": durationMS])
        let data = try await postRaw("/api/identify", body: body)
        let reply = try? JSONDecoder().decode(Reply.self, from: data)
        if reply?.ok == false {
            throw DeviceError.http(200, "The Canary declined to identify.")
        }
        return reply?.visualOnly ?? false
    }

    // MARK: - the Wi-Fi surface (fleet credential rollout)

    /// The head of the witness chain, one record's worth — the cheap "did
    /// anything happen?" read the 5-second sentinel can afford. Nil when the
    /// device reports an empty chain. WAP dialect first (`chain_seq` on
    /// `/api/status` — the contract this repo's firmware serves), then the
    /// v1 page for the reference device-api; same both-dialects spirit as
    /// `authorize(_:)` and `publicKey()`.
    func witnessHeadSeq() async throws -> UInt64? {
        if let status = try? await wapStatus(), let seq = status.chainSeq, seq > 0 {
            return seq
        }
        let page: WitnessChainPage = try await get("/api/v1/witness",
                                                   query: [URLQueryItem(name: "last", value: "1")])
        return page.records.map(\.seq).max()
    }

    /// `GET /api/wifi` — where this Canary stands with its network, as it
    /// tells it. Tolerantly decoded; only the fields the rollout needs.
    func wifiStatus() async throws -> WiFiStatus {
        try await get("/api/wifi")
    }

    /// `POST /api/wifi/connect` — hand this Canary new credentials. The
    /// firmware validates (SSID 1–32, password ≤ 64), persists to NVS, and
    /// kicks its connect state machine; the device then LEAVES this network,
    /// so no answer after the accept is expected — the rollout verifies by
    /// watching for the device to answer again, not by trusting this call.
    func wifiConnect(ssid: String, password: String) async throws {
        struct Body: Encodable { let ssid: String; let password: String }
        let body = try JSONEncoder().encode(Body(ssid: ssid, password: password))
        let data = try await postRaw("/api/wifi/connect", body: body)
        // The firmware answers 200 with {"ok": false, "error": …} on
        // validation refusals — surface that as the failure it is.
        if let reply = try? JSONDecoder().decode(WiFiReply.self, from: data), reply.ok == false {
            throw DeviceError.http(200, reply.error ?? "The Canary refused the credentials.")
        }
    }

    // MARK: - the device's signing key (TOFU pinning)

    /// The device's raw 32-byte Ed25519 public key — fetched once, at first
    /// sight, so `PinnedKeyStore.pin` has a key to pin (the pairing receipt
    /// deliberately carries only id/URL/token). Both dialects are tried, same
    /// spirit as `authorize(_:)`: the WAP firmware serves the key as 64 hex
    /// chars in `/api/status` ("pubkey"); the reference device-api serves it
    /// SPKI-PEM-encoded in `/api/v1/witness/export` ("public_key_pem").
    func publicKey() async throws -> Data {
        if let data = try? await getRaw("/api/status"),
           let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let hex = obj["pubkey"] as? String,
           let key = Data(hexString: hex), key.count == 32 {
            return key
        }
        let data = try await getRaw("/api/v1/witness/export")
        guard let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let pem = obj["public_key_pem"] as? String,
              let key = Self.ed25519Key(fromSPKIPEM: pem) else {
            throw DeviceError.http(200, "The device presented no public key.")
        }
        return key
    }

    /// Extract the raw 32-byte Ed25519 key from an SPKI PEM. The DER for an
    /// Ed25519 SubjectPublicKeyInfo is a fixed 12-byte prefix followed by the
    /// raw key, so anything else is rejected rather than guessed at.
    static func ed25519Key(fromSPKIPEM pem: String) -> Data? {
        let base64 = pem
            .components(separatedBy: .newlines)
            .filter { !$0.hasPrefix("-----") }
            .joined()
            .trimmingCharacters(in: .whitespaces)
        guard let der = Data(base64Encoded: base64) else { return nil }
        let prefix: [UInt8] = [0x30, 0x2A, 0x30, 0x05, 0x06, 0x03,
                               0x2B, 0x65, 0x70, 0x03, 0x21, 0x00]
        guard der.count == prefix.count + 32,
              [UInt8](der.prefix(prefix.count)) == prefix else { return nil }
        return Data(der.suffix(32))
    }

    // MARK: - plumbing

    private func get<T: Decodable>(_ path: String, query: [URLQueryItem]? = nil) async throws -> T {
        let data = try await getRaw(path, query: query)
        return try Self.decoder.decode(T.self, from: data)
    }

    private func getRaw(_ path: String, query: [URLQueryItem]? = nil) async throws -> Data {
        var req = URLRequest(url: url(for: path, query: query))
        authorize(&req)
        return try await send(req)
    }

    private func url(for path: String, query: [URLQueryItem]?) -> URL {
        Self.requestURL(base: base, path: path, query: query)
    }

    /// Build a request URL. Queries ride URLComponents (as NightlightAPI's
    /// do) — `appendingPathComponent` percent-encodes '?', so a "?last=N"
    /// baked into the path becomes the literal path `witness%3Flast=N` and
    /// the device answers 404. Static + pure so the tests can pin it.
    static func requestURL(base: URL, path: String, query: [URLQueryItem]?) -> URL {
        let plain = base.appendingPathComponent(path)
        guard let query, !query.isEmpty else { return plain }
        var comps = URLComponents(url: plain, resolvingAgainstBaseURL: false)
        comps?.queryItems = query
        return comps?.url ?? plain
    }

    private func postRaw(_ path: String, body: Data) async throws -> Data {
        var req = URLRequest(url: base.appendingPathComponent(path))
        req.httpMethod = "POST"
        req.httpBody = body
        authorize(&req)
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        return try await send(req)
    }

    /// Both credential dialects, every request. The `/api/v1/*` contract
    /// (canary-vision/docs/api.md) reads `X-Canary-Token`; the WAP admin
    /// routes (`/api/wifi/*`, `/api/identify`) read `Authorization: Bearer`.
    /// Each side ignores the header it doesn't know, so sending both means
    /// one client speaks to both dialects without a per-route table that
    /// would drift the first time firmware moved an endpoint.
    private func authorize(_ req: inout URLRequest) {
        req.setValue(token, forHTTPHeaderField: "X-Canary-Token")
        req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
    }

    private func send(_ req: URLRequest) async throws -> Data {
        let result: (Data, URLResponse)
        do {
            result = try await session.data(for: req)
        } catch let error as URLError
                    where pinDelegate?.sawMismatch == true
                        && (error.code == .cancelled
                            || error.code == .userCancelledAuthentication
                            || error.code == .serverCertificateUntrusted) {
            // The pin cut the handshake: name that, not a bare "canceled".
            throw DeviceError.certificateMismatch
        }
        let (data, resp) = result
        guard let http = resp as? HTTPURLResponse else { return data }
        guard (200..<300).contains(http.statusCode) else {
            let msg = (try? Self.decoder.decode([String: String].self, from: data))?["message"] ?? ""
            throw DeviceError.http(http.statusCode, msg)
        }
        return data
    }

    static let decoder: JSONDecoder = {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .custom { dec in
            let s = try dec.singleValueContainer().decode(String.self)
            return ISO8601DateFormatter.witness.date(from: s)
                ?? ISO8601DateFormatter().date(from: s)
                ?? Date()
        }
        return d
    }()

    /// Turn an mDNS TXT `host` value into a URL we can actually dial.
    ///
    /// The canonical TXT schema (canary-vision/docs/discovery.md) documents
    /// `host=canary-a3f7.local`, but the firmware writes the raw mDNS hostname:
    /// `make_hostname()` produces a BARE label like `canary-display-a1b2c3`,
    /// with no domain. Building `http://canary-display-a1b2c3` from that is
    /// rejected by `isPrivate` and the request is never made — so the whole
    /// `/api/fleet` path would silently do nothing for exactly the boards it
    /// exists to reach. Appending the implied `.local` is what makes a bare
    /// mDNS label resolvable.
    ///
    /// A value that already carries a dot (a `.local` name or an IP) is left
    /// alone; `isPrivate` remains the gate either way.
    static func url(forDiscoveredHost host: String) -> URL? {
        let trimmed = host.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        let name = trimmed.contains(".") ? trimmed : trimmed + ".local"
        return URL(string: "http://\(name)")
    }

    /// Only RFC-1918 / loopback / link-local / .local hosts are allowed to be
    /// contacted. This is the app's "nothing phones home" gate: every base URL
    /// (paired receipt, discovered host, nightlight, glass) passes through it.
    ///
    /// Every label must be a decimal octet and there must be exactly four.
    /// The first version did `split(".").compactMap { Int($0) }`, which DROPPED
    /// non-numeric labels — so `10.0.0.1.attacker.com` collapsed to 10.0.0.1
    /// and a crafted pairing receipt could point the app at a public host.
    static func isPrivate(_ url: URL) -> Bool {
        guard let rawHost = url.host, !rawHost.isEmpty else { return false }
        let host = rawHost.lowercased()
        if host == "localhost" { return true }
        if host.hasSuffix(".local") {
            // "<something>.local" only — never ".local" alone, never a host
            // with an embedded scheme/at-sign that URL parsing let through.
            return host.count > ".local".count && !host.contains("@") && !host.hasPrefix(".")
        }
        let labels = host.split(separator: ".", omittingEmptySubsequences: false)
        guard labels.count == 4 else { return false }
        var octets: [UInt8] = []
        octets.reserveCapacity(4)
        for label in labels {
            guard !label.isEmpty, label.count <= 3,
                  label.allSatisfy({ $0.isASCII && $0.isNumber }),
                  let value = UInt8(label) else { return false }
            octets.append(value)
        }
        switch (octets[0], octets[1]) {
        case (10, _): return true
        case (192, 168): return true
        case (172, 16...31): return true
        case (127, _): return true
        case (169, 254): return true   // link-local: a Canary on a hotspot with no DHCP
        default: return false
        }
    }
}

private struct DynamicKey: CodingKey {
    var stringValue: String
    var intValue: Int? { nil }
    init(_ s: String) { stringValue = s }
    init?(stringValue: String) { self.stringValue = stringValue }
    init?(intValue: Int) { nil }
}

// MARK: - TLS pinning (the receipt's tls_cert_fp)

/// The one fact that makes an https Canary reachable AND checkable: the
/// SHA-256 of its self-signed certificate's DER bytes, handed over in the
/// pairing receipt (`tls_cert_fp`, canary_wap.ino send_provisioning_receipt).
/// Public data — the same bytes any TLS client sees — so an exact compare is
/// the right compare; what makes it a pin is that it arrived over the
/// physical-presence receipt, not over the network being checked.
enum TLSPin {
    /// Lowercase hex SHA-256 of a DER-encoded certificate — the firmware's
    /// `sha256_raw(cert_der)` as hex.
    static func fingerprintHex(ofDER der: Data) -> String {
        SHA256.hash(data: der).map { String(format: "%02x", $0) }.joined()
    }

    /// A receipt value as a canonical pin: case folded, `:`/space/`-`
    /// separators dropped; nil unless exactly 64 hex digits remain (an
    /// http device's receipt carries the empty string).
    static func normalize(_ raw: String?) -> String? {
        guard let raw else { return nil }
        let cleaned = raw.lowercased().filter { !($0 == ":" || $0 == "-" || $0.isWhitespace) }
        guard cleaned.count == 64, cleaned.allSatisfy({ $0.isHexDigit }) else { return nil }
        return cleaned
    }

    /// Does this DER certificate hash to the pinned fingerprint?
    static func matches(der: Data, pinned: String) -> Bool {
        guard let pin = normalize(pinned) else { return false }
        return fingerprintHex(ofDER: der) == pin
    }
}

/// Answers URLSession's server-trust challenge with the receipt's pin: the
/// leaf certificate's DER must hash to it, and nothing else is consulted —
/// a Canary's certificate is self-signed, so the system trust store has
/// nothing to say about it either way. On a mismatch the challenge is
/// canceled and the mismatch remembered, so DeviceAPI can name the failure
/// (`.certificateMismatch`) instead of surfacing a bare "canceled".
final class PinnedTrustDelegate: NSObject, URLSessionDelegate, @unchecked Sendable {
    let pinnedFingerprint: String
    private let lock = NSLock()
    private var mismatch = false

    init(pinnedFingerprint: String) {
        self.pinnedFingerprint = pinnedFingerprint
    }

    /// True once any connection through this delegate was cut for a
    /// mismatched (or missing) leaf certificate.
    var sawMismatch: Bool {
        lock.lock()
        defer { lock.unlock() }
        return mismatch
    }

    private func noteMismatch() {
        lock.lock()
        mismatch = true
        lock.unlock()
    }

    func urlSession(_ session: URLSession,
                    didReceive challenge: URLAuthenticationChallenge,
                    completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        let space = challenge.protectionSpace
        guard space.authenticationMethod == NSURLAuthenticationMethodServerTrust,
              let trust = space.serverTrust else {
            completionHandler(.performDefaultHandling, nil)
            return
        }
        guard let chain = SecTrustCopyCertificateChain(trust) as? [SecCertificate],
              let leaf = chain.first else {
            noteMismatch()
            completionHandler(.cancelAuthenticationChallenge, nil)
            return
        }
        let der = SecCertificateCopyData(leaf) as Data
        if TLSPin.matches(der: der, pinned: pinnedFingerprint) {
            completionHandler(.useCredential, URLCredential(trust: trust))
        } else {
            noteMismatch()
            completionHandler(.cancelAuthenticationChallenge, nil)
        }
    }
}

/// One pinned URLSession per fingerprint, for the app's lifetime. A session
/// retains its delegate and lives until invalidated, and DeviceAPI is built
/// per call (DeviceStore.api(for:) on every poll), so a session per
/// DeviceAPI would leak one every twenty seconds. Ephemeral configuration:
/// no disk cache, no cookies — a Canary's answers are not to be cached.
final class PinnedSessions: @unchecked Sendable {
    static let shared = PinnedSessions()

    private let lock = NSLock()
    private var byFingerprint: [String: (session: URLSession, delegate: PinnedTrustDelegate)] = [:]

    func session(pinnedTo fingerprint: String) -> (session: URLSession, delegate: PinnedTrustDelegate) {
        lock.lock()
        defer { lock.unlock() }
        if let existing = byFingerprint[fingerprint] { return existing }
        let delegate = PinnedTrustDelegate(pinnedFingerprint: fingerprint)
        let config = URLSessionConfiguration.ephemeral
        config.waitsForConnectivity = false
        let session = URLSession(configuration: config, delegate: delegate, delegateQueue: nil)
        let entry = (session: session, delegate: delegate)
        byFingerprint[fingerprint] = entry
        return entry
    }
}
