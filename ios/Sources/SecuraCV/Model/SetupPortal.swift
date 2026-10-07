// SetupPortal.swift
//
// A Canary's own first-boot setup network, as a contract the phone can
// speak — pure, so every judgment here is host-tested.
//
// Every Canary that has no Wi-Fi yet raises a SoftAP named `SecuraCV-XXXX`
// (four characters of its salted pseudonym; a `-xx` suffix when the key
// could not be made durable) with a captive page at 192.168.4.1. The
// display line and the shared portal the camera and radar Canaries use
// (firmware/common/network/setup_portal.cpp, canary-display's
// provision.cpp) serve the same three routes:
//
//   GET  /scan     → {"networks":[{"ssid","rssi","secure"}], "tz": …} or {"scanning":true}
//   POST /join     ← form-urlencoded ssid=…&pass=…[&tz=…]   → {"ok":true} | 400 {"ok":false,"reason":…}
//   GET  /status   → {"state":"idle"|"connecting"|"success"|"fail","reason":…}
//
// The key to that network is shown where the device can show it: a display
// draws a `WIFI:` QR on its glass; a headless Canary's key is printed by the
// Flasher after the flash. The phone joins the setup network itself
// (NEHotspotConfiguration — the walkthrough never sends the owner to
// Settings), posts the home Wi-Fi, and watches /status until the device
// says it is across.
//
// canary-wap is the exception: its own wizard lives at /companion and its
// preferred path is the bonded Bluetooth provisioning service the app
// already speaks (BLEConsole.writeWiFiCredentials); SetupGuide says so.

import Foundation

enum SetupPortal {
    static let ssidPrefix = "SecuraCV-"
    static let host = "192.168.4.1"
    static let baseURL = URL(string: "http://192.168.4.1")!

    /// Is this a Canary's setup network? `SecuraCV-A1B2` or `SecuraCV-A1B2-xy`
    /// — four characters after the prefix, an optional two-character suffix.
    static func isSetupNetwork(_ ssid: String) -> Bool {
        guard ssid.hasPrefix(ssidPrefix) else { return false }
        let rest = ssid.dropFirst(ssidPrefix.count)
        let parts = rest.split(separator: "-", omittingEmptySubsequences: false)
        guard let first = parts.first, first.count == 4,
              first.allSatisfy({ $0.isLetter || $0.isNumber }) else { return false }
        if parts.count == 1 { return true }
        return parts.count == 2 && parts[1].count == 2
            && parts[1].allSatisfy({ $0.isLetter || $0.isNumber })
    }

    /// The `POST /join` body. The portal's page posts form fields, not
    /// JSON; the time zone is optional and the device keeps its own when
    /// it is left out.
    static func joinBody(ssid: String, password: String, timeZone: String? = nil) -> String {
        var pairs = [("ssid", ssid), ("pass", password)]
        if let tz = timeZone, !tz.isEmpty { pairs.append(("tz", tz)) }
        return HubOnboarding.formEncode(pairs)
    }

    /// The firmware's own limits, checked before anything is sent so the
    /// walkthrough can say why instead of relaying a bare "bad request".
    static func credentialProblem(ssid: String, password: String) -> String? {
        let bytes = ssid.utf8.count
        if bytes == 0 { return "Type your Wi-Fi's name." }
        if bytes > 32 { return "A Wi-Fi name can be at most 32 bytes." }
        if password.utf8.count > 64 { return "A Wi-Fi password can be at most 64 characters." }
        return nil
    }

    /// `{"ok":true}` or `{"ok":false,"reason":"…"}` from `/join`.
    static func parseJoinReply(_ data: Data) -> (ok: Bool, reason: String?) {
        guard let v = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return (false, "The Canary's answer wasn't readable.")
        }
        return ((v["ok"] as? Bool) ?? false, v["reason"] as? String)
    }

    /// Where the device stands with the join, from `/status`.
    enum JoinState: Equatable, Sendable {
        case idle, connecting, success
        case fail(reason: String)
    }

    static func parseStatus(_ data: Data) -> JoinState? {
        guard let v = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let state = v["state"] as? String else { return nil }
        switch state {
        case "idle": return .idle
        case "connecting": return .connecting
        case "success": return .success
        case "fail": return .fail(reason: (v["reason"] as? String) ?? "The Canary couldn't join.")
        default: return nil
        }
    }

    /// The `WIFI:T:WPA;S:<ssid>;P:<pass>;H:<hidden>;;` grammar — what a
    /// display draws on its glass for its own setup network (and what any
    /// router's sticker QR says). Escapes `\;`, `\:`, `\,` and `\\` as the
    /// grammar defines them; keys are case-insensitive, order is free.
    struct WiFiQR: Equatable, Sendable {
        var ssid: String
        var password: String
        var hidden: Bool
    }

    static func parseWiFiQR(_ text: String) -> WiFiQR? {
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard t.uppercased().hasPrefix("WIFI:") else { return nil }
        var fields: [String: String] = [:]
        var key = ""
        var value = ""
        var inKey = true
        var escaped = false
        for ch in t.dropFirst(5) {
            if escaped { value.append(ch); escaped = false; continue }
            if ch == "\\" && !inKey { escaped = true; continue }
            if inKey {
                if ch == ":" { inKey = false } else { key.append(ch) }
            } else if ch == ";" {
                fields[key.uppercased()] = value
                key = ""; value = ""; inKey = true
            } else {
                value.append(ch)
            }
        }
        if !key.isEmpty { fields[key.uppercased()] = value }
        guard let ssid = fields["S"], !ssid.isEmpty else { return nil }
        let hidden = (fields["H"] ?? "").lowercased() == "true"
        return WiFiQR(ssid: ssid, password: fields["P"] ?? "", hidden: hidden)
    }
}
