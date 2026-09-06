// WristSnapshot.swift  (SHARED — the phone→watch state contract)
//
// The ONE payload the iPhone sends the watch, and the one shape the watch app
// and its widgets render. It compiles into all of: the iOS app (builder), the
// watch app (receiver), and the watch widget extension (cache reader) — from
// this single file — so the two ends of the wrist link can never disagree
// about the schema. That, plus latest-state-wins delivery (WCSession
// applicationContext) and the revision rule below, is the whole
// "nothing ever gets out of sync" design (RFC
// docs/design/apple_watch_and_notifications.md §3.2).
//
// Rules that keep this rot-proof:
//   * Pure Foundation. No WatchConnectivity, no SwiftUI, no ActivityKit —
//     host-testable in SecuraCVTests like every other Wire-grade type.
//   * Enums travel as raw bytes with tolerant accessors (the
//     FleetActivityAttributes precedent): a newer phone never breaks an
//     older watch.
//   * New fields must be added as OPTIONALS (JSONDecoder then accepts old
//     payloads that lack them), and `schema` only bumps on a change an old
//     reader could misread — not on additive growth.
//   * Times cross the wire as ABSOLUTE dates, never as "N seconds ago"
//     counters — a counter goes stale in transit and would make every
//     snapshot look different from the last (defeating push dedup); a date
//     stays true and each side renders its own "ago".
//   * Two kinds of time, two precisions. WITNESS EVENT times are the coarse
//     10-minute buckets every surface renders (Invariant III — metadata
//     minimization of the witnessed world). LINK-HEALTH times (`sentAt`,
//     `lastVerifiedAt`) are the operational state of the user's own
//     phone↔watch path and delivery heartbeat — the same precision the
//     phone's provably-alive card and the Live Activity already show; they
//     describe the app, not the world, and coarsening them would only make
//     the staleness and dead-man's-switch displays dishonest.
//   * Encoding is pinned (JSON, sorted keys, dates as secondsSince1970) so
//     both ends agree byte-for-byte across OS versions.

import Foundation

/// One Canary's row on the wrist — the glanceable subset of `Witness`.
/// Timestamps are already the coarse 10-minute buckets the app renders
/// (Invariant III: never a precise second, on any surface).
struct WristWitness: Codable, Hashable, Identifiable, Sendable {
    var id: String
    var name: String
    var severityRaw: UInt8
    var linkRaw: UInt8
    var badgeRaw: UInt8
    var tamper: Bool
    /// The per-kind tamper story ("Power was cut", "Storage card removed" —
    /// the phone's Witness.tamperKind narration), when the phone knows one.
    /// ADDITIVE OPTIONAL: an older phone sends nothing and the wrist falls
    /// back to the bare "Tamper detected", which is still true.
    var tamperNarration: String? = nil
    var lastEventHeadline: String?
    var lastEventBucket: Date?
    var batteryPct: Int?
    var isMuted: Bool
    /// The device's 16-hex pubkey fingerprint — what lets the WRIST tie a
    /// heard presence beacon (which carries the last 2 bytes of it) to this
    /// row, so "Find this Canary" can run on the watch's own radio.
    /// ADDITIVE OPTIONAL: an older phone sends nothing and the wrist simply
    /// doesn't offer finding for the row — never a wrong match.
    var fingerprint: String? = nil
    /// The twin verdict, computed by the PHONE against the FULL fleet: the
    /// snapshot's rows are capped, so a wrist-side check could miss a twin
    /// the cap dropped and range toward the wrong device. ADDITIVE OPTIONAL:
    /// nil (an older phone) means the wrist falls back to checking the rows
    /// it can see — narrower, but never claiming more than it knows.
    var suffixAmbiguous: Bool? = nil

    var severity: Severity { Severity(tolerant: Int(severityRaw)) }
    var link: Liveness { Liveness(tolerant: Int(linkRaw)) }
    var badge: TrustBadge { TrustBadge(tolerant: Int(badgeRaw)) }

    /// What a live tamper should say on the wrist: the kind's story when the
    /// phone sent one, the bare truth otherwise. Empty reads as absent — a
    /// tolerant reader never renders a blank where an alarm belongs.
    var tamperHeadline: String {
        guard let n = tamperNarration, !n.isEmpty else { return "Tamper detected" }
        return n
    }
}

/// The heartbeat dead-man's-switch state, flattened for the wire. The phone's
/// `Heartbeat.PathState` carries associated values; the wrist needs the shape
/// plus the facts to re-render the same words, nothing more.
enum WristHeartbeatState: UInt8, Codable, Sendable {
    case unknown = 0
    case alive
    case testing
    case dark
    case failed

    init(tolerant raw: Int) { self = WristHeartbeatState(rawValue: UInt8(clamping: raw)) ?? .unknown }

    /// Semantic color role, resolved by Theme on whatever surface renders it.
    var role: Theme.Role {
        switch self {
        case .alive: return .calm
        case .testing: return .info
        case .unknown: return .neutral
        case .dark, .failed: return .alert
        }
    }

    var sfSymbol: String {
        switch self {
        case .alive: return "checkmark.seal.fill"
        case .testing: return "arrow.triangle.2.circlepath"
        case .unknown: return "questionmark.circle"
        case .dark: return "moon.zzz.fill"
        case .failed: return "exclamationmark.triangle.fill"
        }
    }
}

/// What the last heartbeat actually proved. Two very different claims, and
/// the copy below refuses to let the weaker one borrow the stronger one's
/// words: a Canary answering on the LAN is not evidence that a notification
/// would reach a phone across town.
enum WristBeatSource: UInt8, Codable, Sendable {
    /// An alert reached this phone and iOS accepted it — the strong claim.
    case pathVerified = 0
    /// A Canary answered. The fleet is up; the notification path is untested.
    case fleetCheckIn = 1

    /// Unknown (newer) sources decode to the WEAKER claim, never the stronger
    /// one. Every other tolerant decoder in this file clamps toward the safe
    /// reading; here "safe" means the sentence that promises less, because a
    /// source this build has never heard of proves nothing about delivery.
    init(tolerant raw: Int) { self = WristBeatSource(rawValue: UInt8(clamping: raw)) ?? .fleetCheckIn }
}

/// The heartbeat's human wording, in ONE place. The phone's provably-alive
/// card and the watch's Heartbeat screen both call this — two surfaces, one
/// sentence, no drift.
enum HeartbeatCopy {
    /// How long ago, in units a person uses. Minutes stop being readable
    /// somewhere around "verified 4320 min ago" — which is what a persisted
    /// verification from three days back would otherwise say.
    static func ago(_ seconds: Int) -> String {
        if seconds < 90 { return "just now" }
        let minutes = seconds / 60
        if minutes < 60 { return "\(minutes) min ago" }
        let hours = minutes / 60
        if hours < 24 { return hours == 1 ? "an hour ago" : "\(hours) hours ago" }
        let days = hours / 24
        return days == 1 ? "yesterday" : "\(days) days ago"
    }

    static func summary(state: WristHeartbeatState,
                        secondsSinceVerified: Int?,
                        failureReason: String? = nil,
                        source: WristBeatSource? = nil) -> String {
        switch state {
        case .unknown: return "Not yet verified"
        case .alive:
            guard let s = secondsSinceVerified else {
                return source == .fleetCheckIn ? "Your fleet checked in" : "Delivery verified"
            }
            // The honesty split: only a delivery iOS accepted may say
            // "verified". A Canary answering says what it is.
            return source == .fleetCheckIn
                ? "Your fleet checked in \(ago(s))"
                : "Delivery verified \(ago(s))"
        case .testing: return "Testing the whole path…"
        case .dark:
            let s = secondsSinceVerified ?? 0
            return "No heartbeat for \(s / 60) min — check your fleet"
        case .failed:
            if let why = failureReason, !why.isEmpty { return "Test failed: \(why)" }
            return "Test failed"
        }
    }
}

/// The full phone→watch state: the fleet roll-up every ambient surface shows,
/// plus the worst-first witness rows and the heartbeat.
/// One alert as the wrist needs it: who, what, how often, how it reached
/// them, and whether it still wants a human. A trimmed AlertRecord — same
/// fields, same coarse bucket, nothing added.
struct WristAlert: Codable, Hashable, Identifiable, Sendable {
    var id: String
    var witnessID: String
    var name: String
    var headline: String
    var severityRaw: UInt8
    /// Coarse 10-minute bucket (Invariant III) of the most recent occurrence.
    var bucket: Date
    var count: Int
    var deliveryRaw: UInt8
    var handlingRaw: UInt8
    /// True once the condition cleared — "over" must never look like "still
    /// happening" on the wrist either. ADDITIVE OPTIONAL: an older phone
    /// sends nothing and the watch simply doesn't claim a state it wasn't
    /// told (nil renders as no chip, not as "live").
    var resolved: Bool?

    var severity: Severity { Severity(tolerant: Int(severityRaw)) }
    var delivery: AlertDelivery { AlertDelivery(tolerant: Int(deliveryRaw)) }
    var handling: AlertHandling { AlertHandling(tolerant: Int(handlingRaw)) }
}

struct WristSnapshot: Codable, Hashable, Sendable {
    /// Bump ONLY for changes an old reader could misread; additive optional
    /// fields never bump it.
    static let schemaVersion = 1

    var schema: Int = WristSnapshot.schemaVersion

    /// Monotonic per-sender counter. See `isNewer(than:)`.
    var revision: UInt64
    /// When the phone composed this snapshot — the staleness anchor the watch
    /// UI shows, so the wrist never presents old state as current.
    var sentAt: Date

    var fleetName: String

    // The FleetRollup trio (same math as the Dynamic Island state).
    var severityRaw: UInt8
    var headline: String
    var healthy: Int
    var total: Int

    /// When the delivery path last verified end-to-end. nil = never.
    var lastVerifiedAt: Date?
    var heartbeatRaw: UInt8
    /// Only set when the heartbeat state is `.failed`.
    var heartbeatFailureReason: String?

    /// True when any of this state came from the seeded demo fleet — the
    /// wrist shows the same "sample data" banner the phone does. Honesty
    /// travels with the data.
    var isDemoData: Bool

    /// Worst-first, capped by `WristSync.maxWitnessRows`.
    var witnesses: [WristWitness]
    /// How many rows the cap dropped — a cap is never silent.
    var omittedWitnesses: Int

    // The living canary (Shared/CanaryMood.swift — the display firmware's
    // mood engine, mirrored). ADDITIVE OPTIONALS by the schema rules: an
    // older phone simply sends no mood and the wrist derives a safe one.
    var faceRaw: UInt8?
    var postureRaw: UInt8?
    var anxiety: Int?
    var trustDays: Int?
    /// Ambient copy only — the Voice rule: a mood line may rephrase
    /// contentment or name who's being looked for; it never words alarms.
    var moodLine: String?

    /// What actually needed the user, newest first — the wrist's copy of the
    /// phone's alert history. ADDITIVE OPTIONAL, same rules as the mood: an
    /// older phone sends none and the watch shows its honest empty state
    /// rather than inventing rows. Capped hard (`maxAlertRows`); the wrist is
    /// a glance, not an archive.
    var alerts: [WristAlert]?

    /// The last heartbeat of any kind, and what it proved. ADDITIVE
    /// OPTIONALS: a phone too old to send them leaves the wrist reading
    /// `lastVerifiedAt` exactly as it did before, which is still true — it
    /// simply can't tell a fleet check-in from a verified delivery, so it
    /// says the conservative thing.
    var lastBeatAt: Date?
    var beatSourceRaw: UInt8?

    /// The phone's consent-first discovery choice, carried to the wrist so
    /// finding over the watch's own radio honors the SAME gate the phone's
    /// radios do — the wrist never invents consent the user gave nowhere.
    /// ADDITIVE OPTIONAL: nil (an older phone) reads as not-consented, the
    /// honest failure direction for a radio decision.
    var discoveryConsented: Bool?

    var severity: Severity { Severity(tolerant: Int(severityRaw)) }
    /// nil when the sending phone predates the distinction.
    var beatSource: WristBeatSource? {
        beatSourceRaw.map { WristBeatSource(tolerant: Int($0)) }
    }
    var heartbeat: WristHeartbeatState { WristHeartbeatState(tolerant: Int(heartbeatRaw)) }

    /// The character's face, with an honest fallback for a phone too old to
    /// send one: alarming fleets hand the stage to the instruments (hidden),
    /// quiet fleets get the calm bird. Same rule the engine would apply.
    var face: CanaryFace {
        if let raw = faceRaw { return CanaryFace(tolerant: Int(raw)) }
        return severity >= .alert ? .hidden : .calm
    }

    var posture: CanaryPosture {
        guard let raw = postureRaw else { return .asFace }
        return CanaryPosture(tolerant: Int(raw))
    }

    /// The same sentence the phone's provably-alive card shows, rendered from
    /// this snapshot's facts at the wrist's own clock.
    func heartbeatSummary(now: Date = Date()) -> String {
        // The freshest signal the sender had: its last beat when it sends one,
        // its last verification otherwise (an older phone, or one that has
        // only ever been verified).
        let anchor = lastBeatAt ?? lastVerifiedAt
        let ago = anchor.map { max(0, Int(now.timeIntervalSince($0))) }
        return HeartbeatCopy.summary(state: heartbeat,
                                     secondsSinceVerified: ago,
                                     failureReason: heartbeatFailureReason,
                                     source: beatSource)
    }

    /// Adoption rule for the receiving side. Either monotonic signal wins:
    /// a higher revision (normal flow, immune to clock adjustments) or a
    /// later sentAt (covers a reinstalled phone whose counter restarted).
    /// Equal on both counts = a duplicate, not news.
    func isNewer(than other: WristSnapshot?) -> Bool {
        guard let other else { return true }
        return revision > other.revision || sentAt > other.sentAt
    }
}

/// The wire helpers both ends share: pinned encoding, the applicationContext
/// envelope, and the message vocabulary for the live (reachable) channel.
enum WristSync {
    /// applicationContext / reply-dict keys. The payload is one JSON blob
    /// under `snap` (WCSession dictionaries carry plist types only), plus the
    /// schema version beside it so a reader can tell "newer schema" apart
    /// from "garbage" without decoding.
    static let contextVersionKey = "v"
    static let contextPayloadKey = "snap"

    /// Live-channel message vocabulary (sendMessage). One key, one verb.
    static let messageCommandKey = "cmd"
    static let commandRefresh = "refresh"
    static let commandTestAlertPath = "testAlertPath"
    /// Mute one witness (payload: `muteIDKey` = witness id, and optionally
    /// `muteDurationKey` = a MuteDuration raw value). The PHONE owns mute
    /// semantics (its ledger, its punch-through rules); the wrist only asks,
    /// and the reply snapshot shows the result.
    ///
    /// The duration key is ADDITIVE-OPTIONAL like every other late arrival on
    /// this wire: a watch too old to send one still means "an hour", which is
    /// what it has always meant, and an unreadable value means the same. The
    /// phone never widens a mute it couldn't parse.
    static let commandMute = "mute"
    static let muteIDKey = "id"
    static let muteDurationKey = "for"

    /// Ask the Canary named by `identifyIDKey` to make itself known (~15 s
    /// blink + chirp). The PHONE carries it out — identify travels over
    /// Wi-Fi by device id, which is also what makes it the disambiguator
    /// when two Canaries share a beacon suffix. The reply carries
    /// `identifyOKKey` (Bool), `identifyVisualOnlyKey` (Bool — the device's
    /// chirp is set to visual-only), and on failure `identifyWhyKey` (the
    /// honest reason, shown verbatim).
    static let commandIdentify = "identify"
    static let identifyIDKey = "id"
    static let identifyOKKey = "chirpOK"
    static let identifyVisualOnlyKey = "chirpVisualOnly"
    static let identifyWhyKey = "chirpWhy"

    /// Row cap for the snapshot — applicationContext has a small transfer
    /// budget and a wrist list past this is unreadable anyway. The cap is
    /// reported via `omittedWitnesses`, never silent.
    static let maxWitnessRows = 24
    /// The wrist is a glance, not an archive: enough history to answer "what
    /// did I miss?" on a walk, small enough that the payload stays cheap.
    static let maxAlertRows = 12

    static func makeEncoder() -> JSONEncoder {
        let enc = JSONEncoder()
        enc.dateEncodingStrategy = .secondsSince1970
        // Deterministic bytes so "did the state actually change?" can be
        // answered by comparing encodings (the phone's push dedup).
        enc.outputFormatting = [.sortedKeys]
        return enc
    }

    static func makeDecoder() -> JSONDecoder {
        let dec = JSONDecoder()
        dec.dateDecodingStrategy = .secondsSince1970
        return dec
    }

    /// The applicationContext / reply envelope for a snapshot.
    static func context(for snapshot: WristSnapshot) throws -> [String: Any] {
        [contextVersionKey: snapshot.schema,
         contextPayloadKey: try makeEncoder().encode(snapshot)]
    }

    /// Schema version stamped on an envelope, if it is one of ours.
    static func contextVersion(of context: [String: Any]) -> Int? {
        context[contextVersionKey] as? Int
    }

    /// Decode an envelope. Returns nil for anything unreadable — and for any
    /// FUTURE schema, even one this build could structurally decode: a schema
    /// bump is RESERVED for changes an old reader would misread, so decoding
    /// it with old semantics is exactly the failure the version exists to
    /// prevent. Callers use `contextVersion(of:)` to tell the user "update
    /// the other side" instead of showing nothing.
    static func snapshot(fromContext context: [String: Any]) -> WristSnapshot? {
        if let version = contextVersion(of: context), version > WristSnapshot.schemaVersion {
            return nil
        }
        guard let data = context[contextPayloadKey] as? Data else { return nil }
        return try? makeDecoder().decode(WristSnapshot.self, from: data)
    }
}

// MARK: - Deterministic sample

extension WristSnapshot {
    /// A fixed, obviously-demo snapshot for widget placeholders, previews,
    /// and tests. Deterministic for a fixed clock (the DemoFleet discipline),
    /// always flagged `isDemoData` so no surface can pass it off as real, and
    /// never faking an alarm.
    static func sample(now: Date = Date(timeIntervalSince1970: 1_784_000_000)) -> WristSnapshot {
        let rows = [
            WristWitness(id: "demo-porch", name: "Front Porch",
                         severityRaw: Severity.notice.rawValue,
                         linkRaw: Liveness.online.rawValue,
                         badgeRaw: TrustBadge.verified.rawValue,
                         tamper: false,
                         lastEventHeadline: "Motion at Front Porch",
                         lastEventBucket: now.addingTimeInterval(-1_800),
                         batteryPct: nil, isMuted: false),
            WristWitness(id: "demo-garage", name: "Garage",
                         severityRaw: Severity.ok.rawValue,
                         linkRaw: Liveness.online.rawValue,
                         badgeRaw: TrustBadge.verified.rawValue,
                         tamper: false,
                         lastEventHeadline: nil, lastEventBucket: nil,
                         batteryPct: 82, isMuted: false),
            WristWitness(id: "demo-yard", name: "Back Yard",
                         severityRaw: Severity.ok.rawValue,
                         linkRaw: Liveness.online.rawValue,
                         badgeRaw: TrustBadge.verified.rawValue,
                         tamper: false,
                         lastEventHeadline: nil, lastEventBucket: nil,
                         batteryPct: nil, isMuted: false),
        ]
        return WristSnapshot(revision: 1,
                             sentAt: now.addingTimeInterval(-120),
                             fleetName: "Your Canaries",
                             severityRaw: Severity.notice.rawValue,
                             headline: "Front Porch • Activity",
                             healthy: 3,
                             total: 3,
                             lastVerifiedAt: now.addingTimeInterval(-300),
                             heartbeatRaw: WristHeartbeatState.alive.rawValue,
                             heartbeatFailureReason: nil,
                             isDemoData: true,
                             witnesses: rows,
                             omittedWitnesses: 0)
    }
}
