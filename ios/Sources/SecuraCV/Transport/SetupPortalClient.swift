// SetupPortalClient.swift
//
// The hands for SetupPortal: join a Canary's setup network from inside the
// app, ask the Canary which networks it can see, hand its captive page the
// home Wi-Fi the person picked, and watch the device's own verdict. The one place the app uses NetworkExtension — NEHotspotConfiguration
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
        case listing              // asking the Canary which networks it can see
        case ready                // on its setup network, its list in hand: pick and Join
        case posting              // sending the home Wi-Fi
        case waiting(SetupPortal.JoinState)
        case done
        case failed(String)
    }

    @Published private(set) var phase: Phase = .idle
    /// What the Canary's own radio heard (`GET /scan`), strongest first —
    /// empty until listed, or when it heard nothing it could show.
    @Published private(set) var networks: [SetupPortal.Network] = []
    /// The setup network this client asked iOS to join, so `finish()` can
    /// forget exactly that one.
    private var joinedSSID: String?

    /// The phone is on the Canary's setup network and its page answered —
    /// so `hand(...)` (or a retry of it after a typo) needs no new prompt.
    @Published private(set) var onSetupNetwork = false

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

    /// Step one: join the Canary's setup network (iOS asks once), wait for
    /// its page, and ask it which networks it can see — so the person
    /// picks from the Canary's own list instead of typing a name it may
    /// not be able to hear. Ends in `.ready` (list in `networks`, possibly
    /// empty: the form then takes a typed name) or a named failure.
    func connect(setupSSID: String, setupKey: String) async {
        onSetupNetwork = false
        networks = []
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
        joinedSSID = setupSSID

        phase = .reachingPortal
        guard await waitForPortal(seconds: 25) else {
            phase = .failed("Joined \(setupSSID), but the Canary's setup page didn't answer. Move closer, or power the Canary off and on and try again.")
            return
        }
        onSetupNetwork = true

        phase = .listing
        networks = await scan(seconds: 10)
        phase = .ready
    }

    /// Step two: hand over the home Wi-Fi and watch the device's own
    /// verdict. Safe to call again after a failure (a typo, a 5 GHz name):
    /// the phone is still on the setup network, so a retry costs no prompt.
    func hand(homeSSID: String, homePassword: String,
              timeZone: String? = TimeZone.current.identifier) async {
        if let problem = SetupPortal.credentialProblem(ssid: homeSSID, password: homePassword) {
            phase = .failed(problem)
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
                phase = .failed(SetupPortal.advice(for: reply.reason ?? "The Canary refused the credentials."))
                return
            }
        } catch {
            phase = .failed("The Canary's setup page didn't take the credentials: \(error.localizedDescription)")
            return
        }

        // The device's own verdict. It keeps its setup network up until the
        // phone has seen the answer, then takes it down — so a "success"
        // read here is the join, confirmed at the source.
        let deadline = Date().addingTimeInterval(90)
        while Date() < deadline {
            if let state = await status() {
                phase = .waiting(state)
                switch state {
                case .success:
                    phase = .done
                    finish()
                    return
                case .fail(let reason):
                    phase = .failed(SetupPortal.advice(for: reason))
                    return
                case .idle, .connecting: break
                }
            }
            try? await Task.sleep(for: .seconds(2))
        }
        phase = .failed("No verdict from the Canary in time. If it joined, it appears on the Fleet tab shortly; otherwise power it off and on and try again.")
    }

    /// Let the setup network go (done, abandoned, or the screen closed).
    /// `joinOnce` already forgets it when the phone leaves; this makes the
    /// phone leave now, back to the home Wi-Fi.
    func finish() {
        if let ssid = joinedSSID {
            NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: ssid)
        }
        joinedSSID = nil
        onSetupNetwork = false
    }

    /// Poll `GET /scan` until the Canary stops answering "scanning" (it
    /// sweeps asynchronously, ~1-3 s) or the time runs out. Empty on any
    /// failure: the form then takes a typed name, as it always did.
    private func scan(seconds: Int) async -> [SetupPortal.Network] {
        let deadline = Date().addingTimeInterval(TimeInterval(seconds))
        while Date() < deadline {
            var request = URLRequest(url: SetupPortal.baseURL.appendingPathComponent("scan"))
            request.cachePolicy = .reloadIgnoringLocalAndRemoteCacheData
            if let reply = try? await session.data(for: request),
               let parsed = SetupPortal.parseScan(reply.0) {
                if !parsed.scanning { return parsed.networks }
            }
            try? await Task.sleep(for: .milliseconds(900))
        }
        return []
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
