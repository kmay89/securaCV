// FleetWiFiRollout.swift
//
// "The router password changed — update every Canary" as a staged, honest,
// impossible-to-strand-the-fleet plan. Pure policy: this file decides WHO
// gets the new credentials, in WHAT order, over WHICH path, and what each
// outcome means. The transports (HTTP /api/wifi/connect, the BLE
// provisioning service) and the UI live elsewhere and just carry it out.
//
// The one safety rule everything here serves: **never fan out an unproven
// credential.** A typo'd password pushed to the whole fleet at once turns
// every Canary into a hands-on recovery job. So the plan always starts with
// a single PILOT — the healthiest reachable Canary — and the rest of the
// fleet is not touched until the pilot has actually come back on the new
// network. A wrong password strands one device (which the BLE provisioning
// service and the BOOT-button portal can still rescue), never the fleet.
//
// The second rule is about what the password crosses on its way (roadmap
// row 13): the Bluetooth lane is bonded — encrypted end to end — and an
// https Canary pinned to its receipt's certificate is too, but a plain-http
// push puts the router password on the LAN in the clear. So a device that
// can be reached over the bonded lane takes it even when it is answering
// over plain http, plain http is offered only when nothing better exists,
// and then only behind a disclosure the user has to acknowledge. The sheet
// used to send it in the clear and call the app the safe path.
//
// Four transports, matched to what each device can actually do right now:
//   * HTTP (pinned TLS) — a WAP-class Canary answering over https whose
//     certificate fingerprint came with its pairing receipt. Encrypted and
//     checked (DeviceAPI pins it); the normal fast path.
//   * BLE — a WAP-class Canary heard over its bonded provisioning service:
//     the rescue for one that has gone dark on Wi-Fi, and the preferred lane
//     for one that is online but not pinned.
//   * HTTP (cleartext) — a WAP-class Canary answering over plain http with
//     no bonded lane in range. Works, but the password crosses the LAN
//     unencrypted; the plan says so and asks first.
//   * Hands-on — the display family stores credentials only through its
//     first-boot portal; the plan says so up front instead of pretending.

import Foundation

enum FleetWiFiRollout {
    /// How the new credentials can reach one device right now.
    enum Path: Hashable, Sendable {
        case http          // answering over pinned https — push over /api/wifi/connect, encrypted
        case httpCleartext // answering over plain http only — same push, password in the clear; needs the disclosure
        case ble           // heard over the bonded provisioning service — encrypted write
        case handsOn       // no runtime credential path (the display family)
        case unreachable   // updatable in principle; no path to it right now

        /// The two HTTP lanes share the transport; only the wire differs.
        var isHTTP: Bool { self == .http || self == .httpCleartext }
    }

    struct Candidate: Identifiable, Hashable, Sendable {
        var id: String
        var name: String
        /// WAP-class, paired, token held — the runtime credential surface exists.
        var updatable: Bool
        /// Answering over HTTP on the LAN right now.
        var online: Bool
        /// Heard over BLE right now (console connected) — the bonded lane.
        var bleReachable: Bool
        var rssiDBM: Int?
        /// Its base URL is https AND its pairing receipt carried the
        /// certificate fingerprint DeviceAPI pins — the HTTP lane is
        /// encrypted and checked. Defaults false: a plain-http device is the
        /// common case and must never be mistaken for a pinned one.
        var tlsPinned: Bool = false

        var path: Path {
            guard updatable else { return .handsOn }
            if online && tlsPinned { return .http }
            // Bonded beats cleartext even when the device is answering on
            // the LAN: an online-but-unpinned Canary in Bluetooth range takes
            // the encrypted lane, not the one that leaks the password.
            if bleReachable { return .ble }
            if online { return .httpCleartext }
            return .unreachable
        }
    }

    /// The staged plan. `pilot` proves the credentials; `followers` wait for
    /// that proof; `handsOn` and `unreachable` are named honestly so nobody
    /// closes the sheet believing the whole fleet moved.
    struct Plan: Hashable, Sendable {
        var pilot: Candidate?
        var followers: [Candidate]
        var handsOn: [Candidate]
        var unreachable: [Candidate]

        /// Everything the rollout will actually push to.
        var pushTargets: [Candidate] {
            (pilot.map { [$0] } ?? []) + followers
        }

        /// The push targets whose password would cross the LAN unencrypted.
        var cleartextTargets: [Candidate] {
            pushTargets.filter { $0.path == .httpCleartext }
        }

        /// Does running this plan need the one-time cleartext disclosure
        /// acknowledged first? True whenever any push would ride plain http.
        var needsCleartextDisclosure: Bool { !cleartextTargets.isEmpty }
    }

    /// Build the staged plan. The pilot is the healthiest Canary on the
    /// fastest ENCRYPTED lane: pinned https first (proof in seconds, nothing
    /// in the clear), then plain http (still seconds, but the password is
    /// exposed — so only when no pinned device can prove it), then the
    /// bonded BLE lane (a bond ceremony can take a minute). Strongest signal
    /// first within a lane — the device MOST likely to rejoin fast, so the
    /// proof arrives fast. Followers keep the same lane-then-strength order
    /// so the fleet moves sturdiest-to-shakiest.
    static func plan(_ candidates: [Candidate]) -> Plan {
        let byStrength: (Candidate, Candidate) -> Bool = { a, b in
            switch (a.rssiDBM, b.rssiDBM) {
            case let (x?, y?): return x > y
            case (_?, nil): return true
            case (nil, _?): return false
            case (nil, nil): return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
            }
        }
        let pinned = candidates.filter { $0.path == .http }.sorted(by: byStrength)
        let cleartext = candidates.filter { $0.path == .httpCleartext }.sorted(by: byStrength)
        let ble = candidates.filter { $0.path == .ble }.sorted(by: byStrength)
        let pilot = pinned.first ?? cleartext.first ?? ble.first
        let followers = (pinned + cleartext + ble).filter { $0.id != pilot?.id }
        return Plan(pilot: pilot,
                    followers: followers,
                    handsOn: candidates.filter { $0.path == .handsOn },
                    unreachable: candidates.filter { $0.path == .unreachable })
    }

    /// Is this device's HTTP lane encrypted AND checked? Only an https base
    /// whose receipt fingerprint parses as a pin counts — the same rule
    /// DeviceAPI applies before it will dial an https Canary at all.
    static func isPinnedTLS(url: URL?, fingerprint: String?) -> Bool {
        guard let url, DeviceAPI.isTLS(url) else { return false }
        return TLSPin.normalize(fingerprint) != nil
    }

    // MARK: - credentials

    /// Mirror of the firmware's own validation (handle_wifi_connect and the
    /// BLE provisioning service enforce the same WPA2 bounds) — catching it
    /// here means catching it before the pilot, not after.
    static func credentialProblem(ssid: String, password: String) -> String? {
        let s = ssid.trimmingCharacters(in: .whitespacesAndNewlines)
        if s.isEmpty { return "Enter the network's name." }
        if s.utf8.count > 32 { return "A Wi-Fi name is at most 32 characters." }
        if password.utf8.count > 64 { return "A Wi-Fi password is at most 64 characters." }
        return nil
    }

    // MARK: - the cleartext disclosure

    /// Shown once, above the plan, whenever a push would ride plain http.
    /// It names the exposure plainly and the two ways out of it.
    static let cleartextDisclosure =
        "Some of these Canaries can only take the new password over plain "
        + "HTTP right now, which sends it across your Wi-Fi unencrypted — "
        + "anyone already on this network could read it while it's in "
        + "flight. Their encrypted lanes aren't available: Bluetooth needs "
        + "you within a room or two, and a secure (https) connection needs "
        + "a pairing receipt that carried the Canary's certificate."

    /// The acknowledgment the user has to switch on before such a push runs.
    static let cleartextAcknowledgment = "Send it over plain HTTP anyway"

    /// The verdict for a cleartext target the user did not approve.
    static let cleartextDeclined =
        "Not sent — sending the password over unencrypted Wi-Fi wasn't "
        + "approved. Move within Bluetooth range for its encrypted lane, or "
        + "re-pair it with a secure (https) receipt, then try again."

    /// The per-row note for a cleartext lane, before and during the run.
    static let cleartextNote = "Plain HTTP — the password crosses your Wi-Fi unencrypted"

    // MARK: - the per-device story

    /// One device's progress through the rollout.
    enum StepState: Hashable, Sendable {
        case waiting                 // in line behind the pilot's proof
        case sending                 // credentials on their way
        case confirming              // sent; watching for it to come back
        case moved                   // answered on the network again — proven
        case failed(String)          // didn't, and here is exactly why
        case handsOn                 // needs its own portal; never pushed
    }

    /// How long a device gets to rejoin before the rollout stops waiting.
    /// Generous on purpose: DHCP + mDNS re-registration on a cheap board is
    /// slow, and a false "failed" here would send someone to rescue a Canary
    /// that was fine.
    static let returnWindow: TimeInterval = 90

    /// The verdict once the return window closes without an answer. The
    /// wording carries the recovery path — a dead end with no next step is
    /// the one sentence this sheet must never show.
    static let didNotReturn =
        "Didn't answer on the new network within 90 seconds. It may still be "
        + "rejoining — pull down to check again in a moment. If the password "
        + "was wrong, this Canary's Bluetooth rescue or its setup portal "
        + "(hold BOOT) will take the correction."

    /// Why the fleet is still waiting, said plainly on every follower row.
    static let waitingReason = "Waiting for the first Canary to prove the password."

    /// May the followers be pushed yet? Only a pilot that actually came
    /// back proves the credentials. No pilot means nothing was pushable —
    /// there is nobody to fan out to, so the answer is moot but safe.
    static func mayFanOut(pilotState: StepState?) -> Bool {
        pilotState == .moved
    }

    /// May THIS push go out? Every lane but cleartext always may; cleartext
    /// only once the disclosure was acknowledged. The runner asks before
    /// every push, so a plan built before the toggle cannot slip one past.
    static func mayPush(_ path: Path, cleartextApproved: Bool) -> Bool {
        path != .httpCleartext || cleartextApproved
    }
}
