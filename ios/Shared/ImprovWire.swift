// ImprovWire.swift  (SHARED — pure Foundation)
//
// The app-side twin of `firmware/common/network/improv_core.h`: Improv Wi-Fi
// over BLE, the open standard a brand-new Canary speaks so this phone can
// hand it Wi-Fi with one tap. PURE by design — no CoreBluetooth, no SwiftUI —
// so `ImprovWireTests` can pin every byte against the firmware's host test
// (`firmware/tests_host/test_improv_core.cpp`); the transport glue
// (ImprovClient) sits on top and only moves bytes.
//
// THE WIRE (the standard's, restated once):
//
//   Service        00467768-6228-2272-4663-277478268000
//   Current state  ...8001  READ | NOTIFY   one byte, `State`
//   Error state    ...8002  READ | NOTIFY   one byte, `Error`
//   RPC command    ...8003  WRITE           what the phone asks, framed below
//   RPC result     ...8004  READ | NOTIFY   what the device answers
//   Capabilities   ...8005  READ            one byte, capability bits
//
//   Advert service data under the 16-bit UUID 0x4677: [state][caps][0 0 0 0].
//   The device puts it in the same advertisement as the 128-bit service UUID,
//   so a filtered scan finds a Canary with its door open and nothing else.
//
//   Command  [cmd][data_len][data…][checksum]       checksum = sum & 0xFF
//   Wi-Fi settings data: [ssid_len][ssid][pass_len][password]
//   Result   [cmd][data_len][len₁][str₁]…[lenₙ][strₙ][checksum]
//
//   A Wi-Fi settings result carries URLs the device can now be reached at;
//   a networks scan is one result per network (ssid, rssi, auth), closed by
//   an empty result; device info is four strings (firmware name, version,
//   hardware, device name).

import Foundation

enum ImprovWire {
    // ── UUIDs (strings here; the transport makes CBUUIDs of them) ──
    static let serviceUUID       = "00467768-6228-2272-4663-277478268000"
    static let currentStateUUID  = "00467768-6228-2272-4663-277478268001"
    static let errorStateUUID    = "00467768-6228-2272-4663-277478268002"
    static let rpcCommandUUID    = "00467768-6228-2272-4663-277478268003"
    static let rpcResultUUID     = "00467768-6228-2272-4663-277478268004"
    static let capabilitiesUUID  = "00467768-6228-2272-4663-277478268005"
    /// The 16-bit UUID the advert's service data rides under.
    static let serviceDataUUID16: UInt16 = 0x4677

    static let maxSSIDLength = 32
    static let maxPasswordLength = 64

    // ── capability bits ──
    static let capIdentify: UInt8   = 0x01
    static let capDeviceInfo: UInt8 = 0x02
    static let capScanWiFi: UInt8   = 0x04
    static let capHostname: UInt8   = 0x08

    enum State: UInt8, Sendable {
        case stopped = 0x00
        case awaitingAuthorization = 0x01
        case authorized = 0x02
        case provisioning = 0x03
        case provisioned = 0x04

        /// A device with its door open — the ones the nearby card offers.
        var isOpen: Bool { self == .authorized || self == .provisioning }
    }

    enum Error: UInt8, Sendable {
        case none = 0x00
        case invalidRPC = 0x01
        case unknownRPC = 0x02
        case unableToConnect = 0x03
        case notAuthorized = 0x04
        case badHostname = 0x05
        case unknown = 0xFF

        /// What to tell the person, in the Canary's own terms.
        var message: String {
            switch self {
            case .none: return ""
            case .invalidRPC: return "The Canary didn't understand that request — try again."
            case .unknownRPC: return "This Canary's firmware doesn't know that request."
            case .unableToConnect: return "The Canary couldn't join with those credentials — check the password."
            case .notAuthorized: return "This Canary already has Wi-Fi and isn't accepting a new network. Tap its button to open it up, or use its setup network."
            case .badHostname: return "The Canary refused that name."
            case .unknown: return "The Canary reported an error it couldn't name."
            }
        }
    }

    enum Command: UInt8, Sendable {
        case wifiSettings = 0x01
        case identify = 0x02
        case deviceInfo = 0x03
        case wifiNetworks = 0x04
        case hostname = 0x05
        case deviceName = 0x06
        case networkState = 0x07
    }

    /// The low 8 bits of the byte sum — the standard's trailing byte.
    static func checksum<C: Collection>(_ bytes: C) -> UInt8 where C.Element == UInt8 {
        UInt8(truncatingIfNeeded: bytes.reduce(0) { $0 &+ UInt32($1) })
    }

    // MARK: - commands (phone → Canary)

    /// Frame one command. Nil when the data is over the one-byte length.
    static func command(_ cmd: Command, data: [UInt8] = []) -> Data? {
        guard data.count <= 255 else { return nil }
        var out: [UInt8] = [cmd.rawValue, UInt8(data.count)]
        out.append(contentsOf: data)
        out.append(checksum(out))
        return Data(out)
    }

    /// The Wi-Fi settings command. Nil for an empty SSID or a credential over
    /// WPA2's bounds — refused here rather than truncated on the device.
    static func wifiSettings(ssid: String, password: String) -> Data? {
        let s = Array(ssid.utf8), p = Array(password.utf8)
        guard !s.isEmpty, s.count <= maxSSIDLength, p.count <= maxPasswordLength else { return nil }
        guard !s.contains(0), !p.contains(0) else { return nil }
        var body: [UInt8] = [UInt8(s.count)]
        body.append(contentsOf: s)
        body.append(UInt8(p.count))
        body.append(contentsOf: p)
        return command(.wifiSettings, data: body)
    }

    static var identify: Data { command(.identify)! }
    static var deviceInfo: Data { command(.deviceInfo)! }
    static var wifiNetworks: Data { command(.wifiNetworks)! }

    // MARK: - results (Canary → phone)

    struct Result: Equatable, Sendable {
        /// The command this answers — raw, so a future command still decodes.
        var command: UInt8
        var strings: [String]
    }

    /// Decode one result frame; nil when the bytes are not a result (short,
    /// a length that disagrees, a bad checksum, an inner string past the end).
    static func parseResult(_ data: Data) -> Result? {
        let b = [UInt8](data)
        guard b.count >= 3, Int(b[1]) == b.count - 3 else { return nil }
        guard checksum(b[..<(b.count - 1)]) == b[b.count - 1] else { return nil }
        var strings: [String] = []
        var pos = 2
        let end = b.count - 1
        while pos < end {
            let len = Int(b[pos]); pos += 1
            guard pos + len <= end else { return nil }
            strings.append(String(decoding: b[pos..<(pos + len)], as: UTF8.self))
            pos += len
        }
        return Result(command: b[0], strings: strings)
    }

    /// One row of a networks scan: three strings, rssi as decimal text.
    struct Network: Equatable, Sendable {
        var ssid: String
        var rssi: Int
        var auth: String
        /// The standard's auth names for an open network.
        var isOpen: Bool { auth.uppercased() == "NO" || auth.uppercased() == "OPEN" }
    }

    static func network(from strings: [String]) -> Network? {
        guard strings.count >= 3, !strings[0].isEmpty, let rssi = Int(strings[1]) else { return nil }
        return Network(ssid: strings[0], rssi: rssi, auth: strings[2])
    }

    // MARK: - the one-byte characteristics and the advert

    static func parseState(_ data: Data) -> State? {
        guard let first = data.first else { return nil }
        return State(rawValue: first)
    }

    static func parseError(_ data: Data) -> Error? {
        guard let first = data.first else { return nil }
        return Error(rawValue: first)
    }

    struct ServiceData: Equatable, Sendable {
        var state: State
        var capabilities: UInt8
        var canIdentify: Bool { capabilities & ImprovWire.capIdentify != 0 }
        var canScanWiFi: Bool { capabilities & ImprovWire.capScanWiFi != 0 }
    }

    /// The advert's 0x4677 service data: [state][caps][reserved…]. Nil when
    /// shorter than two bytes or the state byte is not one of the standard's.
    static func parseServiceData(_ data: Data) -> ServiceData? {
        let b = [UInt8](data)
        guard b.count >= 2, let state = State(rawValue: b[0]) else { return nil }
        return ServiceData(state: state, capabilities: b[1])
    }
}
