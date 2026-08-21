// FleetWiFiRunner.swift
//
// Carries out a FleetWiFiRollout plan. The POLICY (pilot-first, who gets
// which transport, what the verdicts mean) lives in FleetWiFiRollout and is
// host-tested; this class is the pair of hands: it posts the HTTP writes,
// asks BLEConsole for the Bluetooth rescue, watches for each device to
// answer on the network again, and publishes per-device progress the sheet
// renders live.
//
// The one rule it enforces at runtime is the plan's one safety rule: the
// followers are not touched until the pilot has PROVEN the credentials by
// answering on the network again. A wrong password costs one Canary a
// rescue, never the fleet.

import Foundation

@MainActor
final class FleetWiFiRunner: ObservableObject {
    @Published private(set) var steps: [String: FleetWiFiRollout.StepState] = [:]
    @Published private(set) var running = false
    @Published private(set) var finished = false

    private let devices: DeviceStore
    private let ble: BLEConsole

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
    /// final verdict.
    func run(plan: FleetWiFiRollout.Plan, ssid: String, password: String) async {
        guard !running else { return }
        running = true
        finished = false
        defer { running = false; finished = true }

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
        let httpFollowers = plan.followers.filter { $0.path == .http }
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
        steps[candidate.id] = .sending
        switch candidate.path {
        case .http:
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
            // it answering again, nothing softer.
            steps[candidate.id] = .confirming
            steps[candidate.id] = await Self.watchForReturn(url: url)

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
    static func watchForReturn(url: URL, window: TimeInterval = FleetWiFiRollout.returnWindow)
        async -> FleetWiFiRollout.StepState {
        let deadline = Date().addingTimeInterval(window)
        while Date() < deadline {
            if await LivenessProbe.isAnswering(url) { return .moved }
            try? await Task.sleep(for: .seconds(5))
        }
        return .failed(FleetWiFiRollout.didNotReturn)
    }
}
