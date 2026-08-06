// Witness.swift
//
// One Canary's live state, a Swift value-type mirror of `struct Witness` in
// fleet_model.h. Deliberately a plain, Codable, Sendable struct: it survives
// being cached, synced through CloudKit, or handed to a widget, and adding a
// firmware field later means adding one optional here — never a schema break.

import Foundation

struct Witness: Identifiable, Codable, Hashable, Sendable {
    var id: String                       // stable device_id, e.g. "canary-a3f7"
    var deviceType: DeviceType = .unknown
    /// The device type EXACTLY as the witness published it (`/api/fleet`
    /// "product", mDNS TXT `dt`) — kept beside the decoded enum because the
    /// enum is deliberately coarse: it collapses the whole display line to
    /// one case and folds types it has never heard of into `.unknown`. The
    /// raw string is what the figure lookup resolves at full precision
    /// (`FleetFigure.resolve`), so a "canary-watch" draws the round drum the
    /// moment the generated map carries it, with no app change. Nil when no
    /// transport told us.
    var publishedType: String?
    var name: String = ""                // user-assigned display name
    var room: String = ""
    var fingerprint: String = ""         // 16-hex pubkey fp; first 4 = chirp id

    // Liveness
    var link: Liveness = .unknown
    var lastSeen: Date?
    var seenViaBLE: Bool = false         // heard directly over BLE, no broker/WiFi

    // Trust
    var badge: TrustBadge = .unknown
    var chainLength: UInt32 = 0
    var firmware: String = ""

    // Last event (edge-triggered)
    var lastEvent: String = ""
    var lastEventAt: Date?
    var lastEventSeverity: Severity = .ok

    // Level-triggered conditions
    var tamper: Bool = false
    var tamperKind: String = ""
    var batteryPct: Int? = nil           // nil = unknown / not present
    var rssiDBM: Int? = nil

    // Radar / wellbeing surface (canary-sense) — coarse by design, never
    // per-target. nil keeps "not published" distinct from a real zero.
    var radarPresent: Bool? = nil        // nil unknown / false clear / true present
    var radarOccupants: Int? = nil       // 0 / 1 / 2 (=2+)
    var breathingLock: Bool? = nil
    var tempC: Double? = nil
    var humidityPct: Int? = nil

    // Per-witness mute: caps nagging at Notice but stays visible; tamper and a
    // failed chain verify still punch through (a hidden bypass is how alarms
    // get missed — this one can't hide).
    var mutedUntil: Date?

    // Address the app reaches it at (mDNS name preferred over IP — IPs churn
    // on DHCP, mDNS names don't). Nil for MQTT-only sensors with no HTTP.
    var baseURL: URL?

    var isMuted: Bool {
        guard let until = mutedUntil else { return false }
        return until > Date()
    }

    /// The severity the UI should show for this row, honoring mute but never
    /// letting tamper or a failed signature be silenced.
    var effectiveSeverity: Severity {
        var sev = displaySeverity
        if isMuted && !tamper && badge != .failed {
            sev = min(sev, .notice)
        }
        return sev
    }

    /// Raw (pre-mute) severity from the worst live condition. Internal on
    /// purpose: mute caps NAGGING (effectiveSeverity), but anything that
    /// reasons about whether an alarm is LIVE — the mood engine's
    /// alarm-unacked rule, the alert ledger — must read the pre-mute truth,
    /// or a muted alarm could dress the app in a calm it hasn't earned.
    var displaySeverity: Severity {
        if tamper { return .tamper }
        if badge == .failed || link.isDark { return .alert }
        var sev = lastEventSeverity
        if link == .stale { sev = max(sev, .warn) }
        if let b = batteryPct, b >= 0, b < 15 { sev = max(sev, .warn) }
        return sev
    }

    var displayName: String {
        if !name.isEmpty { return name }
        if !room.isEmpty { return room }
        return id
    }
}

extension Witness {
    /// A stable, human one-liner for the row subtitle.
    var statusLine: String {
        if tamper { return tamperKind.isEmpty ? "Tamper detected" : tamperKind }
        if badge == .failed { return "Signature did not verify" }
        if link.isDark { return "Gone dark" }
        if !lastEvent.isEmpty { return lastEvent }
        return link.label
    }
}
