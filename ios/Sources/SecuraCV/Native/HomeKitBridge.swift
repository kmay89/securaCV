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

    /// Lowercased accessory serial numbers — the never-rot identity anchor.
    /// The hub's HAP bridge sets each Canary accessory's serial to its
    /// pseudonymous device id, so this set survives every rename on either
    /// side. Populated from cached characteristic values only.
    @Published private(set) var accessorySerials: Set<String> = []

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
        // The battery the model already holds, projected at the SAME
        // threshold the app's own severity ladder warns at — one number,
        // one meaning, everywhere (Witness.lowBatteryThreshold). The house
        // learns "this sensor is running low", which is exactly what a dumb
        // HomeKit sensor would say about itself.
        if let b = w.batteryPct, b >= 0, b < Witness.lowBatteryThreshold {
            out.insert(.lowBattery)
        }
        // The class-scoped motion signals, live now that a transport can
        // carry a class for a PAIRED Canary: the fleet-selfreport slice
        // folds `seeing` only from an address-attributed poll of the
        // device's own URL (FleetMerge's rule — a beacon's class still
        // never leaves the beacon's own provisional row), and the claim
        // ages out through seeingNow's freshness window, so a stale claim
        // derives nothing. Derived only while plain .motion is derivable —
        // a class is a refinement of motion, never a second opinion — and
        // the consent model is unchanged: each class signal projects only
        // if the owner enabled it, exactly like every signal above.
        if out.contains(.motion), let seen = w.seeingNow() {
            switch seen.kind {
            case .person: out.insert(.motionPerson)
            case .vehicle: out.insert(.motionVehicle)
            case .animal: out.insert(.motionAnimal)
            case .package: out.insert(.motionPackage)
            }
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
        // Identity first: the accessory serial IS the device id, set by the
        // bridge at pairing. This match survives any rename in either app —
        // no sync button, because nothing depends on names.
        if !w.id.isEmpty && accessorySerials.contains(w.id.lowercased()) {
            return true
        }
        // Name fallback, observation-grade, for accessories bridged by
        // paths that don't set the serial (the HA HomeKit Bridge lane).
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
            shim.manager = m
            delegateShim = shim
            manager = m
        }
        if let m = manager {
            noteAuthorization(m.authorizationStatus)
            noteHomes(m.homes)
        }
        #endif
    }

    // ── The concierge's impure half (§4.2: the app authors, never runs) ──
    //
    // Every method below runs only downstream of an explicit user tap with
    // isEnabled && authorized — nothing here is reachable from init, body,
    // or a preview, per the lazy-manager rule at the top of this class.

    /// User-defined scenes in the primary home, as plain values.
    /// (Apple's builtin arrival/departure action sets are excluded — the
    /// concierge runs the household's own scenes, it doesn't invent or
    /// borrow them.)
    func userScenes() -> [(id: UUID, name: String)] {
        #if canImport(HomeKit)
        guard let home = manager?.primaryHome else { return [] }
        return home.actionSets
            .filter { $0.actionSetType == HMActionSetTypeUserDefined }
            .map { ($0.uniqueIdentifier, $0.name) }
        #else
        return []
        #endif
    }

    /// Accessories in the primary home carrying a characteristic this
    /// signal projects to — the concrete things an automation can trigger
    /// on. Names only; the HomeKit objects never leave the bridge.
    func automationSources(for signal: HomeSignal) -> [String] {
        #if canImport(HomeKit)
        guard let home = manager?.primaryHome else { return [] }
        let wanted = signal.hmCharacteristicTypeID
        return home.accessories.filter { accessory in
            accessory.services.contains { service in
                service.characteristics.contains { $0.characteristicType == wanted }
            }
        }
        .map(\.name)
        #else
        return []
        #endif
    }

    /// Is the current user an administrator of the primary home? Writing a
    /// trigger needs it; the readiness ladder says so instead of failing.
    func isAdministrator() -> Bool {
        #if canImport(HomeKit)
        guard let home = manager?.primaryHome else { return false }
        return home.homeAccessControl(for: home.currentUser).isAdministrator
        #else
        return false
        #endif
    }

    /// Write the planned automation into the primary home: a real
    /// HMEventTrigger on our accessory's characteristic, running the scene
    /// the household already authored. Runs on the home hub from then on,
    /// app closed — the app is the author, never the runtime.
    func author(_ plan: PlannedAutomation) async throws {
        #if canImport(HomeKit)
        guard let home = manager?.primaryHome else {
            throw HomeAuthorError.noHome
        }
        guard let accessory = home.accessories.first(where: { $0.name == plan.accessoryName }),
              let characteristic = accessory.services
                  .flatMap(\.characteristics)
                  .first(where: { $0.characteristicType == plan.signal.hmCharacteristicTypeID })
        else {
            throw HomeAuthorError.accessoryGone
        }
        guard let scene = home.actionSets.first(where: { $0.uniqueIdentifier == plan.sceneID })
        else {
            throw HomeAuthorError.sceneGone
        }
        let event = HMCharacteristicEvent(
            characteristic: characteristic, triggerValue: NSNumber(value: true))
        let trigger = HMEventTrigger(name: plan.triggerName, events: [event], predicate: nil)
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            home.addTrigger(trigger) { error in
                if let error { c.resume(throwing: error) } else { c.resume() }
            }
        }
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            trigger.addActionSet(scene) { error in
                if let error { c.resume(throwing: error) } else { c.resume() }
            }
        }
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            trigger.enable(true) { error in
                if let error { c.resume(throwing: error) } else { c.resume() }
            }
        }
        // Anchor by identity, not name: the stored UUID keeps this
        // automation recognizable (and removable) after any rename.
        Self.rememberAuthored(trigger.uniqueIdentifier)
        #else
        _ = plan
        throw HomeAuthorError.noHome
        #endif
    }

    /// Automations this app authored (recognized by the trigger-name
    /// prefix), for the review-and-remove list.
    /// Durable anchors for automations this app authored. UUIDs, not names:
    /// a rename in the Home app must not orphan the review-and-remove list.
    /// The name prefix stays as a fallback for automations authored before
    /// anchoring existed.
    static let authoredIDsKey = "authored_automation_ids_v1"

    static func rememberAuthored(_ id: UUID, defaults: UserDefaults = .standard) {
        var ids = Set(defaults.stringArray(forKey: authoredIDsKey) ?? [])
        ids.insert(id.uuidString)
        defaults.set(ids.sorted(), forKey: authoredIDsKey)
    }

    static func forgetAuthored(_ id: UUID, defaults: UserDefaults = .standard) {
        var ids = Set(defaults.stringArray(forKey: authoredIDsKey) ?? [])
        ids.remove(id.uuidString)
        defaults.set(ids.sorted(), forKey: authoredIDsKey)
    }

    /// Pure recognition rule, host-tested: ours if anchored by UUID, or if
    /// it still carries the name prefix (pre-anchor authorship).
    static func isAuthored(name: String, id: UUID, anchors: Set<String>) -> Bool {
        anchors.contains(id.uuidString) || name.hasPrefix("SecuraCV: ")
    }

    func authoredAutomations() -> [(id: UUID, name: String)] {
        #if canImport(HomeKit)
        guard let home = manager?.primaryHome else { return [] }
        let anchors = Set(UserDefaults.standard.stringArray(forKey: Self.authoredIDsKey) ?? [])
        return home.triggers
            .filter { Self.isAuthored(name: $0.name, id: $0.uniqueIdentifier, anchors: anchors) }
            .map { ($0.uniqueIdentifier, $0.name) }
        #else
        return []
        #endif
    }

    /// Remove one authored automation. Only ours are offered for removal;
    /// the household's own automations are never touched.
    func removeAutomation(id: UUID) async throws {
        #if canImport(HomeKit)
        let anchors = Set(UserDefaults.standard.stringArray(forKey: Self.authoredIDsKey) ?? [])
        guard let home = manager?.primaryHome,
              let trigger = home.triggers.first(where: { $0.uniqueIdentifier == id }),
              Self.isAuthored(name: trigger.name, id: id, anchors: anchors)
        else {
            return
        }
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            home.removeTrigger(trigger) { error in
                if let error { c.resume(throwing: error) } else { c.resume() }
            }
        }
        #else
        _ = id
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
        // The never-rot anchor: the bridge sets each accessory's serial
        // number to the Canary's pseudonymous device id, so identity
        // survives any rename on either side. Cached values only — reading
        // a characteristic's cache touches no network; a nil cache just
        // means no anchor yet, and the name fallback covers it.
        var serials: Set<String> = []
        for home in homes {
            home.delegate = delegateShim
            for accessory in home.accessories {
                accessory.delegate = delegateShim
                for service in accessory.services
                where service.serviceType == HMServiceTypeAccessoryInformation {
                    for characteristic in service.characteristics
                    where characteristic.characteristicType == HMCharacteristicTypeSerialNumber {
                        if let serial = characteristic.value as? String, !serial.isEmpty {
                            serials.insert(serial.lowercased())
                        }
                    }
                }
            }
        }
        accessorySerials = serials
    }
    #endif
}

#if canImport(HomeKit)
/// The HomeKit delegate protocols require an `NSObject`; the bridge stays a
/// plain `ObservableObject`, so this shim forwards every callback that can
/// change what the two worlds know about each other. Live readback IS the
/// sync: an accessory added, removed, or renamed in the Home app lands here
/// seconds later — never at the next "import".
private final class HomeManagerShim: NSObject, HMHomeManagerDelegate,
    HMHomeDelegate, HMAccessoryDelegate
{
    weak var bridge: HomeKitBridge?
    weak var manager: HMHomeManager?
    init(bridge: HomeKitBridge) { self.bridge = bridge }

    private func renote() {
        guard let homes = manager?.homes else { return }
        Task { @MainActor [weak bridge] in bridge?.noteHomes(homes) }
    }

    func homeManager(_ manager: HMHomeManager,
                     didUpdate status: HMHomeManagerAuthorizationStatus) {
        Task { @MainActor [weak bridge] in bridge?.noteAuthorization(status) }
    }

    func homeManagerDidUpdateHomes(_ manager: HMHomeManager) {
        renote()
    }

    // Home-level changes never reach homeManagerDidUpdateHomes — these do.
    func home(_ home: HMHome, didAdd accessory: HMAccessory) {
        renote()
    }

    func home(_ home: HMHome, didRemove accessory: HMAccessory) {
        renote()
    }

    // A rename in the Home app: the name set refreshes, and the serial
    // anchor is untouched — which is the whole point.
    func accessoryDidUpdateName(_ accessory: HMAccessory) {
        renote()
    }
}
#endif
