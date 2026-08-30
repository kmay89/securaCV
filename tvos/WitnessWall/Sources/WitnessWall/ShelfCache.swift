//  ShelfCache.swift  (SHARED — written by the app, read by the Top Shelf provider)
//
//  The tvOS twin of ios/Shared/WristCache.swift's PhoneGlanceCache: the Wall
//  parks its latest fleet summary in app-group UserDefaults, and the Top Shelf
//  extension renders from here even when the app is not running. The provider
//  process never fetches anything — no network, no Rust core, no CloudKit —
//  it only reads what the app last verified, and only while that answer is
//  recent enough to present as current.
//
//  Write-only from the app (WallModel's live path), read-only from the
//  provider — ONE writer, so the shelf and the wall cannot hold two truths.
//  Pure Foundation on purpose: this one file is compiled into both targets
//  (the NSE single-file precedent, ios/project.yml), so it must drag nothing
//  along.
//
//  Storage is UserDefaults-in-a-suite rather than a file, same as WristCache:
//  the payload is one small JSON blob, and defaults give atomicity for free.

import Foundation

/// What the shelf may say: the fleet's own summary — already worded by
/// `FleetSnapshot.summary`, verbatim — plus the counts behind it and WHEN it
/// was true. The shelf never re-words the fleet; one vocabulary on every
/// surface, the same rule the device vocabulary enforces on the wall itself.
struct ShelfSnapshot: Codable, Equatable, Sendable {
    /// `FleetSnapshot.summary`, verbatim — e.g. "3 Canaries, all online".
    let summary: String
    let onlineCount: Int
    let total: Int
    let hasChainTrouble: Bool
    /// When the Wall actually heard this from the fleet.
    let asOf: Date

    /// How old a cached answer may be and still appear on the shelf. The Wall
    /// polls every 10 seconds while live, so anything older than this means
    /// the app has been gone a while — and the shelf then shows NOTHING. The
    /// wall can label a remembered fleet stale (WallState.stale); the shelf
    /// has no room for a label, so its only honest degraded state is silence.
    static let maxAge: TimeInterval = 15 * 60

    /// Fresh enough to present as current. An `asOf` slightly in the future
    /// (clock skew across the app/extension boundary) is skew, not age.
    func isCurrent(at now: Date = Date()) -> Bool {
        now.timeIntervalSince(asOf) < Self.maxAge
    }

    /// The one line the shelf item shows. Chain trouble is appended in the
    /// wall's own words (WallView says "needs attention" for the same fact),
    /// never hidden behind a calm summary.
    var shelfTitle: String {
        hasChainTrouble ? "\(summary) · a record needs attention" : summary
    }
}

enum ShelfCache {
    /// tvOS-side app group — declared in Support/WitnessWall.entitlements and
    /// Support/TopShelf.entitlements (and ONLY those two).
    static let appGroupID = "group.com.securacv.witnesswall"
    static let snapshotKey = "shelf_snapshot_v1"

    /// `UserDefaults(suiteName:)` can return nil — an unsigned CI build has
    /// no app-group container — and a cache that cannot be reached is simply
    /// no cache: the save becomes a no-op and the provider shows an empty
    /// shelf, never a crash.
    static func save(_ snapshot: ShelfSnapshot, to defaults: UserDefaults? = nil) {
        guard let store = defaults ?? UserDefaults(suiteName: appGroupID),
              let data = try? makeEncoder().encode(snapshot) else { return }
        store.set(data, forKey: snapshotKey)
    }

    static func load(from defaults: UserDefaults? = nil) -> ShelfSnapshot? {
        guard let store = defaults ?? UserDefaults(suiteName: appGroupID),
              let data = store.data(forKey: snapshotKey) else { return nil }
        return try? makeDecoder().decode(ShelfSnapshot.self, from: data)
    }

    // The date strategy is spelled out because the blob crosses a process
    // boundary: the format is a contract between two binaries, not a default
    // an SDK update may quietly move.
    private static func makeEncoder() -> JSONEncoder {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .secondsSince1970
        return encoder
    }

    private static func makeDecoder() -> JSONDecoder {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .secondsSince1970
        return decoder
    }
}
