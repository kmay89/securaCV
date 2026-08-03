// HomeKitBridge.swift
//
// Surface each Canary's coarse state into Apple Home so a witness can drive a
// HomeKit automation ("if the porch Canary reports tamper, turn on every
// light") and appear in the Home app alongside everything else. We map ONLY
// the invariant-safe, coarse signals — motion/occupancy/contact/tamper/
// liveness — never video, never identity. HomeKit is read-out, not a new data
// path.
//
// WHAT THIS TYPE IS, AND IS NOT. The HomeKit *framework* can read and control
// accessories that already exist in a home; no iOS API lets an app publish
// itself as one. Publishing a Canary is a bridge's job, and the bridge runs
// where the fleet lives — the hub (Home Assistant's HomeKit bridge, or the
// kernel's own `bridge-homekit` lane) or, eventually, the Canary itself. So
// this type is deliberately the *shepherd*: it guides pairing, reads back
// what Apple Home actually sees, and says so plainly when the two worlds
// disagree. It never pretends to be the accessory.
//
// Design of record: docs/design/apple_home_integration.md.
// Vocabulary: spec/witness_dictionary.json → homekit_projection
// (scripts/lint_dictionary_sync.py fails CI if this file drifts from it).

import Foundation
#if canImport(HomeKit)
import HomeKit
#endif

/// The closed vocabulary of coarse booleans a Canary may project into Apple
/// Home. Mirrors the Rust `HomeSignal` in `src/bridge/homekit.rs`; raw values
/// are the witness dictionary's ids.
///
/// There is no zone, timestamp, count, or identity case here — by
/// construction, not by policy. Adding one starts in the dictionary.
enum HomeSignal: String, CaseIterable {
    case motion = "motion"
    case occupancy = "occupancy"
    case contact = "contact"
    case tamper = "tamper"
    case active = "active"
    case lowBattery = "low_battery"
    case motionPerson = "motion_person"
    case motionVehicle = "motion_vehicle"
    case motionAnimal = "motion_animal"
    case motionPackage = "motion_package"

    /// The HAP characteristic this signal projects as.
    var hapCharacteristic: String {
        switch self {
        case .motion: return "motion-detected"
        case .occupancy: return "occupancy-detected"
        case .contact: return "contact-sensor-state"
        case .tamper: return "status-tampered"
        case .active: return "status-active"
        case .lowBattery: return "status-lo-batt"
        case .motionPerson, .motionVehicle, .motionAnimal, .motionPackage:
            return "motion-detected"
        }
    }

    /// Whether this signal carries the coarse object-class word — the one
    /// sanctioned step past the dumb-PIR bar, and therefore off by default.
    var isClassScoped: Bool {
        switch self {
        case .motionPerson, .motionVehicle, .motionAnimal, .motionPackage:
            return true
        default:
            return false
        }
    }

    /// Plain-language name, for the consent screen.
    var label: String {
        switch self {
        case .motion: return "Motion"
        case .occupancy: return "Occupancy"
        case .contact: return "Contact"
        case .tamper: return "Tamper"
        case .active: return "Responding"
        case .lowBattery: return "Low battery"
        case .motionPerson: return "Motion (person)"
        case .motionVehicle: return "Motion (vehicle)"
        case .motionAnimal: return "Motion (animal)"
        case .motionPackage: return "Motion (package)"
        }
    }

    /// What a projection publishes unless a human says otherwise.
    static var defaultEnabled: Set<HomeSignal> {
        Set(allCases.filter { !$0.isClassScoped })
    }
}

/// What the app can honestly tell the user about a Canary's standing in
/// Apple Home. Every case is something we can actually observe — there is no
/// "verified" here, because confirming an accessory is present is not
/// checking an Ed25519 signature against a pinned key.
enum HomeKitStanding: Equatable {
    /// The user has not turned the integration on.
    case off
    /// Turned on, but iOS has not granted HomeKit access yet.
    case needsAuthorization
    /// Authorized, and this Canary is not in the home yet.
    case notPaired
    /// Present in the home and answering.
    case paired
    /// Present in the home, but the home has no hub — so automations will
    /// not run. Apple's rule, and worth saying out loud exactly once.
    case pairedWithoutHomeHub
    /// Apple Home still lists it, but the fleet says this Canary is dark.
    /// The two worlds disagree and the user should know which to believe.
    case staleInHome

    /// The one-line Doctor card, or nil when there is nothing to say.
    var doctorNote: String? {
        switch self {
        case .off, .paired:
            return nil
        case .needsAuthorization:
            return "Allow Home access so your Canaries can trigger automations."
        case .notPaired:
            return "Not in Apple Home yet — add it to use it in automations."
        case .pairedWithoutHomeHub:
            return "Apple Home needs a home hub (HomePod or Apple TV) to run automations while you're away."
        case .staleInHome:
            return "Apple Home still shows this Canary, but it stopped answering."
        }
    }
}

@MainActor
final class HomeKitBridge: ObservableObject {
    @Published private(set) var authorized = false
    @Published var isEnabled = false        // opt-in; off by default

    /// Per-signal consent. Class-scoped signals start off — the dumb-PIR bar.
    @Published private(set) var enabledSignals: Set<HomeSignal> = HomeSignal.defaultEnabled

    #if canImport(HomeKit)
    private let manager = HMHomeManager()
    #endif

    /// Turn a signal's projection on or off.
    ///
    /// Tamper cannot be turned off: a witness that reports its own tampering
    /// must not be able to do so invisibly to a home the owner already chose
    /// to publish into. Mirrors the same refusal in the Rust projection.
    @discardableResult
    func setSignal(_ signal: HomeSignal, enabled: Bool) -> Bool {
        if !enabled && signal == .tamper { return false }
        if enabled {
            enabledSignals.insert(signal)
        } else {
            enabledSignals.remove(signal)
        }
        return true
    }

    /// Which coarse signals this witness would publish, given consent.
    ///
    /// Pure and host-testable: it reads a `Witness` and returns vocabulary,
    /// touching no HomeKit API, so the mapping can be tested without a device.
    func signals(for w: Witness) -> Set<HomeSignal> {
        var out: Set<HomeSignal> = []
        if !w.link.isDark { out.insert(.active) }
        if w.tamper { out.insert(.tamper) }
        if let present = w.radarPresent, present { out.insert(.occupancy) }
        if w.lastEventSeverity >= .notice, w.deviceType == .vision {
            out.insert(.motion)
        }
        return out.intersection(enabledSignals.union([.tamper]))
    }

    /// What we can honestly say about this witness's standing in Apple Home.
    func standing(for w: Witness, presentInHome: Bool, homeHubPresent: Bool) -> HomeKitStanding {
        guard isEnabled else { return .off }
        guard authorized else { return .needsAuthorization }
        guard presentInHome else { return .notPaired }
        if w.link.isDark { return .staleInHome }
        return homeHubPresent ? .paired : .pairedWithoutHomeHub
    }

    func requestAccess() {
        #if canImport(HomeKit)
        // HMHomeManager triggers the authorization prompt on first use; state
        // arrives via its delegate in the full implementation.
        _ = manager.homes
        authorized = true
        #endif
    }
}
