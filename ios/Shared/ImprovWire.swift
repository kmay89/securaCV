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
//
// THE DOOR (the Canary's rules, not the standard's): a Canary opens the
// door only while it has NO stored Wi-Fi — for half an hour after power-on
// (a power cycle re-arms it) — or for one minute after a short tap on its
// BOOT button (Sense, Vision; a WAP has no tap door yet). A saved network
// that stopped working does NOT open it. A second credentials write within
// 3 s is refused with InvalidRpc; the attempt past ten on one open door
// shuts it (NotAuthorized); a link that sends nothing for three minutes is
// dropped; after a join the Canary drops the link about 20 s later.
//
// SECURACV'S ADDITION, OUTSIDE THE IMPROV SERVICE (a WAP only): the claim
// ticket, `firmware/common/network/claim_ticket.h`. The door's link is
// encrypted but not authenticated (Just Works), so the bearer token never
// rides it. Once the join this phone asked for succeeded, the link that
// asked may read CLAIM exactly once, within 180 s:
//
//   Companion service  8fc1cf00-b162-4401-9607-c8ac21383e90
//   CLAIM              8fc1cf01-…  READ (encrypted)   JSON, below
//
//   {"device_id":"…","claim":"<32 lowercase hex>",
//    "claim_url":"https://canary-ab12.local/api/provisioning-receipt?claim=<hex>",
//    "tls_cert_fp":"<64 hex, or empty>","sta_ip":"192.168.1.23",
//    "mdns_host":"canary-ab12","expires_in_s":180}
//
//   Anyone else, and any later read, gets "{}". The phone then spends the
//   claim ON THE HOME LAN — an unauthenticated GET of claim_url — and gets
//   the same pairing receipt the BOOT-tap route serves. Single use: a wrong
//   guess burns it; a 403 is spent, expired or refused. Sense and Vision
//   serve no companion service (nothing to pair over HTTP).

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

    // ── SecuraCV's companion service (a WAP only; see the header) ──
    /// A separate GATT service beside the Improv one — optional, never
    /// required for the door itself.
    static let companionServiceUUID = "8fc1cf00-b162-4401-9607-c8ac21383e90"
    /// The claim ticket, readable once by the link that provisioned.
    static let claimUUID            = "8fc1cf01-b162-4401-9607-c8ac21383e90"
    /// The firmware spells a claim as 16 random bytes in hex.
    static let claimHexLength = 32

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
            case .invalidRPC: return "The Canary didn't understand that request, or got it too soon after the last one — wait a moment and try again."
            case .unknownRPC: return "This Canary's firmware doesn't know that request."
            case .unableToConnect: return "The Canary couldn't join with those credentials — check the password."
            case .notAuthorized: return "This Canary isn't accepting a new network right now: it already has Wi-Fi, or its setup door closed. Tap its BOOT button once to open it for a minute, or power-cycle a brand-new one; a WAP uses its setup page instead."
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

    /// Hashable, not merely Equatable: NearbyCanaries.Heard (Hashable, it
    /// is a dictionary value the watch widgets compile too) holds one.
    struct ServiceData: Hashable, Sendable {
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

    // MARK: - the claim ticket (SecuraCV's companion service, a WAP only)

    /// What CLAIM answered the link that provisioned: where to spend the
    /// claim, and what the phone needs to dial it.
    struct Claim: Hashable, Sendable {
        var deviceID: String
        /// 32 hex characters, folded to lower case (the firmware's spelling;
        /// its compare folds case too).
        var claim: String
        /// `claim_url` as given — nil when absent, empty, or not an http(s)
        /// URL with a host. Its host is the Canary's `.local` name.
        var claimURL: URL?
        /// `tls_cert_fp`: the SHA-256 of the Canary's certificate, or nil
        /// when the field was empty (an http WAP). An https claim URL with
        /// no pin is never dialed (MagicPairPlan).
        var tlsCertFingerprint: String?
        /// `sta_ip`: the address it got from your router — the fallback
        /// host when its `.local` name does not resolve yet.
        var staIP: String?
        var mdnsHost: String?
        var expiresIn: Int?
    }

    /// Decode one CLAIM read. Nil for "{}" (the door's "not yours"), for a
    /// missing or malformed `claim` (exactly 32 hex characters, either
    /// case), and for a missing `device_id`. Tolerant of extra keys.
    static func parseClaim(_ data: Data) -> Claim? {
        guard let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return nil }
        func str(_ key: String) -> String? {
            guard let s = obj[key] as? String else { return nil }
            let trimmed = s.trimmingCharacters(in: .whitespacesAndNewlines)
            return trimmed.isEmpty ? nil : trimmed
        }
        guard let deviceID = str("device_id"), let rawClaim = str("claim") else { return nil }
        let claim = rawClaim.lowercased()
        guard claim.count == claimHexLength,
              claim.allSatisfy({ $0.isASCII && $0.isHexDigit }) else { return nil }
        var claimURL: URL?
        if let s = str("claim_url"), let url = URL(string: s),
           let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https",
           let host = url.host, !host.isEmpty {
            claimURL = url
        }
        let expires: Int?
        if let n = obj["expires_in_s"] as? Int { expires = n }
        else if let d = obj["expires_in_s"] as? Double { expires = Int(d) }
        else { expires = nil }
        return Claim(deviceID: deviceID,
                     claim: claim,
                     claimURL: claimURL,
                     tlsCertFingerprint: str("tls_cert_fp"),
                     staIP: str("sta_ip"),
                     mdnsHost: str("mdns_host"),
                     expiresIn: expires)
    }

    /// The claim URL with its host swapped — the `sta_ip` fallback when the
    /// `.local` name will not resolve — keeping scheme, port, path and
    /// query. Nil when the claim carries no URL or the host cannot be set.
    /// A nil or empty host gives the claim URL back as it is.
    static func claimURL(_ claim: Claim, host newHost: String?) -> URL? {
        guard let url = claim.claimURL else { return nil }
        guard let newHost = newHost?.trimmingCharacters(in: .whitespacesAndNewlines), !newHost.isEmpty else {
            return url
        }
        guard var comps = URLComponents(url: url, resolvingAgainstBaseURL: false) else { return nil }
        comps.host = newHost
        guard let out = comps.url, out.host?.isEmpty == false else { return nil }
        return out
    }
}
