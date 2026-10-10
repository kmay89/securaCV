// BLEConsole.swift
//
// The BLE transport — "works even when Wi-Fi is down", and now for the WHOLE
// fleet rather than one board family. Two layers, deliberately separated:
//
//   1. PRESENCE BEACON (universal). Every Canary continuously broadcasts the
//      fleet-link presence beacon (11-byte v1, or the 13-byte v2 that vision
//      boards send with a coarse detection hint) as its PRIMARY advertisement —
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
// NO MANAGER UNTIL ASKED: creating a CBCentralManager is what makes iOS show
// its Bluetooth permission alert, so this object creates none at init — the
// first scan (after the discovery consent, FleetStore) or the first setup
// tap does. A manager built at launch put the system alert in front of the
// consent card that explains it, and a "Don't Allow" given with no context
// left the new-Canary card unable to ever appear. (The watch's WristFinder
// learned the same lesson first.)
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
    /// canary-wap's bonded Wi-Fi provisioning service (ble_provision.h) —
    /// the rescue path for a Canary the router password change stranded.
    static let provisionServiceUUID = CBUUID(string: "8fc1cef0-b162-4401-9607-c8ac21383e90")
    static let provisionCredsUUID   = CBUUID(string: "8fc1cef3-b162-4401-9607-c8ac21383e90")
    static let provisionStateUUID   = CBUUID(string: "8fc1cef4-b162-4401-9607-c8ac21383e90")
    /// The Improv Wi-Fi setup door every Sense, Vision and WAP opens while
    /// it has no Wi-Fi of its own (firmware/common/network/improv_ble): the
    /// 128-bit service in the scan response, and its 0x4677 service data
    /// carrying the state and capabilities (Shared/ImprovWire.swift).
    static let improvServiceUUID     = CBUUID(string: ImprovWire.serviceUUID)
    static let improvServiceDataUUID = CBUUID(string: "4677")

    /// A Canary we've stopped hearing is dark, not fine. Beacons repeat every
    /// few seconds; a minute of silence is unambiguous.
    static let staleAfter: TimeInterval = 60

    @Published private(set) var sightings: [UUID: BeaconSighting] = [:]
    /// Chirps heard, keyed like `sightings`. A SEPARATE map on purpose: a
    /// chirping Canary stops advertising its beacon for 2 seconds, so a
    /// "latest mfg blob wins" store would erase the beacon's status every
    /// time the device had something to say — the display keeps them apart
    /// for the same reason.
    @Published private(set) var chirpSightings: [UUID: ChirpSighting] = [:]
    @Published private(set) var snapshotsByDevice: [String: BLESnapshot] = [:]
    /// Everything heard per peripheral that the "new Canary nearby" card
    /// reads (Shared/NearbyCanary.swift): beacon, name, the Improv service
    /// and its service data. Kept short; the policy applies its own window.
    @Published private(set) var heard: [UUID: NearbyCanaries.Heard] = [:]
    @Published private(set) var poweredOn = false
    @Published private(set) var scanning = false
    /// The radio as the setup screens need it (Model/AddCanaryFlow.swift):
    /// off, not allowed, or able to hear — so no screen spins "Listening…"
    /// over a radio that cannot. Read from the class-level authorization
    /// before any manager exists (that read never prompts), then from the
    /// manager's own state reports.
    @Published private(set) var radio: RadioStanding = BLEConsole.standing(authorization: CBCentralManager.authorization)

    /// Nil until something needs the radio — see "NO MANAGER UNTIL ASKED".
    private var central: CBCentralManager?
    private var peripherals: [UUID: CBPeripheral] = [:]
    /// Every peripheral seen recently, held so one can be connected on
    /// demand for setup (CoreBluetooth needs a strong reference to connect).
    private var seenPeripherals: [UUID: CBPeripheral] = [:]
    /// The one setup ceremony in flight, if any; the central's callbacks for
    /// its peripheral are forwarded there instead of the console path.
    private(set) var setupClient: ImprovClient?
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
    }

    /// Whether the central manager (and so the system's Bluetooth prompt)
    /// exists yet. False from launch until a scan or a setup asks — the
    /// guard AddCanaryFlowTests holds.
    var hasManager: Bool { central != nil }

    /// The one place a manager is born: on the first real need.
    @discardableResult
    private func ensureCentral() -> CBCentralManager {
        if let central { return central }
        let made = CBCentralManager(delegate: self, queue: .main)
        central = made
        return made
    }

    /// CBManagerAuthorization → the setup screens' standing, for the time
    /// before a manager has reported a state.
    nonisolated static func standing(authorization: CBManagerAuthorization) -> RadioStanding {
        switch authorization {
        case .denied, .restricted: return .denied
        default: return .unknown
        }
    }

    /// A manager's reported state → the setup screens' standing.
    nonisolated static func standing(state: CBManagerState) -> RadioStanding {
        switch state {
        case .poweredOn: return .ready
        case .poweredOff: return .off
        case .unauthorized: return .denied
        case .unsupported: return .unsupported
        default: return .unknown          // .unknown, .resetting: a report is coming
        }
    }

    /// Beacons heard recently enough to still mean something.
    var freshSightings: [BeaconSighting] {
        let cutoff = Date().addingTimeInterval(-Self.staleAfter)
        return sightings.values
            .filter { $0.lastHeard >= cutoff }
            .sorted { $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending }
    }

    /// Chirps heard recently — same window as the beacons: a chirp is proof
    /// of life the moment it lands, and a minute later it is history.
    var freshChirps: [ChirpSighting] {
        let cutoff = Date().addingTimeInterval(-Self.staleAfter)
        return chirpSightings.values
            .filter { $0.lastHeard >= cutoff }
            .sorted { $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending }
    }

    func startScan() {
        wantsScan = true
        let central = ensureCentral()     // the first call is the prompt's moment
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
        // No manager yet means nothing was ever scanning — and stopping
        // must never be the thing that creates one (and its prompt).
        if let central, central.state == .poweredOn { central.stopScan() }
    }

    /// Drop sightings we haven't heard in a while so the fleet view can't show
    /// a Canary as present on the strength of a minutes-old advert.
    func pruneStaleSightings(now: Date = Date()) {
        let cutoff = now.addingTimeInterval(-Self.staleAfter)
        sightings = sightings.filter { $0.value.lastHeard >= cutoff }
        chirpSightings = chirpSightings.filter { $0.value.lastHeard >= cutoff }
        heard = heard.filter { $0.value.lastHeard >= cutoff }
        seenPeripherals = seenPeripherals.filter { heard[$0.key] != nil || sightings[$0.key] != nil
            || chirpSightings[$0.key] != nil || peripherals[$0.key] != nil || setupClient?.peripheralID == $0.key }
    }

    // MARK: - the Bluetooth setup door (Improv Wi-Fi)

    /// Begin a setup ceremony with one heard Canary: connect to it and hand
    /// the link to an ImprovClient. One at a time; a ceremony already running
    /// is returned as is. Nil when the peripheral is no longer around.
    func beginSetup(_ peripheralID: UUID) -> ImprovClient? {
        if let running = setupClient {
            if running.peripheralID == peripheralID { return running }
            endSetup()
        }
        guard let peripheral = seenPeripherals[peripheralID] else { return nil }
        let client = ImprovClient(peripheral: peripheral)
        setupClient = client
        if peripheral.state == .connected {
            client.centralDidConnect()
        } else {
            ensureCentral().connect(peripheral)
        }
        return client
    }

    /// Drop the setup link (the ceremony is over, or the person backed out).
    func endSetup() {
        guard let client = setupClient else { return }
        setupClient = nil
        if let peripheral = seenPeripherals[client.peripheralID], peripheral.state != .disconnected {
            central?.cancelPeripheralConnection(peripheral)
        }
    }

    // MARK: - Wi-Fi provisioning over BLE (the rescue path)

    /// Which console peripheral a device id maps to — learned from the
    /// snapshot's own `id` field, so the mapping is the device's claim, not
    /// a guess from advertising order.
    private var peripheralIDByDevice: [String: UUID] = [:]

    /// Canaries whose console is connected RIGHT NOW — the set the Wi-Fi
    /// rollout may offer the Bluetooth rescue to.
    var provisionableDeviceIDs: Set<String> {
        Set(peripheralIDByDevice.compactMap { id, peripheralID in
            peripherals[peripheralID]?.state == .connected ? id : nil
        })
    }

    /// Fires the moment a connected console reports tamper that wasn't
    /// there a beat ago — BLE NOTIFY is sub-second, so the alert loop gets
    /// to run NOW instead of at the next poll. Wired by FleetStore.
    var onUrgentSnapshot: ((String) -> Void)?

    /// Same contract for the connectionless cry: a tamper or alert CHIRP is
    /// the fastest off-grid signal a Canary has, and it used to wait out the
    /// 20-second poll the GATT NOTIFY path bypasses. Fired once per parsed
    /// tamper/alert chirp; heartbeat/witness/boot stay poll-paced (they are
    /// liveness, not cries). Wired by FleetStore.
    var onUrgentChirp: ((UUID) -> Void)?

    /// One credentials write in flight at a time — the firmware rate-limits
    /// to one per 5 s anyway, and a second concurrent write could only
    /// steal the first one's answer.
    private struct PendingProvision {
        let peripheralID: UUID
        let payload: Data
        var continuation: CheckedContinuation<BLEProvisionOutcome, Never>?
        let startedAt = Date()
    }
    private var pendingProvision: PendingProvision?
    /// Which write the running timeout belongs to — a stale timer from an
    /// EARLIER write must never resolve a later one.
    private var provisionGeneration = 0

    /// Hand a stranded Canary new Wi-Fi credentials over its bonded
    /// provisioning service, and wait for the device's OWN verdict (the
    /// STATE characteristic saying `connected` / `failed`). iOS raises the
    /// pairing sheet on first use; the bond is the security boundary.
    func writeWiFiCredentials(deviceID: String, ssid: String, password: String) async -> BLEProvisionOutcome {
        guard pendingProvision == nil else {
            return .failed("Another Canary's rescue is still in progress.")
        }
        guard let peripheralID = peripheralIDByDevice[deviceID],
              let peripheral = peripherals[peripheralID],
              peripheral.state == .connected else {
            return .unreachable
        }
        struct Creds: Encodable { let ssid: String; let password: String }
        guard let payload = try? JSONEncoder().encode(Creds(ssid: ssid, password: password)) else {
            return .failed("Couldn't encode the credentials.")
        }
        provisionGeneration += 1
        let generation = provisionGeneration
        let outcome = await withCheckedContinuation { (cont: CheckedContinuation<BLEProvisionOutcome, Never>) in
            pendingProvision = PendingProvision(peripheralID: peripheralID,
                                                payload: payload,
                                                continuation: cont)
            peripheral.discoverServices([Self.provisionServiceUUID])
            // The whole ceremony — bond, subscribe, write, join, notify —
            // gets 75 seconds; past that the answer is honestly "no answer",
            // never a spinner that outlives the user's patience. The
            // generation check keeps this timer from ever touching a LATER
            // write's ceremony.
            Task { [weak self] in
                try? await Task.sleep(for: .seconds(75))
                guard let self, self.provisionGeneration == generation else { return }
                self.resolveProvision(.failed(
                    "No answer over Bluetooth — move closer to the Canary and try again."))
            }
        }
        return outcome
    }

    /// Resolve the in-flight provisioning exactly once; later calls no-op.
    private func resolveProvision(_ outcome: BLEProvisionOutcome) {
        guard let pending = pendingProvision else { return }
        pendingProvision = nil
        pending.continuation?.resume(returning: outcome)
    }
}

/// How a BLE credentials write ended — the device's own verdict, or the
/// honest reason we never got one.
enum BLEProvisionOutcome: Hashable, Sendable {
    case joined                 // STATE said `connected`: it is on the new network
    case failed(String)
    case unreachable            // no connected console for that device right now
}

extension BLEConsole: CBCentralManagerDelegate, CBPeripheralDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        poweredOn = central.state == .poweredOn
        radio = Self.standing(state: central.state)
        if poweredOn {
            if wantsScan { startScan() }        // the radio caught up with us
        } else {
            scanning = false                    // powered off: we are not scanning
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        // ── Layer 1: the universal presence beacon — or, for 2 seconds at a
        // time, the chirp that replaces it on air. Each parser rejects the
        // other's length, so the order is cosmetic; what matters is that a
        // chirp lands in its OWN map and the beacon's last sighting stands
        // (2 s of replacement is well inside the staleness window).
        if let mfg = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data {
            if let beacon = FleetBeacon.parse(manufacturerData: mfg) {
                sightings[peripheral.identifier] = BeaconSighting(
                    beacon: beacon,
                    rssiDBM: RSSI.intValue,
                    lastHeard: Date(),
                    peripheralID: peripheral.identifier,
                    localName: advertisementData[CBAdvertisementDataLocalNameKey] as? String
                )
            } else if let chirp = ChirpAdvert.parse(manufacturerData: mfg) {
                // A repeated chirp (the 2 s broadcast window, duplicates on)
                // must not re-fire the urgent path every advert — but a NEW
                // cry must, even of the same kind: sightings live well past
                // the broadcast, so dedup by kind alone would swallow a
                // second distinct alert for as long as the first is
                // remembered. Suppress only a same-kind repeat still inside
                // one broadcast window (2 s, with margin for scan latency);
                // anything later is a fresh cry, heard fresh.
                let previous = chirpSightings[peripheral.identifier]
                let isSameBurst = previous.map {
                    $0.chirp.kind == chirp.kind
                        && Date().timeIntervalSince($0.lastHeard) < 10
                } ?? false
                chirpSightings[peripheral.identifier] = ChirpSighting(
                    chirp: chirp,
                    rssiDBM: RSSI.intValue,
                    lastHeard: Date(),
                    peripheralID: peripheral.identifier,
                    localName: advertisementData[CBAdvertisementDataLocalNameKey] as? String
                )
                if (chirp.kind == .tamper || chirp.kind == .alert), !isSameBurst {
                    onUrgentChirp?(peripheral.identifier)
                }
            }
        }

        // ── Layer 1b: what the "new Canary nearby" card reads. A beacon with
        // its setup bit, a "Sense-AB12" name, the Improv service in the scan
        // response and its service data can arrive across more than one
        // callback, so each piece sticks until the sighting goes stale.
        let advertised = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID]) ?? []
        let serviceData = advertisementData[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data]
        let improvData = serviceData?[Self.improvServiceDataUUID].flatMap(ImprovWire.parseServiceData)
        var h = heard[peripheral.identifier] ?? NearbyCanaries.Heard(
            peripheralID: peripheral.identifier, beacon: nil, localName: nil, improvAdvertised: false,
            improvServiceData: nil, rssiDBM: RSSI.intValue, lastHeard: Date())
        if let beacon = sightings[peripheral.identifier]?.beacon { h.beacon = beacon }
        if let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String, !name.isEmpty { h.localName = name }
        if advertised.contains(Self.improvServiceUUID) { h.improvAdvertised = true }
        if let improvData { h.improvServiceData = improvData }
        h.rssiDBM = RSSI.intValue
        h.lastHeard = Date()
        heard[peripheral.identifier] = h
        seenPeripherals[peripheral.identifier] = peripheral

        // ── Layer 2: connect ONLY to a WAP-class console ──
        // Everything else is heard, never dialed: connecting to arbitrary
        // peripherals would waste radio and, on a beacon-only board, achieve
        // nothing — except the one a person tapped the setup card for
        // (beginSetup), which dials on demand.
        guard advertised.contains(Self.consoleServiceUUID) else { return }
        guard peripherals[peripheral.identifier] == nil else { return }   // already dialing/connected
        peripherals[peripheral.identifier] = peripheral
        peripheral.delegate = self
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        if let client = setupClient, client.peripheralID == peripheral.identifier {
            client.centralDidFailToConnect(error)
            return
        }
        peripherals[peripheral.identifier] = nil      // let a later advert retry
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        if let client = setupClient, client.peripheralID == peripheral.identifier {
            client.centralDidDisconnect(error)
            return
        }
        peripherals[peripheral.identifier] = nil
        // A rescue mid-flight lost its device. Two honest readings: if the
        // credentials were already written, the device may simply have
        // rebooted onto the new network — say to watch for it; otherwise
        // the link dropped before anything was handed over.
        if pendingProvision?.peripheralID == peripheral.identifier {
            resolveProvision(.failed("Bluetooth dropped before the Canary answered — watch the fleet list to see if it rejoined."))
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        if let client = setupClient, client.peripheralID == peripheral.identifier {
            client.centralDidConnect()
            return
        }
        peripheral.discoverServices([Self.consoleServiceUUID])
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        for service in peripheral.services ?? [] {
            switch service.uuid {
            case Self.consoleServiceUUID:
                peripheral.discoverCharacteristics([Self.snapshotUUID], for: service)
            case Self.provisionServiceUUID where pendingProvision?.peripheralID == peripheral.identifier:
                peripheral.discoverCharacteristics([Self.provisionCredsUUID, Self.provisionStateUUID],
                                                   for: service)
            default:
                break
            }
        }
        // A rescue asked for the provisioning service and the device doesn't
        // serve it (older firmware, or Bluetooth turned down in settings).
        if pendingProvision?.peripheralID == peripheral.identifier,
           !(peripheral.services ?? []).contains(where: { $0.uuid == Self.provisionServiceUUID }) {
            resolveProvision(.failed("This Canary's firmware doesn't offer the Bluetooth rescue — use its setup portal (hold BOOT) instead."))
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        switch service.uuid {
        case Self.consoleServiceUUID:
            for ch in service.characteristics ?? [] where ch.uuid == Self.snapshotUUID {
                peripheral.setNotifyValue(true, for: ch)   // live pushes
                peripheral.readValue(for: ch)              // and one now
            }
        case Self.provisionServiceUUID:
            guard let pending = pendingProvision, pending.peripheralID == peripheral.identifier else { return }
            let chars = service.characteristics ?? []
            guard let creds = chars.first(where: { $0.uuid == Self.provisionCredsUUID }),
                  let state = chars.first(where: { $0.uuid == Self.provisionStateUUID }) else {
                resolveProvision(.failed("This Canary's provisioning service is missing its controls."))
                return
            }
            // Subscribe to the verdict FIRST, then hand over the
            // credentials — a write whose answer nobody is listening for
            // races the notification.
            peripheral.setNotifyValue(true, for: state)
            peripheral.writeValue(pending.payload, for: creds, type: .withResponse)
        default:
            break
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == Self.provisionCredsUUID,
              pendingProvision?.peripheralID == peripheral.identifier else { return }
        if let error {
            // The bond ceremony refused, or the firmware's rate limit spoke.
            resolveProvision(.failed("The Canary refused the write — \(error.localizedDescription)"))
        }
        // On success: stay quiet and let the STATE characteristic answer.
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        switch characteristic.uuid {
        case Self.snapshotUUID:
            guard let data = characteristic.value,
                  let snap = try? JSONDecoder().decode(BLESnapshot.self, from: data) else { return }
            let key = snap.id ?? peripheral.identifier.uuidString
            let tamperRose = (snap.tamper == true) && (snapshotsByDevice[key]?.tamper != true)
            snapshotsByDevice[key] = snap
            if snap.id != nil { peripheralIDByDevice[key] = peripheral.identifier }
            // Tamper over BLE NOTIFY is the fastest signal the fleet has —
            // hand it to the alert loop now, not at the next poll.
            if tamperRose { onUrgentSnapshot?(key) }
        case Self.provisionStateUUID:
            guard pendingProvision?.peripheralID == peripheral.identifier,
                  let data = characteristic.value,
                  let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let state = obj["state"] as? String else { return }
            switch state {
            case "connected":
                resolveProvision(.joined)
            case "failed":
                resolveProvision(.failed("The Canary couldn't join with those credentials — check the password."))
            case "rate_limited":
                resolveProvision(.failed("The Canary is rate-limiting credential writes — try again in a few minutes."))
            default:
                break   // idle / scanning / connecting — the verdict is still coming
            }
        default:
            break
        }
    }
}
