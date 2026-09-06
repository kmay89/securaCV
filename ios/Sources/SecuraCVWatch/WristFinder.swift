// WristFinder.swift  (watch app target)
//
// The wrist's own ears for "Find this Canary": a foreground CoreBluetooth
// scan that hears the fleet's presence beacons directly, so finding works
// with the iPhone on its charger — the antenna doing the pointing is the one
// strapped to the arm doing the walking. Decoding is the same pure
// Shared/FleetBeacon parser the phone uses; the search arithmetic is the
// same host-tested Shared/ProximityRanger; this file is only the radio glue.
//
// TWO WATCH-SPECIFIC MECHANICS, both about honesty:
//
//   * THE SCAN PULSE. CoreBluetooth may coalesce duplicate adverts however
//     it likes, and a ranger fed one sample per device would go stale-blind.
//     Restarting the scan every few seconds resets that dedup, so fresh RSSI
//     keeps arriving at a cadence the ranger's smoothing expects — and a
//     Canary that actually went quiet still reads "Listening…" within the
//     ranger's six-second window, because a restart cannot resurrect a
//     beacon that isn't on the air.
//
//   * FOREGROUND ONLY, USER-STARTED ONLY. The scan runs while the Find
//     screen is on the wrist and stops the moment it leaves — the same
//     shape as the phone's radios, and the battery posture a watch demands.
//     The consent gate is the PHONE's discovery choice, carried in the
//     snapshot; this file never scans for a user who said no there.

import Foundation
import Combine

#if canImport(CoreBluetooth)
import CoreBluetooth

@MainActor
final class WristFinder: NSObject, ObservableObject {
    /// The freshest reading for the target — what the Find screen renders.
    @Published private(set) var reading = ProximityRanger.Reading(band: .searching,
                                                                  trend: .unknown,
                                                                  smoothedDBM: nil)
    /// Named neighbor currently outranking the target, when one clearly is.
    @Published private(set) var nearerNeighbor: String?
    /// The system said no (Settings → Privacy → Bluetooth) — shown honestly
    /// instead of an eternal "Listening…" over a radio we may not use.
    @Published private(set) var bluetoothDenied = false
    /// The radio is off (or still warming up).
    @Published private(set) var poweredOn = false

    private var central: CBCentralManager?
    private var ranger = ProximityRanger()
    /// The target's full fingerprint, and the named fingerprints of every
    /// OTHER fleet row — the neighbor hint's input.
    private var targetFingerprint = ""
    private var neighbors: [(name: String, fingerprint: String)] = []
    /// Freshest advert per neighbor name, pruned on the ranger's window.
    private var neighborHeard: [String: (dbm: Double, at: Date)] = [:]
    private var loop: Task<Void, Never>?
    private var running = false
    private var lastIngestedAt: Date?

    /// Restart the scan this often — resets CoreBluetooth's duplicate
    /// filtering so RSSI keeps flowing (see the scan-pulse note above).
    static let scanPulse: TimeInterval = 4

    /// Begin finding one Canary. Idempotent; `stop()` is the pair.
    func start(targetFingerprint: String,
               neighbors: [(name: String, fingerprint: String)]) {
        self.targetFingerprint = targetFingerprint
        self.neighbors = neighbors
        guard !running else { return }
        running = true
        ranger = ProximityRanger()
        // Creating the manager is what triggers watchOS's Bluetooth
        // permission prompt — deferred to here so it happens at the moment
        // the user asked to find something, never as an ambush at launch.
        if central == nil {
            central = CBCentralManager(delegate: self, queue: .main)
        } else {
            restartScanIfReady()
        }
        loop = Task { [weak self] in
            var beatsSincePulse = 0
            while !Task.isCancelled {
                guard let self else { return }
                self.tick()
                beatsSincePulse += 1
                // Two 0.5 s beats × 4 = the pulse cadence.
                if beatsSincePulse >= 8 {
                    beatsSincePulse = 0
                    self.restartScanIfReady()
                }
                try? await Task.sleep(for: .milliseconds(500))
            }
        }
    }

    func stop() {
        running = false
        loop?.cancel()
        loop = nil
        central?.stopScan()
    }

    /// Staleness + neighbor refresh on the half-second, matching the
    /// phone's Find loop cadence.
    private func tick() {
        let before = reading.band
        reading = ranger.reading(at: Date())
        playTick(from: before)
        let cutoff = Date().addingTimeInterval(-ProximityRanger.staleAfter)
        neighborHeard = neighborHeard.filter { $0.value.at >= cutoff }
        nearerNeighbor = ProximityRanger.nearerNeighbor(
            targetDBM: ranger.smoothedDBM,
            neighbors: neighborHeard.map { ($0.key, $0.value.dbm) })
    }

    /// WHEN a tap happens is the shared grammar's decision
    /// (ProximityRanger.tick, host-tested); the wrist adapter only grades
    /// it. Called on every band-moving path — advert and staleness alike —
    /// so a transition can never slip through unfelt or buzz twice.
    private func playTick(from before: ProximityBand) {
        WristFeedback.play(finding: ProximityRanger.tick(from: before, to: reading.band))
    }

    private func restartScanIfReady() {
        guard running, let central, central.state == .poweredOn else { return }
        central.stopScan()
        central.scanForPeripherals(withServices: nil,
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    }

}

extension WristFinder: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        Task { @MainActor in
            self.poweredOn = central.state == .poweredOn
            self.bluetoothDenied = central.state == .unauthorized
            self.restartScanIfReady()
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager,
                                    didDiscover peripheral: CBPeripheral,
                                    advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard let mfg = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data,
              let beacon = FleetBeacon.parse(manufacturerData: mfg) else { return }
        let rssiDBM = RSSI.intValue
        // CoreBluetooth uses 127 as "RSSI unavailable" — not a sample.
        guard rssiDBM != 127 else { return }
        Task { @MainActor in
            if beacon.matches(fingerprint: self.targetFingerprint) {
                let before = self.reading.band
                self.reading = self.ranger.ingest(rssiDBM: rssiDBM, at: Date())
                self.playTick(from: before)
            } else if let other = self.neighbors.first(where: {
                beacon.matches(fingerprint: $0.fingerprint)
            }) {
                self.neighborHeard[other.name] = (Double(rssiDBM), Date())
            }
        }
    }
}

#else

/// Simulator/host fallback — same shape, no radio (the WristFinder
/// contract the tests and previews compile against).
@MainActor
final class WristFinder: NSObject, ObservableObject {
    @Published private(set) var reading = ProximityRanger.Reading(band: .searching,
                                                                  trend: .unknown,
                                                                  smoothedDBM: nil)
    @Published private(set) var nearerNeighbor: String?
    @Published private(set) var bluetoothDenied = false
    @Published private(set) var poweredOn = false

    func start(targetFingerprint: String,
               neighbors: [(name: String, fingerprint: String)]) {}
    func stop() {}
}
#endif
