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
    /// Nothing saved yet and the Wall is looking for the fleet by itself —
    /// probing the well-known LAN addresses every Canary install answers
    /// (tvos/discovery/DISCOVERY.md). Typing an address with a TV remote is
    /// the industry's worst setup step; the Wall's is: turn it on.
    case searching
    /// Nothing configured yet — the Wall asks for a hub address.
    case needsHub
    /// Trying, with nothing good to show yet.
    case connecting(to: String)
    /// A fleet, plus how long ago we heard it.
    case live(FleetSnapshot, asOf: Date)
    /// We had a fleet, and now we don't. The last good snapshot is kept so the
    /// screen can say "this is what we last verified, and when" instead of
    /// going blank — but it is labeled stale, never presented as current.
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

    /// The household's resident: this Apple TV, standing watch for the phones
    /// that left. Opt-in and off until someone turns it on.
    let resident: ResidentWatch

    let coreVersion = WitnessCore.version

    private let transport: FleetTransport
    private let defaults: UserDefaults
    private let pollInterval: TimeInterval
    private var backoff = Backoff()
    private var pollTask: Task<Void, Never>?

    private static let hubKey = "SecuraCVHubAddress"

    /// Where a fleet answers on a standard install, most-specific first: the
    /// hub's kernel port, then a lone canary-wap fronting its own fleet at
    /// canary.local. The SAME list the desktop Flasher and Lab probe
    /// (witnessBases / witness-host.js) — one discovery story on every
    /// surface, so "it found it on my Mac but not my TV" can't happen.
    static let wellKnownCandidates = ["canary.local:8099", "canary.local"]

    init(
        transport: FleetTransport = URLSessionFleetTransport(),
        defaults: UserDefaults = .standard,
        pollInterval: TimeInterval = 10
    ) {
        self.transport = transport
        self.defaults = defaults
        self.pollInterval = pollInterval
        self.hubAddress = defaults.string(forKey: Self.hubKey) ?? ""
        self.resident = ResidentWatch(defaults: defaults)
    }

    // No `deinit` canceling the poll task, for two reasons. It cannot compile
    // under complete concurrency checking — `deinit` is nonisolated and
    // `pollTask` is @MainActor — and it would be unreachable anyway: the task
    // holds `self` for the duration of each `pollLoop` call, so deinit cannot
    // run while a loop is in flight. The lifecycle is `start()`/`stop()`,
    // driven by the view's `onAppear`/`onDisappear`.

    /// Start (or restart) the Wall. With a saved address it polls that hub;
    /// with nothing saved it SEARCHES the LAN first — the setup step is
    /// "turn the TV on". Safe to call repeatedly — an in-flight loop is
    /// always canceled first, so a viewer mashing Connect can't leave two
    /// loops fighting over `state`.
    func start() {
        pollTask?.cancel()
        guard !hubAddress.isEmpty else {
            state = .searching
            pollTask = Task { [weak self] in
                await self?.searchThenPoll()
            }
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

    /// One pass over the well-known candidates. On the first address that
    /// answers with a real fleet, the address is persisted (so the next boot
    /// skips the search) and the Wall goes live. Exposed (internal) so tests
    /// can step the search deterministically.
    ///
    /// A candidate that answers with a NON-fleet (a captive portal, someone
    /// else's web server on canary.local) is skipped, not trusted — parse
    /// failure is a "keep looking", never a "close enough".
    @discardableResult
    func searchOnce() async -> Bool {
        for candidate in Self.wellKnownCandidates {
            if Task.isCancelled { return false }
            guard let url = try? FleetAddress.normalize(candidate) else { continue }
            guard let body = try? await transport.fetchFleet(from: url),
                  let snapshot = try? WitnessCore.parseFleet(json: body) else { continue }
            hubAddress = candidate
            defaults.set(candidate, forKey: Self.hubKey)
            backoff.reset()
            state = .live(snapshot, asOf: Date())
            return true
        }
        if case .searching = state { state = .needsHub }
        return false
    }

    /// The self-setup loop: search until a fleet appears, then poll it. While
    /// nothing answers the screen shows the manual-entry prompt, but the Wall
    /// keeps quietly re-searching on the backoff ladder — plug a Canary in a
    /// week later and the Wall finds it by itself.
    private func searchThenPoll() async {
        while !Task.isCancelled {
            if await searchOnce() {
                await pollLoop(address: hubAddress)
                return
            }
            do {
                try await Task.sleep(nanoseconds: UInt64(backoff.nextDelay() * 1_000_000_000))
            } catch {
                return   // canceled
            }
        }
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
            // The resident sees every snapshot the Wall does. It publishes a
            // wake only on a transition, only when a human turned it on, and
            // only for what this endpoint can honestly show (dark Canary,
            // troubled chain) — see ResidentWatch.
            resident.observe(snapshot)
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
        case .searching, .needsHub, .connecting, .unreachable:
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
                return   // canceled
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
