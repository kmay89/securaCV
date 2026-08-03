// EventVocabulary.swift  (SHARED — one vocabulary of meaning on every surface)
//
// The Swift mirror of THE WITNESS DICTIONARY (spec/witness_dictionary.json) —
// the same vocabulary the kernel, the Home Assistant card, and the glass
// speak — plus the device-api dialect a Canary's /api/v1/witness serves
// today, plus a graceful fallback for every event type that doesn't exist
// yet. This is what makes "many sensors, many semantic events" render
// clearly forever: a new firmware capability shows up with an honest,
// readable line and a calm default severity, never as a blank row or a
// crash, and never as an app update held hostage.
//
// Anti-rot: scripts/lint_dictionary_sync.py parses THIS FILE and fails CI if
// `WitnessEvent` drifts from the dictionary (ids or labels) — edit the
// dictionary first, then here, like every other language's copy.
//
// Anti-anxiety: default severities are deliberately calm. Only tamper is
// tamper. Nothing an ordinary week produces ever escalates past `notice`,
// because an app that cries wolf is an app whose alerts get turned off —
// and then it protects no one (the §5b doctrine).

import Foundation

/// The kernel's semantic event vocabulary — one case per dictionary entry,
/// raw value = the canonical snake_case id. The Rust wire form is the
/// PascalCase variant name; `init(wire:)` accepts both.
enum WitnessEvent: String, CaseIterable, Codable, Sendable {
    case boundaryCrossingObjectLarge = "boundary_crossing_object_large"
    case boundaryCrossingObjectSmall = "boundary_crossing_object_small"
    case acousticImpulseInZone = "acoustic_impulse_in_zone"
    case presenceInRestrictedZone = "presence_in_restricted_zone"
    case vehiclePresenceAfterHours = "vehicle_presence_after_hours"
    case contactStateChange = "contact_state_change"
    case objectRemovedFromZone = "object_removed_from_zone"
    case tamperDetected = "tamper_detected"
    case vehicleArrivalDeparture = "vehicle_arrival_departure"

    /// Accepts the snake_case id or the Rust PascalCase wire variant (the
    /// dictionary's event variants are the mechanical PascalCase of their
    /// ids, which `EventVocabulary.snakeCase` inverts).
    init?(wire: String) {
        if let kind = WitnessEvent(rawValue: wire) {
            self = kind
        } else if let kind = WitnessEvent(rawValue: EventVocabulary.snakeCase(wire)) {
            self = kind
        } else {
            return nil
        }
    }

    /// The dictionary's canonical human label — identical wording to the
    /// Home Assistant card, on purpose: one sentence of meaning everywhere.
    var label: String {
        switch self {
        case .boundaryCrossingObjectLarge: return "Large object crossed boundary"
        case .boundaryCrossingObjectSmall: return "Small object crossed boundary"
        case .acousticImpulseInZone: return "Acoustic impulse in zone"
        case .presenceInRestrictedZone: return "Presence in restricted zone"
        case .vehiclePresenceAfterHours: return "Vehicle presence after hours"
        case .contactStateChange: return "Contact state change"
        case .objectRemovedFromZone: return "Object removed from zone"
        case .tamperDetected: return "Tamper detected"
        case .vehicleArrivalDeparture: return "Vehicle arrival/departure"
        }
    }

    /// SF Symbol per event — the app's native equivalent of the dictionary's
    /// mdi:* icons (SF Symbols and Material icons share no namespace, so the
    /// pairing is chosen here, once, for every Apple surface).
    var sfSymbol: String {
        switch self {
        case .boundaryCrossingObjectLarge: return "car.fill"
        case .boundaryCrossingObjectSmall: return "pawprint.fill"
        case .acousticImpulseInZone: return "waveform"
        case .presenceInRestrictedZone: return "person.crop.circle.badge.exclamationmark"
        case .vehiclePresenceAfterHours: return "car.side.fill"
        case .contactStateChange: return "door.left.hand.open"
        case .objectRemovedFromZone: return "shippingbox.fill"
        case .tamperDetected: return "hand.raised.slash.fill"
        case .vehicleArrivalDeparture: return "car.2.fill"
        }
    }

    /// Calm default severity. Context (Phase 8 on the device, rules on the
    /// phone) can escalate; the vocabulary itself never panics.
    var defaultSeverity: Severity {
        switch self {
        case .tamperDetected: return .tamper
        case .presenceInRestrictedZone, .vehiclePresenceAfterHours: return .warn
        default: return .notice
        }
    }
}

/// The dialect a Canary's own /api/v1/witness serves today (see
/// canary-vision/docs/api.md and const.py's coarse mapping). Not dictionary-
/// governed; kept as an explicit list so its meanings are chosen, not guessed.
enum DeviceEvent: String, CaseIterable, Codable, Sendable {
    case personDetected = "person_detected"
    case vehicleDetected = "vehicle_detected"
    case motionDetected = "motion_detected"
    case animalDetected = "animal_detected"
    case tamper = "tamper"
    case panic = "panic"
    case chainBreak = "chain_break"
    case verifyFailed = "verify_failed"
    case witnessLost = "witness_lost"

    var sfSymbol: String {
        switch self {
        case .personDetected: return "figure.stand"
        case .vehicleDetected: return "car.fill"
        case .motionDetected: return "dot.radiowaves.left.and.right"
        case .animalDetected: return "pawprint.fill"
        case .tamper, .panic: return "hand.raised.slash.fill"
        case .chainBreak, .verifyFailed: return "xmark.shield.fill"
        case .witnessLost: return "moon.zzz.fill"
        }
    }

    /// Mirrors the coarse mapping the app has always used (const.py parity):
    /// tamper is tamper, integrity failures alert, everything else notices.
    var defaultSeverity: Severity {
        switch self {
        case .tamper, .panic: return .tamper
        case .chainBreak, .verifyFailed, .witnessLost: return .alert
        default: return .notice
        }
    }

    /// Friendly phrasing for the Today timeline ("Someone at Front Porch").
    func headline(zone: String, deviceName: String) -> String {
        let place = zone.isEmpty ? deviceName : zone
        switch self {
        case .personDetected: return "Someone at \(place)"
        case .vehicleDetected: return "Vehicle at \(place)"
        case .motionDetected: return "Motion at \(place)"
        case .animalDetected: return "Animal at \(place)"
        case .tamper, .panic: return "Tamper at \(deviceName)"
        case .chainBreak, .verifyFailed: return "Chain check failed at \(deviceName)"
        case .witnessLost: return "\(deviceName) went quiet"
        }
    }
}

/// The one resolver every surface calls. Understands the dictionary
/// vocabulary, the device dialect, and — crucially — everything else:
/// an event type this build has never heard of still renders as readable
/// words with a calm severity and a generic glyph. New sensors light up;
/// they never break.
enum EventVocabulary {
    static func severity(forWire wire: String) -> Severity {
        if let known = WitnessEvent(wire: wire) { return known.defaultSeverity }
        if let device = DeviceEvent(rawValue: wire) { return device.defaultSeverity }
        return .notice   // unknown = worth a line, never worth a siren
    }

    static func sfSymbol(forWire wire: String) -> String {
        if let known = WitnessEvent(wire: wire) { return known.sfSymbol }
        if let device = DeviceEvent(rawValue: wire) { return device.sfSymbol }
        return "sparkle"
    }

    static func headline(forWire wire: String, zone: String, deviceName: String) -> String {
        if let device = DeviceEvent(rawValue: wire) {
            return device.headline(zone: zone, deviceName: deviceName)
        }
        let base: String
        if let known = WitnessEvent(wire: wire) {
            base = known.label
        } else {
            base = prettified(wire)
        }
        return zone.isEmpty ? base : "\(base) · \(zone)"
    }

    /// "solar_flare_detected" → "Solar flare detected" — the honest fallback
    /// for vocabulary from a newer fleet than this app.
    static func prettified(_ wire: String) -> String {
        let words = snakeCase(wire).split(separator: "_").joined(separator: " ")
        guard let first = words.first else { return wire }
        return first.uppercased() + words.dropFirst()
    }

    /// "BoundaryCrossingObjectLarge" → "boundary_crossing_object_large".
    /// Already-snake input passes through unchanged.
    static func snakeCase(_ s: String) -> String {
        var out = ""
        for (i, ch) in s.enumerated() {
            if ch.isUppercase {
                if i > 0 { out.append("_") }
                out.append(ch.lowercased())
            } else {
                out.append(ch)
            }
        }
        return out
    }
}
