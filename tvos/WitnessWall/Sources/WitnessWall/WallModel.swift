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
    /// The verdict of THIS TV's own verification of the sealed log, refreshed
    /// every poll cycle (`refreshVerification`) — nil whenever the source
    /// serves no sealed log, which is every source until a hub ships the
    /// endpoint. The header banner keys off it: only a non-nil `ok` verdict
    /// may say "Verified"; everything else is phrased as the fleet's own
    /// report, because that is all it is.
    private(set) var report: VerifyReport?
    /// Where the fleet answers, persisted so a power cut heals itself. ONE
    /// entry when a hub (or a typed address) fronts the fleet; SEVERAL when
    /// the Wall found standalone Canaries by their own announcements and
    /// polls them all, merging the answers (FleetSnapshot.merged).
    private(set) var sources: [String] = []
    /// What the footer and the settings panel print for "connected to".
    var hubAddress: String {
        switch sources.count {
        case 0: return ""
        case 1: return sources[0]
        default: return "\(sources.count) Canaries · found on your network"
        }
    }

    /// The household's resident: this Apple TV, standing watch for the phones
    /// that left. Opt-in and off until someone turns it on.
    let resident: ResidentWatch

    let coreVersion = WitnessCore.version

    private let transport: FleetTransport
    /// The Bonjour browse, injectable exactly like the transport: a real
    /// NWBrowser cannot be constructed or fed in a test, and a unit test must
    /// not spend four wall-clock seconds listening to a simulator's network.
    private let discover: @Sendable (TimeInterval) async -> [String]
    private let defaults: UserDefaults
    private let pollInterval: TimeInterval
    private var backoff = Backoff()
    private var pollTask: Task<Void, Never>?

    private static let hubKey = "SecuraCVHubAddress"
    private static let sourcesKey = "SecuraCVWallSources"
    /// True when `sources` came from the Canaries' own announcements rather
    /// than a typed address or a found hub — the mode where the Wall keeps
    /// listening (see `reconcileDiscovered`), because a fleet you didn't
    /// configure is a fleet that grows without telling you.
    private static let discoveredKey = "SecuraCVWallSourcesDiscovered"
    /// How long one Bonjour browse listens before reporting. Announcements
    /// arrive within a second or two on a healthy LAN; four is patience, not
    /// hope — the search loop comes back around anyway.
    private static let browseWindow: TimeInterval = 4
    /// Poll cycles between re-browses in discovered mode — about a minute at
    /// the steady interval. A Canary plugged in later joins the wall on the
    /// next reconcile, not after a settings reset.
    private static let reconcileEvery = 6

    /// Where a fleet answers on a standard install, most-specific first: the
    /// hub's kernel port, then a lone canary-wap fronting its own fleet at
    /// canary.local. The SAME list the desktop Flasher and Lab probe
    /// (witnessBases / witness-host.js) — one discovery story on every
    /// surface, so "it found it on my Mac but not my TV" can't happen.
    static let wellKnownCandidates = ["canary.local:8099", "canary.local"]

    init(
        transport: FleetTransport = URLSessionFleetTransport(),
        defaults: UserDefaults = .standard,
        pollInterval: TimeInterval = 10,
        discover: @escaping @Sendable (TimeInterval) async -> [String] = { await WallDiscovery.browse(seconds: $0) }
    ) {
        self.transport = transport
        self.discover = discover
        self.defaults = defaults
        self.pollInterval = pollInterval
        // The multi-source list is the current shape; the single hub key is
        // read as a fallback so a Wall set up before 0.2.1 keeps its address.
        if let saved = defaults.stringArray(forKey: Self.sourcesKey), !saved.isEmpty {
            self.sources = saved
        } else if let single = defaults.string(forKey: Self.hubKey), !single.isEmpty {
            self.sources = [single]
        }
        self.resident = ResidentWatch(defaults: defaults)
    }

    private func persist(_ sources: [String], discovered: Bool = false) {
        self.sources = sources
        defaults.set(sources, forKey: Self.sourcesKey)
        defaults.set(discovered, forKey: Self.discoveredKey)
        // Keep the legacy key coherent for anything still reading it.
        defaults.set(sources.first ?? "", forKey: Self.hubKey)
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
        // The Wall coming back on screen is the resident coming back on duty,
        // and an iCloud account can be signed out while tvOS had us suspended.
        // Re-ask rather than trusting an answer from a previous session — a
        // stale "ready" is the false assurance this watch exists to avoid.
        Task { [resident] in await resident.refreshAccount() }
        guard !sources.isEmpty else {
            state = .searching
            pollTask = Task { [weak self] in
                await self?.searchThenPoll()
            }
            return
        }
        pollTask = Task { [weak self] in
            await self?.pollLoop()
        }
    }

    func stop() {
        pollTask?.cancel()
        pollTask = nil
    }

    /// Point the Wall at a hub. Persists only after the address parses, so a
    /// typo can't get saved and re-fail on every boot. Typing an address is a
    /// deliberate act, so it REPLACES a discovered set — one hand on the
    /// wheel at a time.
    func connect(to typed: String) {
        do {
            _ = try FleetAddress.normalize(typed)
        } catch {
            state = .unreachable(reason: error.localizedDescription)
            return
        }
        persist([typed])
        // The old source's verdict does not cover the new source's fleet.
        report = nil
        backoff.reset()
        state = .connecting(to: typed)
        start()
    }

    /// One pass of the two-rung search. Rung one: the well-known addresses a
    /// hub or WAP claims (most-specific first) — a hub fronts its whole
    /// fleet, so it wins outright. Rung two, NEW in 0.2.1: nothing claims
    /// canary.local in a hubless home, but every Canary ANNOUNCES itself —
    /// so browse the `_securacv._tcp` service the firmware has always
    /// advertised, probe everything that answered, and keep every address
    /// that served a real fleet. Exposed (internal) so tests can step the
    /// machine deterministically.
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
            persist([candidate])
            backoff.reset()
            state = .live(snapshot, asOf: Date())
            return true
        }

        if Task.isCancelled { return false }
        let announced = await discover(Self.browseWindow)
        var confirmed: [String] = []
        var parts: [FleetSnapshot] = []
        for host in announced {
            if Task.isCancelled { return false }
            guard let url = try? FleetAddress.normalize(host) else { continue }
            guard let body = try? await transport.fetchFleet(from: url),
                  let snapshot = try? WitnessCore.parseFleet(json: body) else { continue }
            confirmed.append(host)
            parts.append(snapshot)
        }
        if !confirmed.isEmpty {
            // Tag each part with its source before merging, so two units
            // sharing a stale default name stay two rows (see merged()).
            // A single discovered Canary stays untagged, same as a hub.
            let tagged = confirmed.count > 1
                ? zip(confirmed, parts).map { $1.tagged(bySource: $0) }
                : parts
            for (host, part) in zip(confirmed, tagged) { lastAnswer[host] = part }
            persist(confirmed, discovered: true)
            backoff.reset()
            state = .live(FleetSnapshot.merged(tagged), asOf: Date())
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
                await pollLoop()
                return
            }
            do {
                try await Task.sleep(nanoseconds: UInt64(backoff.nextDelay() * 1_000_000_000))
            } catch {
                return   // canceled
            }
        }
    }

    /// The last fleet each source served, so a source that goes dark can be
    /// SHOWN dark instead of silently vanishing from the merge. In-memory
    /// only: after a reboot a source that never answers again is simply
    /// absent, which is the truth a fresh boot actually knows.
    private var lastAnswer: [String: FleetSnapshot] = [:]

    /// One fetch-verify-publish cycle over EVERY source. A hub is one source
    /// answering for everyone; a hubless fleet is several, merged
    /// (FleetSnapshot.merged). Partial answers count as live — one dark
    /// Canary must not blank the ones still talking — but the dark one is
    /// not DROPPED either: its last-known devices stay on the wall, marked
    /// offline, because "this Canary is not answering" is a fact the Wall
    /// knows and hiding it would let the merge read as all-online. The wall
    /// only degrades to stale when NOBODY answers. Exposed (internal) so
    /// tests can step the machine deterministically instead of racing a
    /// real loop.
    func refreshOnce() async {
        guard !sources.isEmpty else {
            state = .unreachable(reason: "No hub or Canary address is set.")
            return
        }

        var parts: [FleetSnapshot] = []
        var anyAnswered = false
        var lastError = "unreachable"
        let multi = sources.count > 1
        for source in sources {
            do {
                let address = try FleetAddress.normalize(source)
                let body = try await transport.fetchFleet(from: address)
                let snapshot = try WitnessCore.parseFleet(json: body)
                let part = multi ? snapshot.tagged(bySource: source) : snapshot
                lastAnswer[source] = part
                parts.append(part)
                anyAnswered = true
            } catch {
                lastError = error.localizedDescription
                if let remembered = lastAnswer[source] {
                    parts.append(remembered.withEveryDeviceOffline())
                }
            }
        }

        if !anyAnswered {
            degrade(reason: lastError)
            return
        }
        let snapshot = FleetSnapshot.merged(parts)
        state = .live(snapshot, asOf: Date())
        // The Top Shelf renders from this cache when the app is off screen.
        // Written ONLY here — on a fresh answer — so the shelf can never say
        // something the wall did not just verify; the provider ages the cache
        // out (ShelfSnapshot.maxAge) rather than trusting it forever.
        ShelfCache.save(ShelfSnapshot(fleet: snapshot, asOf: Date()))
        // The resident sees every snapshot the Wall does. It publishes a
        // wake only on a transition, only when a human turned it on, and
        // only for what this endpoint can honestly show (dark Canary,
        // troubled chain) — see ResidentWatch.
        resident.observe(snapshot)
        backoff.reset()
        await refreshVerification()
    }

    /// One verify pass of this TV's own: fetch the sealed log and run the
    /// Rust core over it, every poll cycle. This is what earns the header's
    /// "Verified" — a fleet that renders is not the same claim as a chain
    /// that verifies, and conflating them is exactly how a wall starts lying.
    ///
    /// Two honesty rules bound it:
    ///  * ONE verdict, ONE source — the same rule `FleetSnapshot.merged`
    ///    applies to `verified_through`: a chain fetched from one Canary must
    ///    not banner devices another one reported, so a multi-source wall
    ///    carries no verdict at all.
    ///  * A body that is not a sealed log AT ALL (a squatted host's login
    ///    page, an endpoint nobody serves — today, every source) is a
    ///    non-answer, not a failed verification: same "keep looking, never
    ///    close enough" rule the fleet parse applies. The core reports that
    ///    case as malformed with no failing entry, which is the one shape
    ///    that never indicts a real chain.
    private func refreshVerification() async {
        guard sources.count == 1,
              let address = try? FleetAddress.normalize(sources[0]),
              let sealed = await transport.fetchSealedLog(from: address) else {
            report = nil
            return
        }
        let verdict = try? WitnessCore.verify(sealedLogJSON: sealed)
        if let verdict, verdict.kind == .malformed, verdict.failedAt == nil {
            report = nil
        } else {
            report = verdict
        }
    }

    /// Losing the hub keeps the last good fleet on screen, clearly marked
    /// stale. Never having had one says so instead of showing an empty wall.
    /// The verify verdict does NOT survive the loss — a remembered verdict is
    /// not a current verdict, the same rule `withEveryDeviceOffline` applies
    /// to a remembered `verified_through`.
    private func degrade(reason: String) {
        report = nil
        switch state {
        case .live(let snapshot, let asOf):
            state = .stale(snapshot, since: asOf, reason: reason)
        case .stale(let snapshot, let since, _):
            state = .stale(snapshot, since: since, reason: reason)
        case .searching, .needsHub, .connecting, .unreachable:
            state = .unreachable(reason: reason)
        }
    }

    /// Discovered mode keeps an ear open: every few poll cycles, re-browse
    /// and probe anything NEW that announced itself. A Canary plugged in
    /// next month joins the wall by itself — the same promise the first
    /// search makes, kept continuously. Dead sources are not removed here;
    /// they stay on the wall as offline (see refreshOnce), because absence
    /// of an announcement is not proof of absence.
    func reconcileDiscovered() async {
        guard defaults.bool(forKey: Self.discoveredKey) else { return }
        let announced = await discover(Self.browseWindow)
        var grew = false
        for host in announced where !sources.contains(host) {
            guard let url = try? FleetAddress.normalize(host),
                  let body = try? await transport.fetchFleet(from: url),
                  let snapshot = try? WitnessCore.parseFleet(json: body) else { continue }
            lastAnswer[host] = snapshot.tagged(bySource: host)
            persist(sources + [host], discovered: true)
            grew = true
        }
        if grew { await refreshOnce() }
    }

    private var pollsSinceReconcile = 0

    private func pollLoop() async {
        while !Task.isCancelled {
            await refreshOnce()

            pollsSinceReconcile += 1
            if pollsSinceReconcile >= Self.reconcileEvery {
                pollsSinceReconcile = 0
                await reconcileDiscovered()
            }

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
}

// MARK: - What the Wall tells the shelf

extension ShelfSnapshot {
    /// Built from the fleet the Wall just went live with. The summary is
    /// `FleetSnapshot.summary` VERBATIM — the shelf and the wall must never
    /// word the same fleet two ways. This bridge lives here rather than in
    /// ShelfCache.swift because that file is compiled into the Top Shelf
    /// target too, which has no `FleetSnapshot` (and must not grow one).
    init(fleet: FleetSnapshot, asOf: Date) {
        self.init(summary: fleet.summary,
                  onlineCount: fleet.onlineCount,
                  total: fleet.devices.count,
                  hasChainTrouble: fleet.hasChainTrouble,
                  asOf: asOf)
    }
}
