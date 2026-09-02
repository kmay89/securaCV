//  WallDiscovery.swift — hear the Canaries announce themselves.
//
//  A fleet with no hub claims no canary.local, so probing well-known names
//  finds nothing — but every Canary has always ANNOUNCED itself, as the
//  `_securacv._tcp` Bonjour service (firmware discovery.cpp), the same one
//  the iPhone's Discovery browses. This is the Wall's ear for it: one browse
//  window, and the salted per-unit hostnames come back ready to poll.
//
//  Deliberately thin: NWBrowser cannot be constructed-and-fed in a test, so
//  everything with judgment in it lives elsewhere (FleetSnapshot.merged, the
//  model's probe loop) and this file only turns adverts into hostnames. The
//  TXT `host` key is the salted mDNS hostname (`canary-nightstand7-001-a1b2c3`)
//  — unique per unit even on firmware whose device_id is the compile-time
//  default, which is exactly why the iPhone keys its rows on it too.
//
//  It is also a CLAIM anyone on the LAN can make. An advert saying
//  `host=evil.example.com` used to be taken at its word and polled — so every
//  host now passes the core's gate (`WitnessCore.normalizeSourceHost`, the
//  same private-host rule the iPhone's DeviceAPI.isPrivate applies) before it
//  becomes a source, and `pollableHosts` is the pure, tested half of that.

import Foundation
import Network

enum WallDiscovery {

    /// Browse the LAN for one window and return every advertised hostname,
    /// `.local`-qualified and ready for FleetAddress.normalize. Empty when
    /// nothing announced itself — the caller decides what that means.
    static func browse(seconds: Double) async -> [String] {
        let params = NWParameters()
        params.includePeerToPeer = true
        let browser = NWBrowser(
            for: .bonjourWithTXTRecord(type: "_securacv._tcp", domain: nil),
            using: params
        )

        let box = HostBox()
        browser.browseResultsChangedHandler = { results, _ in
            var advertised: [String] = []
            for result in results {
                guard case let .bonjour(txt) = result.metadata else { continue }
                if let host = txt.dictionary["host"] { advertised.append(host) }
            }
            box.add(pollableHosts(advertised))
        }
        browser.start(queue: .global(qos: .utility))
        try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
        browser.cancel()
        return box.snapshot()
    }

    /// The TXT `host` values one browse window heard, reduced to the ones the
    /// Wall may poll: validated and `.local`-qualified by the Rust core
    /// (witness-core/src/host.rs — a bare label, a `.local` name, or a
    /// private IPv4 address; nothing else). A rejected host is logged and
    /// skipped, never polled and never persisted. Pure, so it is the testable
    /// half of `browse` (WallDiscoveryTests).
    static func pollableHosts(_ advertised: [String]) -> [String] {
        var hosts: [String] = []
        for raw in advertised {
            if let host = WitnessCore.normalizeSourceHost(raw) {
                hosts.append(host)
            } else if !raw.isEmpty {
                NSLog("WallDiscovery: skipping an advert whose host is not a LAN name: %@", raw)
            }
        }
        return hosts
    }
}

/// A lock around a set: NWBrowser's handler is a @Sendable callback on a
/// dispatch queue, and this is the smallest honest bridge from there to the
/// async caller under complete concurrency checking.
private final class HostBox: @unchecked Sendable {
    private let lock = NSLock()
    private var hosts = Set<String>()

    func add(_ found: [String]) {
        lock.lock()
        defer { lock.unlock() }
        hosts.formUnion(found)
    }

    func snapshot() -> [String] {
        lock.lock()
        defer { lock.unlock() }
        return hosts.sorted()
    }
}
