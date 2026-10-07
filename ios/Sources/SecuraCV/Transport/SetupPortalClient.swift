// SetupPortalClient.swift
//
// The hands for SetupPortal: join a Canary's setup network from inside the
// app, hand its captive page the home Wi-Fi, and watch the device's own
// verdict. The one place the app uses NetworkExtension — NEHotspotConfiguration
// is the system's own "join this network for me" door (iOS asks once), and
// `joinOnce` means the phone forgets the setup network the moment it leaves,
// which it does on its own when the Canary tears the network down.
//
// What is checked, and how: the portal's `/status` is the device saying
// "connected" after it actually got an address on the home network — that
// is the verdict shown, never "the request was accepted". A wrong password
// comes back as the firmware's own reason.

import Foundation
import NetworkExtension

@MainActor
final class SetupPortalClient: ObservableObject {
    enum Phase: Equatable {
        case idle
        case joining              // asking iOS to join SecuraCV-XXXX
        case reachingPortal       // on it; waiting for 192.168.4.1 to answer
        case posting              // sending the home Wi-Fi
        case waiting(SetupPortal.JoinState)
        case done
        case failed(String)
    }

    @Published private(set) var phase: Phase = .idle

    /// A session pinned to the Wi-Fi interface: on a captive network with
    /// no internet iOS would otherwise route around it over cellular, and
    /// 192.168.4.1 would answer from somewhere else or not at all.
    private let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.allowsCellularAccess = false
        config.waitsForConnectivity = false
        config.timeoutIntervalForRequest = 6
        config.timeoutIntervalForResource = 10
        return URLSession(configuration: config)
    }()

    /// The whole ceremony. Returns on the device's verdict or a named
    /// failure; `phase` narrates along the way.
    func provision(setupSSID: String, setupKey: String, homeSSID: String, homePassword: String,
                   timeZone: String? = TimeZone.current.identifier) async {
        if let problem = SetupPortal.credentialProblem(ssid: homeSSID, password: homePassword) {
            phase = .failed(problem)
            return
        }
        phase = .joining
        let config = NEHotspotConfiguration(ssid: setupSSID, passphrase: setupKey, isWEP: false)
        config.joinOnce = true
        do {
            try await NEHotspotConfigurationManager.shared.apply(config)
        } catch let error as NSError {
            // "already associated" is iOS saying it is already on that
            // network — fine. Anything else is the join refused.
            if error.domain != NEHotspotConfigurationErrorDomain
                || error.code != NEHotspotConfigurationError.alreadyAssociated.rawValue {
                phase = .failed("iOS couldn't join \(setupSSID): \(error.localizedDescription) Check the key, and that the Canary is powered on and nearby.")
                return
            }
        }
        defer { NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: setupSSID) }

        phase = .reachingPortal
        guard await waitForPortal(seconds: 25) else {
            phase = .failed("Joined \(setupSSID), but the Canary's setup page didn't answer. Move closer, or power the Canary off and on and try again.")
            return
        }

        phase = .posting
        var request = URLRequest(url: SetupPortal.baseURL.appendingPathComponent("join"))
        request.httpMethod = "POST"
        request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        request.httpBody = Data(SetupPortal.joinBody(ssid: homeSSID, password: homePassword, timeZone: timeZone).utf8)
        do {
            let (data, _) = try await session.data(for: request)
            let reply = SetupPortal.parseJoinReply(data)
            guard reply.ok else {
                phase = .failed(reply.reason ?? "The Canary refused the credentials.")
                return
            }
        } catch {
            phase = .failed("The Canary's setup page didn't take the credentials: \(error.localizedDescription)")
            return
        }

        // The device's own verdict. It keeps its setup network up until the
        // phone has seen the answer, then takes it down — so a "success"
        // read here is the join, verified at the source.
        let deadline = Date().addingTimeInterval(90)
        while Date() < deadline {
            if let state = await status() {
                phase = .waiting(state)
                switch state {
                case .success: phase = .done; return
                case .fail(let reason): phase = .failed(reason); return
                case .idle, .connecting: break
                }
            }
            try? await Task.sleep(for: .seconds(2))
        }
        phase = .failed("No verdict from the Canary in time. If it joined, it appears on the Fleet tab shortly; otherwise power it off and on and try again.")
    }

    private func waitForPortal(seconds: Int) async -> Bool {
        let deadline = Date().addingTimeInterval(TimeInterval(seconds))
        while Date() < deadline {
            if await status() != nil { return true }
            try? await Task.sleep(for: .seconds(1))
        }
        return false
    }

    private func status() async -> SetupPortal.JoinState? {
        var request = URLRequest(url: SetupPortal.baseURL.appendingPathComponent("status"))
        request.cachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        guard let reply = try? await session.data(for: request) else { return nil }
        return SetupPortal.parseStatus(reply.0)
    }
}
