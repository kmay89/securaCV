// BLEConsole.swift
//
// CoreBluetooth client for a Canary's GATT console
// (firmware/projects/canary-wap/arduino/canary_wap/ble_console.h):
//   Service   8fc1cee0-b162-4401-9607-c8ac21383e90
//   Snapshot  8fc1cee1-…  (READ + NOTIFY, ≤220-byte UTF-8 JSON of device state)
// This is the "works even when Wi-Fi is down" path — and going native here is
// the whole reason not to ship a WebView: iOS Safari has no Web Bluetooth, so
// the device's own /companion PWA has to tell people to install Bluefy. Native
// CoreBluetooth erases that footnote. Bonded peers only (Numeric Comparison).

import Foundation
import CoreBluetooth

/// A compact snapshot pushed by the device over BLE NOTIFY.
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
    static let serviceUUID  = CBUUID(string: "8fc1cee0-b162-4401-9607-c8ac21383e90")
    static let snapshotUUID = CBUUID(string: "8fc1cee1-b162-4401-9607-c8ac21383e90")

    @Published private(set) var snapshotsByDevice: [String: BLESnapshot] = [:]
    @Published private(set) var poweredOn = false
    @Published private(set) var scanning = false

    private var central: CBCentralManager!
    private var peripherals: [UUID: CBPeripheral] = [:]

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func startScan() {
        guard poweredOn else { return }
        scanning = true
        // Scan by service so we only wake for Canaries, saving radio + battery.
        central.scanForPeripherals(withServices: [Self.serviceUUID],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }

    func stopScan() {
        scanning = false
        central.stopScan()
    }
}

extension BLEConsole: CBCentralManagerDelegate, CBPeripheralDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        poweredOn = central.state == .poweredOn
        if poweredOn && scanning { startScan() }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        peripherals[peripheral.identifier] = peripheral
        peripheral.delegate = self
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([Self.serviceUUID])
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        for service in peripheral.services ?? [] where service.uuid == Self.serviceUUID {
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
