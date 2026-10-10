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
// canary-wap is the exception: its own wizard lives at /companion and
// speaks its own routes, so the walkthrough explains it and the person
// drives it; its one-tap path is the Bluetooth setup door (SetupGuide).

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

    // MARK: - the networks the Canary itself can see

    /// One row of `GET /scan`: what the Canary's own radio heard. A network
    /// missing from this list is one the Canary cannot join, whatever the
    /// phone sees (these boards are 2.4 GHz only).
    struct Network: Equatable, Hashable, Sendable {
        var ssid: String
        var rssi: Int
        var secure: Bool
    }

    /// `{"networks":[{"ssid","rssi","secure"}], …}` → the list, strongest
    /// first, one row per name; `{"scanning":true}` → still sweeping (ask
    /// again in a moment). Nil for anything unreadable. Rows with an empty
    /// name (hidden networks) are dropped — there is nothing to pick.
    static func parseScan(_ data: Data) -> (networks: [Network], scanning: Bool)? {
        guard let v = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        if (v["scanning"] as? Bool) == true { return ([], true) }
        guard let rows = v["networks"] as? [[String: Any]] else { return nil }
        var best: [String: Network] = [:]
        for row in rows {
            guard let ssid = row["ssid"] as? String, !ssid.isEmpty else { continue }
            let rssi = (row["rssi"] as? NSNumber)?.intValue ?? -100
            let secure = (row["secure"] as? Bool) ?? true
            if let seen = best[ssid], seen.rssi >= rssi { continue }
            best[ssid] = Network(ssid: ssid, rssi: rssi, secure: secure)
        }
        let sorted = best.values.sorted { $0.rssi != $1.rssi ? $0.rssi > $1.rssi : $0.ssid < $1.ssid }
        return (sorted, false)
    }

    /// Which network to preselect: the remembered household Wi-Fi when the
    /// Canary can see it, else the strongest it heard, else none (the
    /// person types a name).
    static func preselect(_ networks: [Network], remembered: String?) -> String? {
        if let remembered, networks.contains(where: { $0.ssid == remembered }) { return remembered }
        return networks.first?.ssid
    }

    // MARK: - saying what went wrong, and what to do

    /// The firmware's join verdicts are short labels ("Wrong password",
    /// "Network not found", "No address from the router", "Couldn't
    /// connect" — firmware/common/network/wifi_join_policy.h). Its own
    /// portal page adds a tip to each (setup_portal.cpp `tip()`); this is
    /// that tip, so the phone never shows a bare label with no next step.
    static func advice(for reason: String) -> String {
        let r = reason.trimmingCharacters(in: CharacterSet(charactersIn: ". \n"))
        let lower = r.lowercased()
        let tip: String
        if lower.contains("password") {
            tip = "check for typos; it's case-sensitive"
        } else if lower.contains("not found") {
            tip = "this Canary only joins 2.4 GHz Wi-Fi, so a 5 GHz-only network is invisible to it"
        } else if lower.contains("connect") || lower.contains("address") {
            tip = "move it closer to your router and try again"
        } else {
            return r.isEmpty ? "The Canary couldn't join." : r + "."
        }
        return r + " — " + tip + "."
    }

    /// Before anything is sent: is the name one the Canary said it cannot
    /// see? Nil when it can, or when no list came back to judge by.
    static func notListedHint(ssid: String, listed: [String]) -> String? {
        let name = ssid.trimmingCharacters(in: .whitespaces)
        guard !name.isEmpty, !listed.isEmpty, !listed.contains(name) else { return nil }
        return "This Canary can't see \(name) — it only joins 2.4 GHz Wi-Fi. Pick one from its list, or move it closer to your router."
    }

    /// Before anything is sent: a WPA password shorter than 8 characters
    /// can never work, so say so instead of letting the Canary find out.
    static func passwordHint(_ password: String, networkIsOpen: Bool) -> String? {
        guard !networkIsOpen, !password.isEmpty, password.count < 8 else { return nil }
        return "A Wi-Fi password is at least 8 characters."
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
