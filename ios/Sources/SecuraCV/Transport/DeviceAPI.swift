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

    /// Physical-presence confirm for gated settings (camera peek, etc.).
    func confirm() async throws { _ = try await postRaw("/api/v1/confirm", body: Data()) }

    // MARK: - plumbing

    private func get<T: Decodable>(_ path: String) async throws -> T {
        let data = try await getRaw(path)
        return try Self.decoder.decode(T.self, from: data)
    }

    private func getRaw(_ path: String) async throws -> Data {
        var req = URLRequest(url: base.appendingPathComponent(path))
        req.setValue(token, forHTTPHeaderField: "X-Canary-Token")
        return try await send(req)
    }

    private func postRaw(_ path: String, body: Data) async throws -> Data {
        var req = URLRequest(url: base.appendingPathComponent(path))
        req.httpMethod = "POST"
        req.httpBody = body
        req.setValue(token, forHTTPHeaderField: "X-Canary-Token")
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        return try await send(req)
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
