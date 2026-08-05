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
    /// One shepherd for the app, the `AwayPush.shared` precedent. Constructing
    /// it touches no HomeKit API — the `HMHomeManager` is created lazily in
    /// `requestAccess()`, so launch, tests, and previews never wake HomeKit
    /// (the CloudKit lesson from RELEASE_LESSONS (ab), applied here first).
    static let shared = HomeKitBridge()
    init() {}

    @Published private(set) var authorized = false
    @Published var isEnabled = false        // opt-in; off by default

    /// Per-signal consent. Class-scoped signals start off — the dumb-PIR bar.
    @Published private(set) var enabledSignals: Set<HomeSignal> = HomeSignal.defaultEnabled

    /// Whether any home this account can see has a connected home hub.
    /// Read from `HMHomeManager` after authorization; false until known —
    /// the ladder treats "don't know" as "can't promise automations".
    @Published private(set) var homeHubPresent = false

    /// Lowercased names of accessories visible in the user's homes, so the
    /// per-witness standing can say "seen in Apple Home" without pretending
    /// to more: a name match is an observation, not a verification.
    @Published private(set) var accessoryNames: Set<String> = []

    #if canImport(HomeKit)
    private var manager: HMHomeManager?
    private var delegateShim: HomeManagerShim?
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
        Self.standing(isEnabled: isEnabled, authorized: authorized,
                      isDark: w.link.isDark, presentInHome: presentInHome,
                      homeHubPresent: homeHubPresent)
    }

    /// The ladder itself, pure so the whole thing is host-testable without an
    /// `HMHomeManager` — the `IslandPolicy`/`FocusGate` factoring.
    static func standing(isEnabled: Bool, authorized: Bool, isDark: Bool,
                         presentInHome: Bool, homeHubPresent: Bool) -> HomeKitStanding {
        guard isEnabled else { return .off }
        guard authorized else { return .needsAuthorization }
        guard presentInHome else { return .notPaired }
        if isDark { return .staleInHome }
        return homeHubPresent ? .paired : .pairedWithoutHomeHub
    }

    /// Was an accessory with this witness's name observed in a home?
    /// Case-insensitive, observation-grade — the standing copy says "seen",
    /// never "verified".
    func seenInHome(_ w: Witness) -> Bool {
        let needle = (w.name.isEmpty ? w.id : w.name).lowercased()
        guard !needle.isEmpty else { return false }
        return accessoryNames.contains { $0.contains(needle) }
    }

    /// Ask iOS for Home access. The OS shows its consent prompt on the first
    /// real HomeKit touch; state lands via the delegate — `authorized` is
    /// only ever set from what the framework reported, never assumed.
    func requestAccess() {
        #if canImport(HomeKit)
        if manager == nil {
            let shim = HomeManagerShim(bridge: self)
            let m = HMHomeManager()
            m.delegate = shim
            delegateShim = shim
            manager = m
        }
        if let m = manager {
            noteAuthorization(m.authorizationStatus)
            noteHomes(m.homes)
        }
        #endif
    }

    #if canImport(HomeKit)
    fileprivate func noteAuthorization(_ status: HMHomeManagerAuthorizationStatus) {
        authorized = status.contains(.authorized)
    }

    fileprivate func noteHomes(_ homes: [HMHome]) {
        homeHubPresent = homes.contains { $0.homeHubState == .connected }
        accessoryNames = Set(homes.flatMap { home in
            home.accessories.map { $0.name.lowercased() }
        })
    }
    #endif
}

#if canImport(HomeKit)
/// `HMHomeManagerDelegate` requires an `NSObject`; the bridge stays a plain
/// `ObservableObject`, so this shim forwards the two callbacks that matter.
private final class HomeManagerShim: NSObject, HMHomeManagerDelegate {
    weak var bridge: HomeKitBridge?
    init(bridge: HomeKitBridge) { self.bridge = bridge }

    func homeManager(_ manager: HMHomeManager,
                     didUpdate status: HMHomeManagerAuthorizationStatus) {
        Task { @MainActor [weak bridge] in bridge?.noteAuthorization(status) }
    }

    func homeManagerDidUpdateHomes(_ manager: HMHomeManager) {
        let homes = manager.homes
        Task { @MainActor [weak bridge] in bridge?.noteHomes(homes) }
    }
}
#endif
