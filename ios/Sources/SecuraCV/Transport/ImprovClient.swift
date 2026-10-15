// ImprovClient.swift
//
// One Canary's Bluetooth setup door, driven from the phone: connect, find
// the Improv Wi-Fi service, ask the Canary which networks it sees, hand it
// the home Wi-Fi, and wait for its own verdict. The bytes and the rules are
// in Shared/ImprovWire.swift (pure, tested); this file is the CoreBluetooth
// glue and nothing else.
//
// The link is encrypted by the firmware's choice (the RPC characteristic
// requires it), so the first write makes iOS show its one-tap pairing
// sheet; CoreBluetooth pairs and retries the write on its own. Nothing is
// bonded — the next setup pairs afresh, which is the point.
//
// A Canary may also serve SecuraCV's companion service beside the Improv
// one (ImprovWire.companionServiceUUID), two characteristics, each
// optional — a missing one fails nothing, and readiness never waits for
// either:
//   * CLAIM (a WAP): readable once by this link after the join it asked
//     for succeeded. A claim ticket, not the receipt — the bearer token
//     never rides Bluetooth; the sheet spends the claim over the home Wi-Fi
//     (Shared/MagicPairPlan.swift) and the receipt comes back over that.
//   * HUB (a Sense or Vision): written once after the join, with the hub
//     this phone knows (ImprovWire.hubHandoff), then read back for the
//     Canary's one-byte answer (ImprovWire.HubHandoffStatus).
//
// Two more things the link does for the person's sake: a join the Canary
// refused leaves its door open on the same link (recoverAfterFailedJoin —
// a wrong password costs a retype, not a reconnect), and a quiet link is
// kept alive with a harmless command (ping) while they find the password,
// because the firmware drops a silent link after three minutes.
//
// Ownership: BLEConsole owns the central and the scan. It hands one
// CBPeripheral to one ImprovClient at a time (beginSetup) and forwards the
// central's connect / disconnect callbacks for it here.

import CoreBluetooth
import Foundation

@MainActor
final class ImprovClient: NSObject, ObservableObject {
    static let serviceUUID      = CBUUID(string: ImprovWire.serviceUUID)
    static let stateUUID        = CBUUID(string: ImprovWire.currentStateUUID)
    static let errorUUID        = CBUUID(string: ImprovWire.errorStateUUID)
    static let commandUUID      = CBUUID(string: ImprovWire.rpcCommandUUID)
    static let resultUUID       = CBUUID(string: ImprovWire.rpcResultUUID)
    static let capabilitiesUUID = CBUUID(string: ImprovWire.capabilitiesUUID)
    /// SecuraCV's companion service beside the Improv one (ImprovWire's
    /// header has both JSON shapes): CLAIM on a WAP, readable once by the
    /// link that provisioned; HUB on a Sense or Vision, written once with
    /// the hub this phone knows and read back for the answer. Both
    /// encrypted, both optional — never required for the door itself.
    static let companionServiceUUID = CBUUID(string: ImprovWire.companionServiceUUID)
    static let claimUUID            = CBUUID(string: ImprovWire.claimUUID)
    static let hubUUID              = CBUUID(string: ImprovWire.hubUUID)

    typealias HubHandoffStatus = ImprovWire.HubHandoffStatus

    enum Phase: Equatable, Sendable {
        case connecting
        case discovering
        /// Connected, subscribed, and the Canary's state read.
        case ready
        case scanning
        case sending
        /// The Canary accepted the credentials and is joining.
        case joining
        /// The Canary said it joined; the URLs it answered with (may be empty).
        case joined([String])
        case failed(String)
        case disconnected
    }

    @Published private(set) var phase: Phase = .connecting
    @Published private(set) var lastError: ImprovWire.Error = .none
    @Published private(set) var capabilities: UInt8 = 0
    /// The networks the Canary reported, strongest first as it sent them.
    @Published private(set) var networks: [ImprovWire.Network] = []
    /// Firmware name, version, hardware, device name — once asked.
    @Published private(set) var deviceInfo: [String] = []
    /// The first encrypted write (the network scan, when the Canary can
    /// scan) is what raises iOS's Pair sheet; when it was declined or
    /// ignored, the scan comes back empty for that reason and not because
    /// the Canary heard nothing. The sheet says so instead of silently
    /// dropping to a typed name. Cleared by the next write that lands.
    @Published private(set) var pairingDeclined = false
    /// A write to an encrypted characteristic has landed on this link, so
    /// iOS's Pair sheet is behind us. Until then, nothing writes on a
    /// timer: the first encrypted write is what raises that sheet, and it
    /// is raised only by something the person can see on screen.
    @Published private(set) var linkEncrypted = false
    /// The URLs the last WIFI_SETTINGS result carried.
    private var lastURLs: [String] = []

    let peripheralID: UUID
    private let peripheral: CBPeripheral
    private var state: CBCharacteristic?
    private var error: CBCharacteristic?
    private var command: CBCharacteristic?
    private var result: CBCharacteristic?
    private var claim: CBCharacteristic?
    private var claimWaiter: CheckedContinuation<Data?, Never>?
    private var hub: CBCharacteristic?
    private var hubWaiter: CheckedContinuation<HubHandoffStatus, Never>?

    /// One request in flight at a time; each resumes exactly once.
    private var readyWaiter: CheckedContinuation<Bool, Never>?
    private var scanWaiter: CheckedContinuation<[ImprovWire.Network], Never>?
    private var joinWaiter: CheckedContinuation<Outcome, Never>?
    private var generation = 0

    enum Outcome: Hashable, Sendable {
        case joined([String])
        case failed(String)
    }

    init(peripheral: CBPeripheral) {
        self.peripheral = peripheral
        self.peripheralID = peripheral.identifier
        super.init()
        peripheral.delegate = self
    }

    // MARK: - what BLEConsole forwards

    func centralDidConnect() {
        phase = .discovering
        peripheral.discoverServices([Self.serviceUUID, Self.companionServiceUUID])
    }

    func centralDidFailToConnect(_ err: Error?) {
        fail("Couldn't connect over Bluetooth — move closer to the Canary and try again.")
        phase = .disconnected
    }

    func centralDidDisconnect(_ err: Error?) {
        // A join in flight is not lost: the Canary may have rebooted onto
        // the network; the card then watches the fleet for it.
        if phase == .joining {
            resolveJoin(.failed("Bluetooth dropped while the Canary was joining — watch the Fleet tab to see if it made it."))
        } else {
            fail("Bluetooth dropped before the Canary answered.")
        }
        phase = .disconnected
    }

    // MARK: - requests

    /// Wait until the service is discovered and subscribed (or give up).
    func waitUntilReady(timeout: Duration = .seconds(20)) async -> Bool {
        if phase == .ready { return true }
        if case .failed = phase { return false }
        if phase == .disconnected { return false }
        generation += 1
        let gen = generation
        return await withCheckedContinuation { cont in
            readyWaiter = cont
            Task { [weak self] in
                try? await Task.sleep(for: timeout)
                guard let self, self.generation == gen, let w = self.readyWaiter else { return }
                self.readyWaiter = nil
                self.phase = .failed("The Canary didn't answer over Bluetooth in time.")
                w.resume(returning: false)
            }
        }
    }

    /// Ask the Canary which networks it sees (GET_WIFI_NETWORKS). Empty when
    /// it cannot scan right now; the card then offers a typed name.
    func scanNetworks(timeout: Duration = .seconds(35)) async -> [ImprovWire.Network] {
        guard phase == .ready, let command else { return [] }
        networks = []
        phase = .scanning
        generation += 1
        let gen = generation
        return await withCheckedContinuation { cont in
            scanWaiter = cont
            peripheral.writeValue(ImprovWire.wifiNetworks, for: command, type: .withResponse)
            Task { [weak self] in
                try? await Task.sleep(for: timeout)
                guard let self, self.generation == gen, let w = self.scanWaiter else { return }
                self.scanWaiter = nil
                if self.phase == .scanning { self.phase = .ready }
                w.resume(returning: self.networks)
            }
        }
    }

    /// Hand over the home Wi-Fi and wait for the Canary's own verdict.
    func provision(ssid: String, password: String, timeout: Duration = .seconds(55)) async -> Outcome {
        guard phase == .ready || phase == .scanning, let command else {
            return .failed("The Canary's Bluetooth setup door isn't ready.")
        }
        guard let frame = ImprovWire.wifiSettings(ssid: ssid, password: password) else {
            return .failed("That network name or password is longer than Wi-Fi allows.")
        }
        phase = .sending
        generation += 1
        let gen = generation
        return await withCheckedContinuation { cont in
            joinWaiter = cont
            peripheral.writeValue(frame, for: command, type: .withResponse)
            Task { [weak self] in
                try? await Task.sleep(for: timeout)
                guard let self, self.generation == gen else { return }
                self.resolveJoin(.failed("No verdict from the Canary in time — it may still be joining; watch the Fleet tab."))
            }
        }
    }

    /// Make it blink, so two Canaries on the table can be told apart.
    func identify() {
        guard let command, phase != .connecting, phase != .discovering else { return }
        peripheral.writeValue(ImprovWire.identify, for: command, type: .withResponse)
    }

    /// Keep a quiet link alive while the person finds the password: the
    /// firmware drops a link that has sent nothing for three minutes
    /// (improv_core.h idle_disconnect_ms) and counts any command as
    /// activity (session_touch) — GET_DEVICE_INFO is the one with no side
    /// effect, and the Canary answers it. Only once a write has landed on
    /// this link: before that the write itself would raise iOS's Pair
    /// sheet, with nothing on screen to say why.
    func ping() {
        guard phase == .ready, linkEncrypted, let command else { return }
        peripheral.writeValue(ImprovWire.deviceInfo, for: command, type: .withResponse)
    }

    /// A join the Canary refused — UnableToConnect: a wrong password, or a
    /// network it cannot see — leaves its door open on the SAME link
    /// (improv_core.h session_on_join_result: back to Authorized), so the
    /// next write need not reconnect. True when the link is still up and
    /// the phase is back at ready for it; false when the link is gone or
    /// the failure was something else (the sheet reconnects then).
    @discardableResult
    func recoverAfterFailedJoin() -> Bool {
        guard case .failed = phase, lastError == .unableToConnect,
              peripheral.state == .connected, command != nil else { return false }
        lastError = .none
        phase = .ready
        return true
    }

    /// Whether this Canary offers a claim ticket beside the door (a WAP).
    var offersClaim: Bool { claim != nil }

    /// Whether this Canary takes a hub over the door (a Sense or Vision
    /// with the HUB characteristic; an older firmware has none).
    var offersHubHandoff: Bool { hub != nil }

    /// Hand the Canary the hub this phone knows (ImprovWire.hubHandoff
    /// built `data`): one write with response, then one read for its
    /// answer. `.unknown` when it offers no HUB, the write was refused by
    /// the link, or nothing came back in time — never a guess at what the
    /// Canary did.
    func handOverHub(_ data: Data, timeout: Duration = .seconds(10)) async -> HubHandoffStatus {
        guard let hub else { return .unknown }
        generation += 1
        let gen = generation
        return await withCheckedContinuation { cont in
            hubWaiter = cont
            peripheral.writeValue(data, for: hub, type: .withResponse)
            Task { [weak self] in
                try? await Task.sleep(for: timeout)
                guard let self, self.generation == gen, let w = self.hubWaiter else { return }
                self.hubWaiter = nil
                w.resume(returning: .unknown)
            }
        }
    }

    /// Read the claim the join earned — the link that provisioned gets it
    /// once; anyone else, and any later read, gets "{}". Nil when the
    /// device offers none, answered "{}", or did not answer in time. Bytes
    /// only: ImprovWire.parseClaim reads them.
    func readClaim(timeout: Duration = .seconds(10)) async -> Data? {
        guard let claim else { return nil }
        generation += 1
        let gen = generation
        return await withCheckedContinuation { cont in
            claimWaiter = cont
            peripheral.readValue(for: claim)
            Task { [weak self] in
                try? await Task.sleep(for: timeout)
                guard let self, self.generation == gen, let w = self.claimWaiter else { return }
                self.claimWaiter = nil
                w.resume(returning: nil)
            }
        }
    }

    var canIdentify: Bool { capabilities & ImprovWire.capIdentify != 0 }
    var canScanWiFi: Bool { capabilities & ImprovWire.capScanWiFi != 0 }

    // MARK: - resolution

    private func fail(_ why: String) {
        phase = .failed(why)
        if let w = readyWaiter { readyWaiter = nil; w.resume(returning: false) }
        if let w = scanWaiter { scanWaiter = nil; w.resume(returning: networks) }
        if let w = joinWaiter { joinWaiter = nil; w.resume(returning: .failed(why)) }
        if let w = claimWaiter { claimWaiter = nil; w.resume(returning: nil) }
        if let w = hubWaiter { hubWaiter = nil; w.resume(returning: .unknown) }
    }

    private func resolveJoin(_ outcome: Outcome) {
        guard let w = joinWaiter else { return }
        joinWaiter = nil
        switch outcome {
        case .joined(let urls): phase = .joined(urls)
        case .failed(let why): phase = .failed(why)
        }
        w.resume(returning: outcome)
    }
}

extension ImprovClient: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        let services = peripheral.services ?? []
        guard let service = services.first(where: { $0.uuid == Self.serviceUUID }) else {
            fail("This Canary's firmware has no Bluetooth setup door — use its setup network instead.")
            return
        }
        peripheral.discoverCharacteristics(
            [Self.stateUUID, Self.errorUUID, Self.commandUUID, Self.resultUUID, Self.capabilitiesUUID],
            for: service)
        // The companion service is optional: its absence fails nothing, and
        // readiness (the state read below) never waits for it. Which of its
        // two characteristics a Canary serves says what it is for — CLAIM
        // on a WAP, HUB on a Sense or Vision.
        if let companion = services.first(where: { $0.uuid == Self.companionServiceUUID }) {
            peripheral.discoverCharacteristics([Self.claimUUID, Self.hubUUID], for: companion)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        let chars = service.characteristics ?? []
        if service.uuid == Self.companionServiceUUID {
            self.claim = chars.first { $0.uuid == Self.claimUUID }
            self.hub = chars.first { $0.uuid == Self.hubUUID }
            return
        }
        guard service.uuid == Self.serviceUUID else { return }
        // `error` the parameter shadows `error` the characteristic here.
        self.state = chars.first { $0.uuid == Self.stateUUID }
        self.error = chars.first { $0.uuid == Self.errorUUID }
        self.command = chars.first { $0.uuid == Self.commandUUID }
        self.result = chars.first { $0.uuid == Self.resultUUID }
        guard let state = self.state, let errorChar = self.error, let result = self.result, command != nil else {
            fail("This Canary's Bluetooth setup door is missing its controls.")
            return
        }
        // Subscribe before anything is asked, so no answer races its
        // listener; the state read that follows is what marks "ready".
        peripheral.setNotifyValue(true, for: state)
        peripheral.setNotifyValue(true, for: errorChar)
        peripheral.setNotifyValue(true, for: result)
        if let caps = chars.first(where: { $0.uuid == Self.capabilitiesUUID }) {
            peripheral.readValue(for: caps)
        }
        peripheral.readValue(for: state)
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if characteristic.uuid == Self.hubUUID {
            // The hub write: landed → read the Canary's answer; refused by
            // the link → no answer is the honest one.
            if error == nil {
                pairingDeclined = false
                linkEncrypted = true
                if let hub { peripheral.readValue(for: hub) }
            } else if let w = hubWaiter {
                hubWaiter = nil
                w.resume(returning: .unknown)
            }
            return
        }
        guard characteristic.uuid == Self.commandUUID else { return }
        guard let error else {
            pairingDeclined = false   // a write landed: the link is encrypted now
            linkEncrypted = true
            return
        }
        // The pairing sheet was declined, or the Canary refused the write.
        let why: String
        if let cb = error as? CBATTError,
           cb.code == .insufficientEncryption || cb.code == .insufficientAuthentication {
            pairingDeclined = true
            why = "Bluetooth pairing was declined — tap Pair when iOS asks, so the credentials cross encrypted."
        } else {
            why = "The Canary refused the request — \(error.localizedDescription)"
        }
        if let w = scanWaiter {
            scanWaiter = nil
            if phase == .scanning { phase = .ready }
            w.resume(returning: [])
            return
        }
        if phase == .sending {
            resolveJoin(.failed(why))
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if characteristic.uuid == Self.claimUUID, error != nil, let w = claimWaiter {
            claimWaiter = nil
            w.resume(returning: nil)
            return
        }
        if characteristic.uuid == Self.hubUUID, error != nil, let w = hubWaiter {
            hubWaiter = nil
            w.resume(returning: .unknown)
            return
        }
        guard let data = characteristic.value, error == nil else { return }
        switch characteristic.uuid {
        case Self.capabilitiesUUID:
            capabilities = data.first ?? 0

        case Self.claimUUID:
            if let w = claimWaiter {
                claimWaiter = nil
                // "{}" is the door's "not yours" — nil to the caller.
                w.resume(returning: data.count > 2 ? data : nil)
            }

        case Self.hubUUID:
            if let w = hubWaiter {
                hubWaiter = nil
                w.resume(returning: ImprovWire.parseHubStatus(data))
            }

        case Self.stateUUID:
            guard let s = ImprovWire.parseState(data) else { return }
            if phase == .discovering {
                phase = .ready
                if let w = readyWaiter { readyWaiter = nil; w.resume(returning: true) }
            }
            if phase == .sending, s == .provisioning { phase = .joining }
            if phase == .joining || phase == .sending, s == .provisioned {
                // The verdict; the URLs arrive on the result characteristic,
                // usually a beat later — give them that beat.
                Task { [weak self] in
                    try? await Task.sleep(for: .milliseconds(800))
                    guard let self else { return }
                    if self.joinWaiter != nil { self.resolveJoin(.joined(self.lastURLs)) }
                }
            }

        case Self.errorUUID:
            guard let e = ImprovWire.parseError(data) else { return }
            lastError = e
            if e != .none, phase == .sending || phase == .joining {
                resolveJoin(.failed(e.message))
            }

        case Self.resultUUID:
            guard let r = ImprovWire.parseResult(data) else { return }
            switch r.command {
            case ImprovWire.Command.wifiNetworks.rawValue:
                if r.strings.isEmpty {
                    // The empty result closes the list.
                    if let w = scanWaiter {
                        scanWaiter = nil
                        if phase == .scanning { phase = .ready }
                        w.resume(returning: networks)
                    }
                } else if let n = ImprovWire.network(from: r.strings),
                          !networks.contains(where: { $0.ssid == n.ssid }) {
                    networks.append(n)
                }
            case ImprovWire.Command.wifiSettings.rawValue:
                lastURLs = r.strings.filter { !$0.isEmpty }
                if joinWaiter != nil { resolveJoin(.joined(lastURLs)) }
            case ImprovWire.Command.deviceInfo.rawValue:
                deviceInfo = r.strings
            default:
                break
            }
        default:
            break
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        // Nothing to do: a refused subscription surfaces as a missing answer,
        // which the timeouts turn into honest words.
    }
}
