// FleetEnums.swift  (SHARED — compiled into the app, both widget targets,
// and the watch app)
//
// A faithful Swift mirror of the firmware's fleet_model.h ladders
// (firmware/projects/canary-display/include/canary/fleet/fleet_model.h).
// Every Apple surface renders the SAME vocabulary the glass and the kernel
// speak — one source of truth for meaning, one copy of it in Swift (the
// widget target used to carry its own mirror of Severity; it must never
// again). Unknown future values decode to `.unknown` rather than throwing,
// so a newer firmware never breaks an older app (the anti-rot bet).

import SwiftUI

/// Severity ladder — mirrors `enum class Sev` and const.py's taxonomy.
/// Ordered so `<` / `max` compare the way the firmware intends.
enum Severity: UInt8, Comparable, Codable, CaseIterable {
    case ok = 0        // all quiet
    case notice        // routine witness activity (presence, occupancy)
    case warn          // needs a look: stale / low battery / after-hours
    case alert         // alarm pattern, chain verify FAILED, witness lost
    case tamper        // tamper / panic — highest

    static func < (a: Severity, b: Severity) -> Bool { a.rawValue < b.rawValue }

    /// Decode tolerant of unknown/newer values (clamps up to tamper).
    init(tolerant raw: Int) { self = Severity(rawValue: UInt8(clamping: raw)) ?? .tamper }

    var label: String {
        switch self {
        case .ok: return "All quiet"
        case .notice: return "Activity"
        case .warn: return "Needs a look"
        case .alert: return "Alert"
        case .tamper: return "Tamper"
        }
    }

    /// Semantic color role — resolved to an actual color by Theme, never here,
    /// so light/dark and accessibility stay in one place.
    var role: Theme.Role {
        switch self {
        case .ok: return .calm
        case .notice: return .info
        case .warn: return .warn
        case .alert: return .alert
        case .tamper: return .tamper
        }
    }

    var sfSymbol: String {
        switch self {
        case .ok: return "checkmark.seal.fill"
        case .notice: return "dot.radiowaves.left.and.right"
        case .warn: return "exclamationmark.triangle"
        case .alert: return "bell.badge.fill"
        case .tamper: return "hand.raised.slash.fill"
        }
    }
}

/// Ed25519 trust badge — mirrors `enum class Badge`.
enum TrustBadge: UInt8, Codable {
    case unknown = 0   // nothing seen yet
    case unsigned      // chain head carries no signature
    case signed        // signature present, no pinned key to check against
    case verified      // Ed25519 verify passed against the TOFU-pinned pubkey
    case failed        // signature present and did NOT verify — loud

    init(tolerant raw: Int) { self = TrustBadge(rawValue: UInt8(clamping: raw)) ?? .unknown }

    var label: String {
        switch self {
        case .unknown: return "Unverified"
        case .unsigned: return "Unsigned"
        case .signed: return "Signed"
        case .verified: return "Verified"
        case .failed: return "Signature failed"
        }
    }

    var sfSymbol: String {
        switch self {
        case .verified: return "checkmark.shield.fill"
        case .signed: return "shield.lefthalf.filled"
        case .unsigned: return "shield.slash"
        case .failed: return "xmark.shield.fill"
        case .unknown: return "shield"
        }
    }

    /// Only a real cryptographic pass earns the calm green check.
    var isTrusted: Bool { self == .verified }
}

/// Liveness — mirrors `enum class Link`.
enum Liveness: UInt8, Codable {
    case unknown = 0
    case online
    case stale     // silent past the stale deadline (amber)
    case lost      // silent past the lost deadline (red)
    case offline   // broker LWT said so

    init(tolerant raw: Int) { self = Liveness(rawValue: UInt8(clamping: raw)) ?? .unknown }

    var label: String {
        switch self {
        case .unknown: return "—"
        case .online: return "Online"
        case .stale: return "Quiet"
        case .lost: return "Lost"
        case .offline: return "Offline"
        }
    }

    /// A witness we can no longer hear is itself an alert condition — the
    /// dead-man's-switch (see docs/design/iphone_companion_app.md §5b).
    var isDark: Bool { self == .lost || self == .offline }
}

/// Canonical device type (`dt` in the mDNS TXT record).
enum DeviceType: String, Codable {
    case wap = "canary-wap"
    case vision = "canary-vision"
    case sense = "canary-sense"
    case display = "canary-display"
    case nightlight = "canary-nightlight"
    case unknown

    /// Decode whatever a device published into the coarse family it belongs
    /// to. Tolerant in three ways, each of which was a real gap:
    ///
    ///   1. **Spelling.** Canonicalized exactly the way `FleetFigure`
    ///      canonicalizes a device type (lowercase; underscores and spaces to
    ///      hyphens), so the `canary_wap` a config spells and the `canary-wap`
    ///      the wire carries are one type.
    ///   2. **The display line.** A display publishes what it IS —
    ///      `canary-dash`, `canary-nightstand`, `canary-nightstand7`,
    ///      `canary-watch` — none of which is a case here, and all of which
    ///      used to land on `.unknown`. That is what drew the generic bird and
    ///      the "Canary" role beside a Nightstand on the Fleet tab, and it also
    ///      switched OFF `servesGlassSettings`, hiding the screen controls on
    ///      the very devices that serve them.
    ///   3. **The future.** Anything else in the `canary-` vocabulary this
    ///      build has never heard of still decodes to `.unknown` rather than
    ///      being dropped — a newer fleet renders as an unnamed Canary, which
    ///      is honest, instead of vanishing.
    ///
    /// The coarse answer is deliberately all this returns. Which display it
    /// is — and therefore what it LOOKS like — is a finer question than the
    /// family, and it is answered from the published string and the hardware
    /// id, never from this enum (see FleetFigure.resolve).
    init(tolerant raw: String?) {
        let canonical = String((raw ?? "").lowercased().map { $0 == "_" || $0 == " " ? "-" : $0 })
        if let exact = DeviceType(rawValue: canonical) {
            self = exact
            return
        }
        self = DeviceType.displayLine.contains(canonical) ? .display : .unknown
    }

    /// The device types the display firmware publishes, all of which are one
    /// coarse `.display`. Listed rather than prefix-matched: a prefix rule
    /// would silently swallow a future `canary-dashcam` into the display line
    /// and offer it a screen it hasn't got.
    ///
    /// `canary-nightlight` is NOT here — it is its own case, because it has a
    /// lamp and therefore a whole section of controls no other display has.
    static let displayLine: Set<String> = [
        "canary-dash", "canary-dash7", "canary-nightstand",
        "canary-nightstand7", "canary-nightstand-touch", "canary-watch",
    ]

    var role: String {
        switch self {
        case .wap: return "Witness beacon"
        case .vision: return "Camera witness"
        case .sense: return "Radar witness"
        case .display: return "Fleet display"
        case .nightlight: return "Nightlight"
        case .unknown: return "Canary"
        }
    }

    var sfSymbol: String {
        switch self {
        case .wap: return "antenna.radiowaves.left.and.right"
        case .vision: return "eye"
        case .sense: return "waveform.badge.magnifyingglass"
        case .display: return "square.split.bottomrightquarter"
        case .nightlight: return "moon.stars.fill"
        case .unknown: return "bird"
        }
    }

    /// Only WAP-class devices run an HTTP server and can be paired over HTTP.
    var isHTTPPairable: Bool { self == .wap }
}
