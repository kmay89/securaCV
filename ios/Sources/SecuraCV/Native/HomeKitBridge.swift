// HomeKitBridge.swift
//
// Surface each Canary's coarse state into Apple Home so a witness can drive a
// HomeKit automation ("if the porch Canary reports tamper, turn on every
// light") and appear in the Home app alongside everything else. We map ONLY the
// invariant-safe, coarse signals — occupancy/motion/tamper/liveness — never
// video, never identity. HomeKit is read-out, not a new data path.
//
// Note: a pure software accessory is exposed via HAP; on-device this uses
// HomeKit's HMAccessory/HMCharacteristic. Kept behind canImport so the test
// target never links HomeKit.

import Foundation
#if canImport(HomeKit)
import HomeKit
#endif

@MainActor
final class HomeKitBridge: ObservableObject {
    @Published private(set) var authorized = false
    @Published var isEnabled = false        // opt-in; off by default

    #if canImport(HomeKit)
    private let manager = HMHomeManager()
    #endif

    /// Which HomeKit characteristic a witness signal maps to.
    enum Signal {
        case motion(Bool)          // HMCharacteristicTypeMotionDetected
        case occupancy(Bool)       // HMCharacteristicTypeOccupancyDetected
        case tamper(Bool)          // HMCharacteristicTypeStatusTampered
        case active(Bool)          // HMCharacteristicTypeStatusActive (liveness)
    }

    /// Translate a Witness into the coarse HomeKit signals it should publish.
    static func signals(for w: Witness) -> [Signal] {
        var out: [Signal] = [.active(!w.link.isDark), .tamper(w.tamper)]
        if let present = w.radarPresent { out.append(.occupancy(present)) }
        if w.lastEventSeverity >= .notice, w.deviceType == .vision {
            out.append(.motion(true))
        }
        return out
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
