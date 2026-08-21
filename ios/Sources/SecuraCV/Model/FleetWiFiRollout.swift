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
// Three transports, matched to what each device can actually do right now:
//   * HTTP — a WAP-class Canary that is answering on the LAN. The normal
//     path when you're migrating ahead of a router change.
//   * BLE — a WAP-class Canary that has gone dark on Wi-Fi (the password
//     already changed under it) but is in Bluetooth range: the firmware's
//     bonded provisioning service exists for exactly this rescue.
//   * Hands-on — the display family stores credentials only through its
//     first-boot portal; the plan says so up front instead of pretending.

import Foundation

enum FleetWiFiRollout {
    /// How the new credentials can reach one device right now.
    enum Path: Hashable, Sendable {
        case http        // answering on the LAN — push over /api/wifi/connect
        case ble         // dark on Wi-Fi, heard over BLE — bonded provisioning write
        case handsOn     // no runtime credential path (the display family)
        case unreachable // updatable in principle; no path to it right now
    }

    struct Candidate: Identifiable, Hashable, Sendable {
        var id: String
        var name: String
        /// WAP-class, paired, token held — the runtime credential surface exists.
        var updatable: Bool
        /// Answering over HTTP on the LAN right now.
        var online: Bool
        /// Heard over BLE right now (console connected) — the rescue path.
        var bleReachable: Bool
        var rssiDBM: Int?

        var path: Path {
            guard updatable else { return .handsOn }
            if online { return .http }
            if bleReachable { return .ble }
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
    }

    /// Build the staged plan. The pilot is the healthiest HTTP-reachable
    /// Canary (strongest signal first — the device MOST likely to rejoin
    /// fast, so the proof arrives fast); if nothing answers over HTTP the
    /// pilot comes from the BLE lane. Followers keep the same
    /// strongest-first order so the fleet moves sturdiest-to-shakiest.
    static func plan(_ candidates: [Candidate]) -> Plan {
        let byStrength: (Candidate, Candidate) -> Bool = { a, b in
            switch (a.rssiDBM, b.rssiDBM) {
            case let (x?, y?): return x > y
            case (_?, nil): return true
            case (nil, _?): return false
            case (nil, nil): return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
            }
        }
        let http = candidates.filter { $0.path == .http }.sorted(by: byStrength)
        let ble = candidates.filter { $0.path == .ble }.sorted(by: byStrength)
        let pilot = http.first ?? ble.first
        let followers = (http + ble).filter { $0.id != pilot?.id }
        return Plan(pilot: pilot,
                    followers: followers,
                    handsOn: candidates.filter { $0.path == .handsOn },
                    unreachable: candidates.filter { $0.path == .unreachable })
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
}
