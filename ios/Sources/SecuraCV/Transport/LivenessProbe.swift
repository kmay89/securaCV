// LivenessProbe.swift
//
// The fast half of "a witness we can no longer hear is itself an alert":
// one tiny on-LAN question per paired Canary — is ANYTHING answering at its
// address? — on a tight timeout, every few seconds while the app is open.
// No cloud round trip anywhere in the loop, which is exactly why an
// unplugged Canary goes red here in seconds while cloud cameras are still
// waiting for a server to notice a missed keepalive.
//
// Honesty rules:
//   * ANY HTTP response is alive — a 401 is an auth wall doing its job on a
//     living device; only transport failure counts as a miss.
//   * One miss is weather (Wi-Fi hiccup, a busy ESP32). The LADDER decides:
//     two misses reads "Quiet", three reads "Lost" — fast, but never jumpy.
//     The ladder is pure and host-tested.

import Foundation

enum LivenessProbe {
    /// Tight-timeout session: liveness wants an answer NOW or a miss —
    /// waiting for connectivity would defeat the question.
    private static let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 2.5
        config.timeoutIntervalForResource = 2.5
        config.waitsForConnectivity = false
        return URLSession(configuration: config)
    }()

    /// True when the device answered with ANY HTTP status.
    static func isAnswering(_ base: URL) async -> Bool {
        var request = URLRequest(url: base.appendingPathComponent("api/v1/info"))
        request.cachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        do {
            _ = try await session.data(for: request)
            return true
        } catch {
            return false
        }
    }
}

/// Consecutive misses → link state. Pure; the sentinel loop applies it.
enum LivenessLadder {
    static let staleAfterMisses = 2
    static let lostAfterMisses = 3

    static func link(afterMisses misses: Int) -> Liveness {
        switch misses {
        case ..<staleAfterMisses: return .online
        case ..<lostAfterMisses: return .stale
        default: return .lost
        }
    }
}
