// BLEConsole.swift
//
// The BLE transport — "works even when Wi-Fi is down", and now for the WHOLE
// fleet rather than one board family. Two layers, deliberately separated:
//
//   1. PRESENCE BEACON (universal). Every Canary continuously broadcasts the
//      11-byte fleet-link presence beacon as its PRIMARY advertisement —
//      canary-vision and canary-sense from `src/net/fleet_beacon_adv.cpp`,
//      canary-wap/canary-display through `securacv_ble_status`. No broker, no
//      home Wi-Fi, no pairing, no connection. Decoding is pure: Wire/FleetBeacon.
//
//   2. GATT CONSOLE (WAP-class enrichment). canary-wap additionally serves a
//      connectable console with a richer JSON snapshot. We connect only when a
//      peripheral actually advertises that service.
//
// WHY WE NO LONGER SCAN FILTERED BY SERVICE UUID: the firmware moved the
// service UUIDs + device name into the SCAN RESPONSE to make room for the
// beacon in the primary advert (`apply_beacon_advertising()`). A
// service-filtered scan is therefore both too narrow (it can never see
// beacon-only boards — vision, sense) and fragile (it depends on iOS matching
// the filter against scan-response UUIDs). Scanning unfiltered and filtering in
// code on the manufacturer payload is strictly more reliable and is the only
// way to hear the boards that run no GATT console at all.
//
// FOREGROUND-ONLY, BY DESIGN: an unfiltered scan with duplicates enabled does
// not run in the background, and iOS withholds manufacturer data from
// background scans. FleetStore stops the scan when the scene deactivates, so
// this is the live-fleet view's transport, not a background delivery path —
// that remains APNs through the notification service extension.

import Foundation
import CoreBluetooth

/// A compact snapshot pushed by a WAP-class device over BLE NOTIFY.
struct BLESnapshot: Codable, Hashable, Sendable {
    var id: String?
    var sev: Int?
    var link: Int?
    var batt: Int?
    var health: Int?
    var muted: Bool?
    var tamper: Bool?
}

@MainActor
final class BLEConsole: NSObject, ObservableObject {
    /// canary-wap's connectable console (ble_console.h).
    static let consoleServiceUUID = CBUUID(string: "8fc1cee0-b162-4401-9607-c8ac21383e90")
    static let snapshotUUID       = CBUUID(string: "8fc1cee1-b162-4401-9607-c8ac21383e90")
    /// The `canary` firmware family's status service (securacv_ble_status.h).
    /// Known so its adverts are recognized as ours; its characteristics are not
    /// read yet — the beacon already carries this family's state.
    static let statusServiceUUID  = CBUUID(string: "5e63a1b0-7c3d-4f2e-8a91-0d1b2c3e4f5a")

    /// A Canary we've stopped hearing is dark, not fine. Beacons repeat every
    /// few seconds; a minute of silence is unambiguous.
    static let staleAfter: TimeInterval = 60

    @Published private(set) var sightings: [UUID: BeaconSighting] = [:]
    @Published private(set) var snapshotsByDevice: [String: BLESnapshot] = [:]
    @Published private(set) var poweredOn = false
    @Published private(set) var scanning = false

    private var central: CBCentralManager!
    private var peripherals: [UUID: CBPeripheral] = [:]
    /// Whether a scan has been *requested*, independent of whether the radio is
    /// ready. CBCentralManager starts in `.unknown` and only reaches
    /// `.poweredOn` a moment later, so at launch `startScan()` always arrives
    /// too early — and Bluetooth can be toggled off and on while the scene
    /// stays active. Remembering the intent is what lets the state callback
    /// pick the scan up; keying that off `scanning` instead would deadlock,
    /// because `scanning` can only become true once the radio is already on.
    private var wantsScan = false

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    /// Beacons heard recently enough to still mean something.
    var freshSightings: [BeaconSighting] {
        let cutoff = Date().addingTimeInterval(-Self.staleAfter)
        return sightings.values
            .filter { $0.lastHeard >= cutoff }
            .sorted { $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending }
    }

    func startScan() {
        wantsScan = true
        guard poweredOn else { return }   // resumed by centralManagerDidUpdateState
        scanning = true
        // Unfiltered: the beacon lives in manufacturer data, which no service
        // filter can select for. Duplicates ON so a status change (tamper!)
        // arrives on the next advert instead of only on first sight — the whole
        // point of a *presence* beacon is that it keeps telling you.
        central.scanForPeripherals(withServices: nil,
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    }

    func stopScan() {
        wantsScan = false
        scanning = false
        central.stopScan()
    }

    /// Drop sightings we haven't heard in a while so the fleet view can't show
    /// a Canary as present on the strength of a minutes-old advert.
    func pruneStaleSightings(now: Date = Date()) {
        let cutoff = now.addingTimeInterval(-Self.staleAfter)
        sightings = sightings.filter { $0.value.lastHeard >= cutoff }
    }
}

extension BLEConsole: CBCentralManagerDelegate, CBPeripheralDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        poweredOn = central.state == .poweredOn
        if poweredOn {
            if wantsScan { startScan() }        // the radio caught up with us
        } else {
            scanning = false                    // powered off: we are not scanning
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        // ── Layer 1: the universal presence beacon ──
        if let mfg = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data,
           let beacon = FleetBeacon.parse(manufacturerData: mfg) {
            sightings[peripheral.identifier] = BeaconSighting(
                beacon: beacon,
                rssiDBM: RSSI.intValue,
                lastHeard: Date(),
                peripheralID: peripheral.identifier,
                localName: advertisementData[CBAdvertisementDataLocalNameKey] as? String
            )
        }

        // ── Layer 2: connect ONLY to a WAP-class console ──
        // Everything else is heard, never dialed: connecting to arbitrary
        // peripherals would waste radio and, on a beacon-only board, achieve
        // nothing.
        let advertised = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID]) ?? []
        guard advertised.contains(Self.consoleServiceUUID) else { return }
        guard peripherals[peripheral.identifier] == nil else { return }   // already dialing/connected
        peripherals[peripheral.identifier] = peripheral
        peripheral.delegate = self
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        peripherals[peripheral.identifier] = nil      // let a later advert retry
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        peripherals[peripheral.identifier] = nil
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([Self.consoleServiceUUID])
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        for service in peripheral.services ?? [] where service.uuid == Self.consoleServiceUUID {
            peripheral.discoverCharacteristics([Self.snapshotUUID], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for ch in service.characteristics ?? [] where ch.uuid == Self.snapshotUUID {
            peripheral.setNotifyValue(true, for: ch)   // live pushes
            peripheral.readValue(for: ch)              // and one now
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let data = characteristic.value,
              let snap = try? JSONDecoder().decode(BLESnapshot.self, from: data) else { return }
        let key = snap.id ?? peripheral.identifier.uuidString
        snapshotsByDevice[key] = snap
    }
}
