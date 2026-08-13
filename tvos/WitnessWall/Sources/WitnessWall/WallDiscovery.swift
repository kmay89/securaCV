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
            var hosts: [String] = []
            for result in results {
                guard case let .bonjour(txt) = result.metadata else { continue }
                if let host = txt.dictionary["host"], !host.isEmpty {
                    hosts.append(host.hasSuffix(".local") ? host : host + ".local")
                }
            }
            box.add(hosts)
        }
        browser.start(queue: .global(qos: .utility))
        try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
        browser.cancel()
        return box.snapshot()
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
