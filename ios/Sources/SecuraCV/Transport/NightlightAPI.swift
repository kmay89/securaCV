// NightlightAPI.swift
//
// A thin client for the Canary Nightlight's glass settings surface — the
// same unauthenticated LAN-only endpoints every display serves
// (`/api/settings`, `/api/set?k=&v=`), plus the nightlight's own knobs the
// firmware adds to them (lamp scene / strength / auto, 12-hour clock).
//
// The device describes, the app renders: `scenes` comes back BY NAME from
// the device's own look-engine catalog, so a new scene ships to this screen
// with no App Store update. Same address discipline as DeviceAPI — private
// or .local hosts only, nothing phones home.

import Foundation

struct NightlightSettings: Sendable {
    var lampScene: Int = 0
    var lampAuto: Bool = true
    var lampPct: Int = 72
    var lampMaxDutyPct: Int = 50    // the HAL's heat ceiling, self-reported
    var clock12h: Bool = true
    var nightStartHH: Int = 20
    var nightEndHH: Int = 7
    var orientation: Int = 0        // 0/1/2/3 quarter turns clockwise
    var autoRotate: Bool = true     // the IMU follows real movement
    var scenes: [String] = []
}

enum NightlightAPI {
    /// GET /api/settings — tolerant by hand: unknown fields are ignored,
    /// missing ones keep their defaults, so an older firmware never breaks
    /// a newer app (or the other way round).
    static func settings(at base: URL, session: URLSession = .shared) async throws -> NightlightSettings {
        guard DeviceAPI.isPrivate(base) else { throw DeviceError.notPrivateAddress }
        var req = URLRequest(url: base.appendingPathComponent("/api/settings"))
        req.timeoutInterval = 4
        let (data, resp) = try await session.data(for: req)
        guard let http = resp as? HTTPURLResponse, http.statusCode == 200 else {
            throw DeviceError.http((resp as? HTTPURLResponse)?.statusCode ?? 0, "settings")
        }
        let obj = try JSONSerialization.jsonObject(with: data) as? [String: Any] ?? [:]
        var s = NightlightSettings()
        if let v = obj["lamp_scene"] as? Int { s.lampScene = v }
        if let v = obj["lamp_auto"] as? Int { s.lampAuto = v == 1 }
        if let v = obj["lamp_pct"] as? Int { s.lampPct = v }
        if let v = obj["lamp_max_duty_pct"] as? Int { s.lampMaxDutyPct = v }
        if let v = obj["clock_12h"] as? Int { s.clock12h = v == 1 }
        if let v = obj["night_start_hh"] as? Int { s.nightStartHH = v }
        if let v = obj["night_end_hh"] as? Int { s.nightEndHH = v }
        if let v = obj["orientation"] as? Int { s.orientation = v }
        if let v = obj["auto_rotate"] as? Int { s.autoRotate = v == 1 }
        if let v = obj["scenes"] as? [String] { s.scenes = v }
        return s
    }

    /// POST /api/set?k=&v= — one knob per request, exactly the contract the
    /// on-glass settings engine already validates and debounces.
    static func set(_ key: String, _ value: Int, at base: URL,
                    session: URLSession = .shared) async throws {
        guard DeviceAPI.isPrivate(base) else { throw DeviceError.notPrivateAddress }
        var comps = URLComponents(url: base.appendingPathComponent("/api/set"),
                                  resolvingAgainstBaseURL: false)
        comps?.queryItems = [URLQueryItem(name: "k", value: key),
                             URLQueryItem(name: "v", value: String(value))]
        guard let url = comps?.url else { throw DeviceError.http(0, "bad url") }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.timeoutInterval = 4
        let (_, resp) = try await session.data(for: req)
        guard let http = resp as? HTTPURLResponse, http.statusCode == 200 else {
            throw DeviceError.http((resp as? HTTPURLResponse)?.statusCode ?? 0, key)
        }
    }
}
