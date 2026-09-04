// The automation concierge's pure half — phase A2's last open piece
// (docs/design/apple_home_integration.md §4.2).
//
// "Tell the house": pick a witness signal, pick a scene the household
// already authored in the Home app, and the app writes a real HomeKit
// automation that runs on the home hub forever after, app closed. The app
// is the author, never the runtime — and this file is the part of the
// author that needs no HomeKit at all: the plan value, the sentence it
// reads back to the owner, the readiness ladder, and the signal-to-
// characteristic-type map. All host-tested; the impure writer lives in
// HomeKitBridge.
//
// Deliberately out of scope, per the RFC's open decisions: creating or
// editing scenes (the Home app owns that), any inbound write to a Canary
// from HomeKit (decision #4, unsettled), and postures (decision #3).

import Foundation
#if canImport(HomeKit)
import HomeKit
#endif

/// One planned "when X, run Y" automation, as a plain value — previews and
/// tests build these freely because there is no HomeKit object inside.
struct PlannedAutomation: Equatable, Sendable {
    let homeID: UUID
    let homeName: String
    let accessoryName: String
    let signal: HomeSignal
    let sceneID: UUID
    let sceneName: String

    /// The confirm-screen sentence. Read back before writing, so the owner
    /// approves exactly what the house will do.
    var sentence: String {
        "When \(accessoryName) reports \(signal.label.lowercased()), run \(sceneName)."
    }

    /// The name the trigger carries in the Home app, so authored
    /// automations are recognizable (and removable) later.
    var triggerName: String {
        "SecuraCV: \(accessoryName) \(signal.label) → \(sceneName)"
    }
}

/// Why the concierge can or cannot offer itself right now. Mirrors the
/// HomeKitStanding ladder: calm states say nothing, problem states owe one
/// plain sentence. The RFC's §6 rule — "the concierge appears with the
/// accessories, not before" — is the `noAccessories` rung.
enum ConciergeReadiness: Equatable {
    case integrationOff
    case needsAuthorization
    case notAdministrator
    case noAccessories
    case noScenes
    case ready
    case readyWithoutHomeHub

    var note: String? {
        switch self {
        case .integrationOff, .ready:
            return nil
        case .needsAuthorization:
            return "Allow Home access first."
        case .notAdministrator:
            return "Only a home administrator can add automations — ask the person who owns this home in Apple Home."
        case .noAccessories:
            return "No Canary is visible in Apple Home yet — pair the bridge first."
        case .noScenes:
            return "Make a scene in the Home app first — the concierge runs your scenes, it doesn't invent them."
        case .readyWithoutHomeHub:
            return "Without a home hub (HomePod or Apple TV), this automation will not run while you're away."
        }
    }

    /// The ladder, pure and total — the IslandPolicy/standing factoring.
    static func evaluate(
        isEnabled: Bool, authorized: Bool, isAdministrator: Bool,
        accessoryCount: Int, sceneCount: Int, homeHubPresent: Bool
    ) -> ConciergeReadiness {
        guard isEnabled else { return .integrationOff }
        guard authorized else { return .needsAuthorization }
        guard isAdministrator else { return .notAdministrator }
        guard accessoryCount > 0 else { return .noAccessories }
        guard sceneCount > 0 else { return .noScenes }
        return homeHubPresent ? .ready : .readyWithoutHomeHub
    }
}

/// What can go wrong between confirming a plan and the home accepting it.
/// Plain, ownable failures — the sheet renders `line` and offers retry.
enum HomeAuthorError: Error {
    case noHome
    case accessoryGone
    case sceneGone
    /// The accessory is there, but no service of it carries this signal —
    /// for a class-scoped signal, the bridge hasn't published the named
    /// class service (usually: that consent is off). Refusing is the honest
    /// move; binding plain motion instead would fire on everything.
    case signalNotPublished(HomeSignal)

    var line: String {
        switch self {
        case .noHome: return "No Apple Home is set up on this iPhone."
        case .accessoryGone: return "That Canary is no longer visible in Apple Home."
        case .sceneGone: return "That scene no longer exists in the Home app."
        case .signalNotPublished(let signal):
            return "\(signal.label) isn't published for that Canary in Apple Home — enable the signal and let the bridge publish it first."
        }
    }
}

extension HomeSignal {
    /// The HomeKit framework characteristic-type identifier this signal's
    /// projection surfaces as — the concrete thing an HMCharacteristicEvent
    /// triggers on. Distinct from `hapCharacteristic` (the dictionary's HAP
    /// short names, linter-parsed in HomeKitBridge.swift); these are the
    /// `HMCharacteristicType*` UUID strings, kept here so the linter's
    /// parsed regions stay untouched. Class-scoped signals collapse onto
    /// motion, exactly as `hapCharacteristic` does — at binding time the
    /// parent service's NAME is what tells the classes apart (see
    /// `automationBindingIndex`).
    var hmCharacteristicTypeID: String {
        #if canImport(HomeKit)
        // The framework's own constants — never UUID literals typed from
        // memory; a wrong one would silently trigger on the wrong sensor.
        switch self {
        case .motion, .motionPerson, .motionVehicle, .motionAnimal, .motionPackage:
            return HMCharacteristicTypeMotionDetected
        case .occupancy:
            return HMCharacteristicTypeOccupancyDetected
        case .contact:
            return HMCharacteristicTypeContactState
        case .tamper:
            return HMCharacteristicTypeStatusTampered
        case .active:
            return HMCharacteristicTypeStatusActive
        case .lowBattery:
            return HMCharacteristicTypeStatusLowBattery
        }
        #else
        return rawValue
        #endif
    }
}

// ── Binding a signal to the right service, not merely the right type ──
//
// HAP has one motion-detected characteristic type, so on a bridged Canary
// "Person" and plain "Motion" are the SAME characteristic type on different
// services — only each service's name says which is which. Binding by type
// alone is how a "Motion (person)" automation ends up firing on any motion.
// Everything here is plain values, so the selection is host-tested with no
// HMAccessory anywhere; HomeKitBridge flattens the real accessory into
// these pairs and indexes the answer back.

/// One (service name, characteristic type) pair from an accessory, in the
/// order the accessory lists them — the shape the selector reads.
struct ServiceCharacteristic: Equatable, Sendable {
    let serviceName: String
    let characteristicType: String
}

extension HomeSignal {
    /// The name the hub's HAP bridge gives this signal's service — the word
    /// under the tile in the Home app. Mirrored, in this one place, from
    /// `service_name()` in `src/bridge/hap/accessory.rs`, where those
    /// strings are minted.
    var bridgeServiceName: String {
        switch self {
        case .motion: return "Motion"
        case .occupancy: return "Occupancy"
        case .contact: return "Contact"
        case .tamper: return "Tamper"
        case .active: return "Active"
        case .lowBattery: return "Battery"
        case .motionPerson: return "Person"
        case .motionVehicle: return "Vehicle"
        case .motionAnimal: return "Animal"
        case .motionPackage: return "Package"
        }
    }

    /// The service names that mean "this tile is class-scoped" — derived
    /// from the vocabulary above, never listed twice.
    static var classServiceNames: Set<String> {
        Set(allCases.filter(\.isClassScoped).map(\.bridgeServiceName))
    }

    /// Which of an accessory's characteristics this signal's automation may
    /// bind to: an index into `pairs`, or nil when no honest binding exists.
    ///
    /// A class-scoped signal binds ONLY its named service — a missing
    /// Person service is a refusal, never a quiet fallback to plain motion.
    /// A plain signal prefers a service NOT named for a class, so plain
    /// motion doesn't ride the Person tile when both exist; when only class
    /// services carry the type, first-of-type stands (the pre-class
    /// behavior).
    func automationBindingIndex(in pairs: [ServiceCharacteristic]) -> Int? {
        let typed = pairs.indices.filter {
            pairs[$0].characteristicType == hmCharacteristicTypeID
        }
        if isClassScoped {
            return typed.first { pairs[$0].serviceName == bridgeServiceName }
        }
        return typed.first { !Self.classServiceNames.contains(pairs[$0].serviceName) }
            ?? typed.first
    }
}
