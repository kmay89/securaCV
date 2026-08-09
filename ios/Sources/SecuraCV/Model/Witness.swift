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
    /// WHICH BOARD this is — the `hw` a device publishes on `/api/fleet` and
    /// in its mDNS advert, spelled the way its pins header does
    /// (`CANARY_FIGURE_HARDWARE`). Nil when no transport told us, which is
    /// every device on firmware older than this field.
    ///
    /// Kept beside `publishedType` because it answers a different question,
    /// and a stricter one: the type says what a device CALLS itself, the board
    /// says what it IS. Several products share one type and one board can
    /// serve two products, so only this pins down the shape — it is the first
    /// thing `FleetFigure.resolve` asks. It is NOT a product name: see
    /// `FleetFigure.sharesBoardAcrossProducts` before printing a figure's
    /// title beside one.
    var hardware: String?
    /// Where this Canary stands with its MQTT hub, as it reported it. The
    /// fleet can run entirely without one — a display renders from the LAN and
    /// this phone talks to devices directly — so `.none` is a state to EXPLAIN,
    /// never an error to raise. `.unknown` means the device said nothing, which
    /// is every device on firmware older than the field.
    var hub: HubState = .unknown
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

    /// The day this Canary's KEY was born, in days since the Unix epoch, as the
    /// device itself reports it (`/api/fleet` `born_day`). Nil until the device
    /// has met a believable clock, or for a device with no key of its own.
    /// This is deliberately NOT the pairing date: pairing is a fact about this
    /// phone, and two people looking at the same Canary would see two different
    /// answers. See firmware/common/identity/birth_day.h.
    var bornDay: Int?
    /// False means `bornDay` is when the device was first DATED, not born —
    /// the app says so rather than promoting a shelf into a birthday.
    var bornExact: Bool = false

    /// The birth day as a date, at UTC midnight. Nil when the device has never
    /// reported one. Rendered day-only everywhere: a birth day carries no time
    /// of day by construction, and inventing one would be the first decorative
    /// thing on a screen whose job is being checkable.
    var bornOn: Date? {
        guard let bornDay, bornDay > 0 else { return nil }
        return Date(timeIntervalSince1970: TimeInterval(bornDay) * 86_400)
    }

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
