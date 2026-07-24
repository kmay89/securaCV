//  WallModel.swift — what the Wall knows, and how it heals.
//
//  One state machine, on the main actor, driven by a poll loop. Everything the
//  screen draws is derived from `state`, so there is no path where the TV shows
//  a stale "verified" while the hub is gone — the requirement from
//  docs/tvos/AUTOPIPELINE.md: drift is shown loudly and calmly, NEVER silently
//  rendered as fine.

import Foundation
import Observation

/// The Wall's whole world.
enum WallState: Equatable {
    /// Nothing configured yet — the Wall asks for a hub address.
    case needsHub
    /// Trying, with nothing good to show yet.
    case connecting(to: String)
    /// A fleet, plus how long ago we heard it.
    case live(FleetSnapshot, asOf: Date)
    /// We had a fleet, and now we don't. The last good snapshot is kept so the
    /// screen can say "this is what we last verified, and when" instead of
    /// going blank — but it is labelled stale, never presented as current.
    case stale(FleetSnapshot, since: Date, reason: String)
    /// Never got a fleet from this address.
    case unreachable(reason: String)
}

@MainActor
@Observable
final class WallModel {
    private(set) var state: WallState = .needsHub
    /// The last verification verdict, when the hub serves a sealed log.
    private(set) var report: VerifyReport?
    /// Address the person entered, persisted so a power cut heals itself.
    private(set) var hubAddress: String = ""

    let coreVersion = WitnessCore.version

    private let transport: FleetTransport
    private let defaults: UserDefaults
    private let pollInterval: TimeInterval
    private var backoff = Backoff()
    private var pollTask: Task<Void, Never>?

    private static let hubKey = "SecuraCVHubAddress"

    init(
        transport: FleetTransport = URLSessionFleetTransport(),
        defaults: UserDefaults = .standard,
        pollInterval: TimeInterval = 10
    ) {
        self.transport = transport
        self.defaults = defaults
        self.pollInterval = pollInterval
        self.hubAddress = defaults.string(forKey: Self.hubKey) ?? ""
    }

    deinit { pollTask?.cancel() }

    /// Start (or restart) the poll loop for the saved address. Safe to call
    /// repeatedly — an in-flight loop is always cancelled first, so a viewer
    /// mashing Connect can't leave two loops fighting over `state`.
    func start() {
        pollTask?.cancel()
        guard !hubAddress.isEmpty else {
            state = .needsHub
            return
        }
        let address = hubAddress
        pollTask = Task { [weak self] in
            await self?.pollLoop(address: address)
        }
    }

    func stop() {
        pollTask?.cancel()
        pollTask = nil
    }

    /// Point the Wall at a hub. Persists only after the address parses, so a
    /// typo can't get saved and re-fail on every boot.
    func connect(to typed: String) {
        do {
            _ = try FleetAddress.normalize(typed)
        } catch {
            state = .unreachable(reason: error.localizedDescription)
            return
        }
        hubAddress = typed
        defaults.set(typed, forKey: Self.hubKey)
        backoff.reset()
        state = .connecting(to: typed)
        start()
    }

    /// One fetch-verify-publish cycle. Exposed (internal) so tests can step the
    /// machine deterministically instead of racing a real loop.
    func refreshOnce() async {
        let address: URL
        do {
            address = try FleetAddress.normalize(hubAddress)
        } catch {
            state = .unreachable(reason: error.localizedDescription)
            return
        }

        do {
            let body = try await transport.fetchFleet(from: address)
            let snapshot = try WitnessCore.parseFleet(json: body)
            state = .live(snapshot, asOf: Date())
            backoff.reset()
        } catch {
            degrade(reason: error.localizedDescription)
        }
    }

    /// Losing the hub keeps the last good fleet on screen, clearly marked
    /// stale. Never having had one says so instead of showing an empty wall.
    private func degrade(reason: String) {
        switch state {
        case .live(let snapshot, let asOf):
            state = .stale(snapshot, since: asOf, reason: reason)
        case .stale(let snapshot, let since, _):
            state = .stale(snapshot, since: since, reason: reason)
        case .needsHub, .connecting, .unreachable:
            state = .unreachable(reason: reason)
        }
    }

    private func pollLoop(address: String) async {
        while !Task.isCancelled {
            await refreshOnce()

            // Healthy: poll at the steady interval. Degraded: back off, so an
            // unplugged hub settles into a slow retry rather than hammering.
            let delay: TimeInterval
            switch state {
            case .live:
                delay = pollInterval
            default:
                delay = backoff.nextDelay()
            }
            do {
                try await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            } catch {
                return   // cancelled
            }
        }
    }

    /// Verify a sealed log the hub served. Kept separate from the fleet poll:
    /// a fleet that renders is not the same claim as a chain that verifies, and
    /// conflating them is exactly how a wall starts lying.
    func verify(sealedLogJSON json: String) {
        report = try? WitnessCore.verify(sealedLogJSON: json)
    }
}
