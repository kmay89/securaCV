//  FleetClient.swift — reaching the kernel, and healing when it isn't there.
//
//  The network contract is one required endpoint, `GET /api/fleet`
//  (tvos/discovery/DISCOVERY.md), plus one OPTIONAL one the Wall asks for and
//  no source serves yet: `GET /api/sealed-log`, the document the Rust core
//  verifies. Its absence is an answer, not an error. Everything else in this
//  file exists to make a flaky LAN a non-event: bounded timeouts, capped
//  exponential backoff, and a protocol seam so the whole reconnect story is
//  testable without a network.

import Foundation

/// The seam. The app uses `URLSessionFleetTransport`; tests use a stub, so the
/// backoff and failure copy are provable without a socket.
protocol FleetTransport: Sendable {
    /// Fetch `GET /api/fleet` from `base` and return the raw body.
    func fetchFleet(from base: URL) async throws -> String

    /// Fetch `GET /api/sealed-log` from `base` — the sealed-log document the
    /// Rust core verifies — or nil when the source doesn't serve one. Absence
    /// is an answer, not an error: no kernel or firmware ships the endpoint
    /// yet, and the Wall renders the honest self-report state until one does.
    func fetchSealedLog(from base: URL) async -> String?
}

extension FleetTransport {
    /// Default: no sealed log. Keeps the seam small for test stubs that only
    /// care about the fleet path; the real transport overrides this.
    func fetchSealedLog(from base: URL) async -> String? { nil }
}

/// Why the Wall can't currently see the fleet. The messages are the *screen*
/// copy: the Wall states drift plainly and never renders a problem as fine.
enum FleetError: LocalizedError, Equatable {
    case badAddress(String)
    case unreachable(String)
    case httpStatus(Int)
    case notFleetResponse(String)

    var errorDescription: String? {
        switch self {
        case .badAddress(let text):
            return "\"\(text)\" isn't an address the Wall can reach. Try something like http://canary.local:8099"
        case .unreachable(let why):
            return "Can't reach your hub right now — \(why)"
        case .httpStatus(404):
            return "That address answered, but it doesn't serve /api/fleet. Point the Wall at your hub."
        case .httpStatus(let code):
            return "Your hub answered with an error (HTTP \(code))."
        case .notFleetResponse(let why):
            return why
        }
    }
}

/// Turn what a person typed on a TV remote into a URL.
///
/// Deliberately forgiving in one direction only: a bare host gets `http://`
/// (the normal case for a hub on your own LAN), but nothing else is guessed.
enum FleetAddress {
    static func normalize(_ typed: String) throws -> URL {
        let trimmed = typed.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { throw FleetError.badAddress(typed) }

        let withScheme = trimmed.contains("://") ? trimmed : "http://\(trimmed)"
        guard let url = URL(string: withScheme), let host = url.host, !host.isEmpty else {
            throw FleetError.badAddress(typed)
        }
        return url
    }

    /// The fleet endpoint for a base address, whatever the person typed.
    /// Idempotent: pasting the full `…/api/fleet` URL works too.
    static func endpoint(for base: URL) -> URL {
        if base.path.hasSuffix("/api/fleet") { return base }
        return base.appendingPathComponent("api").appendingPathComponent("fleet")
    }

    /// The sealed-log endpoint beside the fleet endpoint, tolerating the same
    /// pasted-full-URL input `endpoint(for:)` tolerates.
    static func sealedLogEndpoint(for base: URL) -> URL {
        if base.path.hasSuffix("/api/fleet") {
            return base.deletingLastPathComponent().appendingPathComponent("sealed-log")
        }
        return base.appendingPathComponent("api").appendingPathComponent("sealed-log")
    }
}

/// The real transport.
struct URLSessionFleetTransport: FleetTransport {
    let session: URLSession

    init(timeout: TimeInterval = 6) {
        let config = URLSessionConfiguration.ephemeral   // the Wall caches nothing
        config.timeoutIntervalForRequest = timeout
        config.waitsForConnectivity = false             // fail fast, then back off
        self.session = URLSession(configuration: config)
    }

    func fetchFleet(from base: URL) async throws -> String {
        let url = FleetAddress.endpoint(for: base)
        do {
            let (data, response) = try await session.data(from: url)
            if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
                throw FleetError.httpStatus(http.statusCode)
            }
            guard let text = String(data: data, encoding: .utf8) else {
                throw FleetError.notFleetResponse("Your hub's answer wasn't text the Wall could read.")
            }
            return text
        } catch let error as FleetError {
            throw error
        } catch {
            throw FleetError.unreachable(error.localizedDescription)
        }
    }

    /// Best-effort by design: any non-2xx answer, transport failure, or
    /// non-text body is "this source serves no sealed log right now" — the
    /// state every source is in until a hub ships the endpoint. The verdict
    /// honesty lives downstream: nil here means the Wall shows the fleet's
    /// own report labeled as a report, never a verification it didn't do.
    func fetchSealedLog(from base: URL) async -> String? {
        let url = FleetAddress.sealedLogEndpoint(for: base)
        guard let (data, response) = try? await session.data(from: url) else { return nil }
        if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            return nil
        }
        return String(data: data, encoding: .utf8)
    }
}

/// Capped exponential backoff with full jitter.
///
/// A wall-mounted TV retries forever, so the cap is what matters: an unplugged
/// hub must settle into a slow poll, not hammer the LAN all night. Jitter keeps
/// several TVs in one building from retrying in lockstep.
struct Backoff {
    let base: TimeInterval
    let cap: TimeInterval
    private(set) var attempt: Int = 0

    init(base: TimeInterval = 2, cap: TimeInterval = 60) {
        self.base = base
        self.cap = cap
    }

    /// Undelayed ceiling for the next attempt — the value jitter is drawn from.
    var nextCeiling: TimeInterval {
        min(cap, base * pow(2, Double(attempt)))
    }

    /// Advance and return the delay to actually wait.
    mutating func nextDelay(randomness: (ClosedRange<Double>) -> Double = { Double.random(in: $0) }) -> TimeInterval {
        let ceiling = nextCeiling
        attempt += 1
        return randomness(0...ceiling)
    }

    /// A successful fetch resets the ladder, so one blip doesn't leave the Wall
    /// polling once a minute for the rest of the evening.
    mutating func reset() { attempt = 0 }
}
