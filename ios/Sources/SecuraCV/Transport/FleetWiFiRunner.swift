// FleetWiFiRunner.swift
//
// Carries out a FleetWiFiRollout plan. The POLICY (pilot-first, who gets
// which transport, what the verdicts mean, when plain http may be used)
// lives in FleetWiFiRollout and is host-tested; this class is the pair of
// hands: it posts the HTTP writes, asks BLEConsole for the Bluetooth
// rescue, watches for each device to answer on the network again, and
// publishes per-device progress the sheet renders live.
//
// Two rules it enforces at runtime, both the plan's: the followers are not
// touched until the pilot has PROVEN the credentials by answering on the
// network again (a wrong password costs one Canary a rescue, never the
// fleet), and a cleartext push goes out only if the user acknowledged the
// disclosure — otherwise that device gets an honest "not sent", whatever
// the plan said.

import Foundation

@MainActor
final class FleetWiFiRunner: ObservableObject {
    @Published private(set) var steps: [String: FleetWiFiRollout.StepState] = [:]
    @Published private(set) var running = false
    @Published private(set) var finished = false

    private let devices: DeviceStore
    private let ble: BLEConsole
    /// Whether the user switched on the cleartext acknowledgment for THIS
    /// run. Read before every push (FleetWiFiRollout.mayPush).
    private var cleartextApproved = false

    init(devices: DeviceStore, ble: BLEConsole) {
        self.devices = devices
        self.ble = ble
    }

    func state(for id: String) -> FleetWiFiRollout.StepState {
        steps[id] ?? .waiting
    }

    /// How many targets actually moved — the sheet's closing sentence.
    var movedCount: Int {
        steps.values.filter { $0 == .moved }.count
    }

    /// Run the whole staged plan. Returns when every push target has a
    /// final verdict. `cleartextApproved` is the disclosure toggle: false
    /// leaves every plain-http target untouched with the reason on its row.
    func run(plan: FleetWiFiRollout.Plan, ssid: String, password: String,
             cleartextApproved: Bool = false) async {
        guard !running else { return }
        running = true
        finished = false
        self.cleartextApproved = cleartextApproved
        defer { running = false; finished = true }

        // The plain-http lanes wait on the disclosure; the rest do not wait
        // on them. Without approval the cleartext targets are set aside with
        // the reason on their row and the plan is re-staged around an
        // encrypted pilot, so one unapproved Canary never strands the fleet.
        var plan = plan
        if !cleartextApproved && plan.needsCleartextDisclosure {
            let (kept, declined) = plan.excludingCleartext()
            for c in declined { steps[c.id] = .failed(FleetWiFiRollout.cleartextDeclined) }
            plan = kept
        }

        // The lanes that never get a push start honest, not blank.
        for c in plan.handsOn { steps[c.id] = .handsOn }
        for c in plan.unreachable {
            steps[c.id] = .failed("Not reachable right now — over Wi-Fi or Bluetooth.")
        }
        for c in plan.followers { steps[c.id] = .waiting }

        // ── Stage 1: the pilot proves the credentials ──
        guard let pilot = plan.pilot else { return }
        await push(to: pilot, ssid: ssid, password: password)

        // ── Stage 2: the fleet follows, only behind proof ──
        guard FleetWiFiRollout.mayFanOut(pilotState: steps[pilot.id]) else {
            for c in plan.followers {
                steps[c.id] = .failed("Held back — the first Canary didn't make it across, so nothing else was touched.")
            }
            return
        }
        // Proven credentials fan out; the risk the staging existed for is
        // spent. HTTP followers go concurrently — each talks to its own
        // device. BLE followers QUEUE: the console runs one bonded
        // provisioning ceremony at a time (a second concurrent write would
        // be refused, not interleaved), so a parallel fan-out would rescue
        // one Canary and falsely fail the rest. The queue runs alongside
        // the HTTP work, so the slow lane never holds the fast one.
        let httpFollowers = plan.followers.filter { $0.path.isHTTP }
        let bleFollowers = plan.followers.filter { $0.path == .ble }
        await withTaskGroup(of: Void.self) { group in
            for c in httpFollowers {
                group.addTask { await self.push(to: c, ssid: ssid, password: password) }
            }
            group.addTask {
                for c in bleFollowers {
                    await self.push(to: c, ssid: ssid, password: password)
                }
            }
        }
    }

    /// Push to one device over its planned path and wait for the verdict.
    private func push(to candidate: FleetWiFiRollout.Candidate, ssid: String, password: String) async {
        // The disclosure gate, checked at the moment of the push: a
        // cleartext target the user did not approve is not sent, and says so.
        guard FleetWiFiRollout.mayPush(candidate.path, cleartextApproved: cleartextApproved) else {
            steps[candidate.id] = .failed(FleetWiFiRollout.cleartextDeclined)
            return
        }
        steps[candidate.id] = .sending
        switch candidate.path {
        case .http, .httpCleartext:
            guard let ref = devices.devices.first(where: { $0.id == candidate.id }),
                  let url = ref.baseURL,
                  let api = try? devices.api(for: ref) else {
                steps[candidate.id] = .failed("This Canary's pairing is incomplete — re-pair it, then try again.")
                return
            }
            do {
                try await api.wifiConnect(ssid: ssid, password: password)
            } catch {
                steps[candidate.id] = .failed(error.localizedDescription)
                return
            }
            // Accepted — the device is now leaving this network. Proof is
            // it answering again, nothing softer. An https Canary answers
            // through its pinned session, so the probe carries the pin.
            steps[candidate.id] = .confirming
            steps[candidate.id] = await Self.watchForReturn(url: url,
                                                            tlsFingerprint: ref.tlsCertFingerprint)

        case .ble:
            let outcome = await ble.writeWiFiCredentials(deviceID: candidate.id,
                                                         ssid: ssid, password: password)
            switch outcome {
            case .joined:
                // The device's own STATE characteristic said `connected` —
                // that is the join, verified at the source.
                steps[candidate.id] = .moved
            case .failed(let why):
                steps[candidate.id] = .failed(why)
            case .unreachable:
                steps[candidate.id] = .failed("Bluetooth couldn't reach it — move closer and try again.")
            }

        case .handsOn:
            steps[candidate.id] = .handsOn
        case .unreachable:
            steps[candidate.id] = .failed("Not reachable right now — over Wi-Fi or Bluetooth.")
        }
    }

    /// Probe the device's address until it answers or the return window
    /// closes. Static + nonisolated-friendly: it holds no state, just time.
    /// `tlsFingerprint` is the receipt pin an https Canary must be probed
    /// through (LivenessProbe refuses an unpinned https address).
    static func watchForReturn(url: URL, tlsFingerprint: String? = nil,
                               window: TimeInterval = FleetWiFiRollout.returnWindow)
        async -> FleetWiFiRollout.StepState {
        let deadline = Date().addingTimeInterval(window)
        while Date() < deadline {
            if await LivenessProbe.isAnswering(url, tlsFingerprint: tlsFingerprint) { return .moved }
            try? await Task.sleep(for: .seconds(5))
        }
        return .failed(FleetWiFiRollout.didNotReturn)
    }
}
