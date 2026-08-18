// DeviceAPI.swift
//
// A thin async client for a WAP-class Canary's local HTTP API
// (canary-vision/docs/api.md). Every request carries the per-device
// X-Canary-Token; the base URL is validated to be a private/.local address
// before any request goes out (the SPA does the same — public addresses are
// rejected, matching the "nothing phones home" promise). Decoding is tolerant:
// new firmware fields are ignored, missing ones default, so the app never
// hard-fails on a version it predates.

import Foundation

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

    // Accept the documented alternate field names too.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: DynamicKey.self)
        func str(_ keys: [String]) -> String? {
            for k in keys { if let v = try? c.decode(String.self, forKey: DynamicKey(k)) { return v } }
            return nil
        }
        deviceID = str(["device_id"]) ?? ""
        token = str(["token", "api_token"]) ?? ""
        let urlString = str(["base_url", "host"]) ?? ""
        guard let url = URL(string: urlString) else {
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

enum DeviceError: Error, LocalizedError {
    case notPrivateAddress
    case http(Int, String)
    case badReceipt
    case notPairable

    var errorDescription: String? {
        switch self {
        case .notPrivateAddress: return "That address isn't on your local network."
        case .http(let code, let msg): return "Device error \(code): \(msg)"
        case .badReceipt: return "That pairing receipt couldn't be read."
        case .notPairable: return "This Canary is paired through Home Assistant, not here."
        }
    }
}

actor DeviceAPI {
    private let base: URL
    private let token: String
    private let session: URLSession

    init(base: URL, token: String, session: URLSession = .shared) throws {
        guard Self.isPrivate(base) else { throw DeviceError.notPrivateAddress }
        self.base = base
        self.token = token
        self.session = session
    }

    func info() async throws -> DeviceInfo { try await get("/api/v1/info") }

    func witness(last: Int = 20) async throws -> WitnessChainPage {
        try await get("/api/v1/witness?last=\(max(1, min(100, last)))")
    }

    func config() async throws -> Data { try await getRaw("/api/v1/config") }

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
    /// device reports an empty chain.
    func witnessHeadSeq() async throws -> UInt64? {
        let page: WitnessChainPage = try await get("/api/v1/witness?last=1")
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

    // MARK: - plumbing

    private func get<T: Decodable>(_ path: String) async throws -> T {
        let data = try await getRaw(path)
        return try Self.decoder.decode(T.self, from: data)
    }

    private func getRaw(_ path: String) async throws -> Data {
        var req = URLRequest(url: base.appendingPathComponent(path))
        authorize(&req)
        return try await send(req)
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
        let (data, resp) = try await session.data(for: req)
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

    /// Only RFC-1918 / loopback / .local hosts are allowed to be contacted.
    static func isPrivate(_ url: URL) -> Bool {
        guard let host = url.host else { return false }
        if host.hasSuffix(".local") || host == "localhost" { return true }
        let parts = host.split(separator: ".").compactMap { Int($0) }
        guard parts.count == 4 else { return false }
        switch (parts[0], parts[1]) {
        case (10, _): return true
        case (192, 168): return true
        case (172, 16...31): return true
        case (127, _): return true
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
