// FirmwareUpdateAPI.swift
//
// The phone's two doors to one Canary's update engine (FirmwareUpdate.swift
// has the contract and the honesty rules). Same three routes, two trust
// models:
//   * a display answers on the home network without a credential, like its
//     other writes (glass_web.cpp: the LAN is the trust boundary, and the
//     worst a caller can do is start a check for firmware the device then
//     verifies against its pinned release key). It refuses a request whose
//     Host is not its own name or whose Origin is another site — a native
//     URLSession request sends no Origin and dials the device by its own
//     name, so neither guard is in the way;
//   * a paired WAP answers only with its bearer token over its pinned https,
//     so it goes through DeviceAPI, which carries both.

import Foundation

protocol FirmwareTransport: Sendable {
    /// The engine's status. Nil when the firmware predates the routes (404).
    func status() async throws -> FirmwareStatus?
    /// Ask for a check (install == false) or a check-and-install.
    func kick(install: Bool) async throws -> FirmwareKick
}

/// A display, over plain http on the home network.
struct GlassFirmwareTransport: FirmwareTransport {
    let base: URL
    var session: URLSession = .shared

    func status() async throws -> FirmwareStatus? {
        guard DeviceAPI.isPrivate(base) else { throw DeviceError.notPrivateAddress }
        var req = URLRequest(url: base.appendingPathComponent("/api/ota/status"))
        req.timeoutInterval = 5
        let (data, resp) = try await session.data(for: req)
        let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
        if code == 404 { return nil }
        guard code == 200 else { throw DeviceError.http(code, "update status") }
        return try JSONDecoder().decode(FirmwareStatus.self, from: data)
    }

    func kick(install: Bool) async throws -> FirmwareKick {
        guard DeviceAPI.isPrivate(base) else { throw DeviceError.notPrivateAddress }
        var req = URLRequest(url: base.appendingPathComponent(install ? "/api/ota/install" : "/api/ota/check"))
        req.httpMethod = "POST"
        req.httpBody = Data()
        req.timeoutInterval = 8
        let (data, resp) = try await session.data(for: req)
        return FirmwareUpdate.kick(status: (resp as? HTTPURLResponse)?.statusCode ?? 0, body: data)
    }
}

/// A paired WAP, through its authenticated, pinned client.
struct WapFirmwareTransport: FirmwareTransport {
    let api: DeviceAPI

    func status() async throws -> FirmwareStatus? { try await api.otaStatus() }
    func kick(install: Bool) async throws -> FirmwareKick { try await api.otaKick(install: install) }
}
