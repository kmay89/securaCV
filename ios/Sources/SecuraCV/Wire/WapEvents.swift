// WapEvents.swift  (Wire/ — pure parsing, app target only)
//
// The WAP's sensing-event feed, exactly as the firmware serves it:
// `GET /api/events/today` → `{"events":[ <row>, … ]}`, newest first, at most
// 64 rows, Bearer-token-gated (csi_integration.cpp:handle_events_today).
// This is the transport the DeviceEvent dialect (Shared/EventVocabulary) was
// built FOR: the row's `type` field carries the same firmware type_name
// strings — smoke_alarm_t3, presence_changed, frame_sealed — that the
// dialect's severities and headlines already speak. Until this file, that
// vocabulary had no wire; the seven-module belt in EventVocabularyTests kept
// it from rotting while the transport caught up. WapEventsTests pins THIS
// file to the firmware's serializer the same way — the first client is the
// first contract holder, so it brings the belt.
//
// Wire truths a reader must not "fix":
//   * `id` is a uint32 and the bundler mints ids from 0x8000_0000 up — an
//     Int32 anywhere in this path overflows on the first bundled event.
//   * `time_bucket` (0…143 ten-minute buckets) is the ONLY time information
//     on this wire — no epoch, no ISO string. That is Invariant III working,
//     not an omission. And the buckets are BOOT-RELATIVE on every current
//     device: `csi_event_set_clock_offset_minutes` has no production caller
//     (only a host test), so a bucket's absolute value means nothing —
//     while the DELTAS between buckets are exact, because they share one
//     clock. `anchoredDates` builds times from exactly those two truths and
//     nothing more (review finding on #1611).
//   * This is a RECORD, not a siren. State-bearing rows sit in an open
//     bundle until a two-minute quiet gap or the ten-minute window closes
//     it (csi_bundler_admit returns BUFFERED; nothing calls
//     csi_event_flush_bundles), so a row can surface minutes after the
//     sound — and then stay "newest" for hours. The fold treats it
//     accordingly: timeline history, never a live severity latch.
//   * An empty `events` array is NORMAL, not an error: the ring is RAM-only
//     and empties on every reboot (boot rehydration is deliberately
//     deferred — canary_wap.ino "ring rehydration").
//   * Rows are unsigned ring entries, not witness records: nothing here may
//     ever wear a `verified` badge (TrustBadge stays .unknown).

import Foundation

/// The `{"events":[…]}` envelope. Tolerant: a body without the key decodes
/// as an empty feed, matching the repo's "newer firmware never breaks an
/// older app" rule.
struct WapEventsToday: Codable, Sendable {
    var events: [WapEventRow] = []

    init(events: [WapEventRow] = []) { self.events = events }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        events = (try? c.decode([WapEventRow].self, forKey: .events)) ?? []
    }
}

/// One ring row, key-for-key with the firmware's serializer
/// (csi_integration.cpp handle_events_today — the printf that
/// WapEventsTests reads off disk). Every field tolerant-defaults so a row
/// from newer firmware still renders instead of sinking the whole page.
struct WapEventRow: Codable, Sendable, Equatable {
    var id: UInt32 = 0
    var module: String = ""
    var type: String = ""            // firmware type_name — the DeviceEvent wire
    var category: String = ""        // "ambient" | "anomaly" | "event"
    var state: String = ""
    var confidence: String = ""      // "tentative" | "observed" | "confirmed" | ""
    var motion: Int = 0
    var breathing: Int = 0
    var bpm: Int = 0
    var durationSec: Int = 0
    var bundled: Int = 0
    var timeBucket: Int = 0          // 0…143 — ten-minute buckets, Invariant III
    var dismissed: Int = 0           // 0/1 on the wire
    var open: Int = 0                // 1 while the bundle is still collecting — LIVE

    enum CodingKeys: String, CodingKey {
        case id, module, type, category, state, confidence
        case motion, breathing, bpm
        case durationSec = "duration_sec"
        case bundled
        case timeBucket = "time_bucket"
        case dismissed
        case open
    }

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = (try? c.decode(UInt32.self, forKey: .id)) ?? 0
        module = (try? c.decode(String.self, forKey: .module)) ?? ""
        type = (try? c.decode(String.self, forKey: .type)) ?? ""
        category = (try? c.decode(String.self, forKey: .category)) ?? ""
        state = (try? c.decode(String.self, forKey: .state)) ?? ""
        confidence = (try? c.decode(String.self, forKey: .confidence)) ?? ""
        motion = (try? c.decode(Int.self, forKey: .motion)) ?? 0
        breathing = (try? c.decode(Int.self, forKey: .breathing)) ?? 0
        bpm = (try? c.decode(Int.self, forKey: .bpm)) ?? 0
        durationSec = (try? c.decode(Int.self, forKey: .durationSec)) ?? 0
        bundled = (try? c.decode(Int.self, forKey: .bundled)) ?? 0
        timeBucket = (try? c.decode(Int.self, forKey: .timeBucket)) ?? 0
        dismissed = (try? c.decode(Int.self, forKey: .dismissed)) ?? 0
        open = (try? c.decode(Int.self, forKey: .open)) ?? 0
    }

    var isDismissed: Bool { dismissed != 0 }

    /// Whether the device says this row is happening NOW — an open bundle
    /// still collecting observations. Absent on pre-wave-5 firmware, so the
    /// tolerant default (0) reads as history, never as a false "now".
    var isOpen: Bool { open != 0 }

    /// The severity a row may contribute to a witness's LIVE state. The
    /// review on the first client established the rule this encodes: a
    /// closed ring row is history — it can sit "newest" for hours, so it
    /// caps at the calm tick and can never latch the fleet red. An OPEN row
    /// is the device's own claim of "current", so its true severity speaks —
    /// and the latch self-clears, because the flag drops the moment the
    /// bundle closes.
    var liveSeverity: Severity {
        let sev = EventVocabulary.severity(forWire: type)
        return isOpen ? sev : min(sev, .notice)
    }

    /// Whether this row names something that HAPPENED — the firmware's
    /// "anomaly" and "event" categories. "ambient" rows are the room-sense
    /// chatter (raw motion ticks, channel activity) that the device's own
    /// dashboard graphs but a phone timeline of happenings must not flood
    /// with. (Despite csi_event.h's "never persisted" comment, ambient rows
    /// DO reach the ring — the emit path persists unconditionally — so the
    /// filter lives client-side, here, where a test can hold it.)
    var isNamedOccurrence: Bool { category == "anomaly" || category == "event" }

    /// Dates for a newest-first page of rows, anchored at the fetch time —
    /// the strongest claim this wire supports, and not one inch more.
    ///
    /// Absolute buckets are boot-relative on every current device (no
    /// production caller of the clock-offset setter), so mapping a bucket
    /// to a time of day would misdate events by hours. What IS exact is the
    /// spacing: all rows share the device's clock, so the mod-144 delta
    /// between a row's bucket and the newest row's bucket is a true
    /// 10-minute-granular age difference — correct even across the ring's
    /// midnight wrap. The newest row is anchored at the fetch time's own
    /// bucket ("no later than now"), and every older row steps back by its
    /// delta. Both facts stay coarse: every result lands on the 10-minute
    /// grid (Invariant III) and never in the future.
    static func anchoredDates(for rows: [WapEventRow], fetchedAt now: Date) -> [Date] {
        guard let newest = rows.first else { return [] }
        let anchor = (now.timeIntervalSince1970 / 600).rounded(.down) * 600
        return rows.map { row in
            let delta = (((newest.timeBucket - row.timeBucket) % 144) + 144) % 144
            return Date(timeIntervalSince1970: anchor - TimeInterval(delta) * 600)
        }
    }
}

// MARK: - the 1 Hz stream snapshot

/// `GET /api/csi/stream` — a plain 1 Hz GET snapshot (not SSE), Bearer-gated,
/// with THREE mutually exclusive body shapes the firmware actually sends:
///   * radio HAL never came up → `{"t":0,"status":"unavailable","reason":…}`
///     (`status`/`reason` exist ONLY in this variant);
///   * a committed event → the full row (`id`/`module`/`type`/`privacy`/…);
///   * boot fallback, sensing but nothing committed yet → state "sensing"
///     with live motion/breathing and no `id`.
/// So every field is optional here, and the reader keys off `status` first,
/// then `id`, exactly as the device's own dashboard does. `supply` carries
/// pipeline health: `silentMs` is SIGNED and -1 means "no frames ever" — a
/// tile that ignores it would show a healthy quiet room on a starved radio.
struct WapStream: Codable, Sendable {
    var t: Double?             // seconds since sensing init — freshness, NOT epoch
    var status: String?        // "unavailable" in the hal-failed variant only
    var reason: String?
    var id: UInt32?            // present only in the committed-event variant
    var state: String?
    var confidence: String?
    var motion: Int?
    var breathing: Int?
    var bpm: Int?
    var supply: WapStreamSupply?

    enum CodingKeys: String, CodingKey {
        case t, status, reason, id, state, confidence, motion, breathing, bpm, supply
    }

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        t = try? c.decode(Double.self, forKey: .t)
        status = try? c.decode(String.self, forKey: .status)
        reason = try? c.decode(String.self, forKey: .reason)
        id = try? c.decode(UInt32.self, forKey: .id)
        state = try? c.decode(String.self, forKey: .state)
        confidence = try? c.decode(String.self, forKey: .confidence)
        motion = try? c.decode(Int.self, forKey: .motion)
        breathing = try? c.decode(Int.self, forKey: .breathing)
        bpm = try? c.decode(Int.self, forKey: .bpm)
        supply = try? c.decode(WapStreamSupply.self, forKey: .supply)
    }

    var isUnavailable: Bool { status == "unavailable" }

    /// True when the radio has never produced a frame — the honesty gate the
    /// dashboard applies before believing a quiet room.
    var radioSilent: Bool {
        guard let supply else { return false }
        if let silent = supply.silentMs, silent < 0 { return true }
        return supply.fps == 0
    }

    /// The room, in the dashboard's own words (its state→label dictionary,
    /// mirrored so the phone and the device tell one story).
    var stateLabel: String {
        switch state {
        case "empty": return "Empty"
        case "subtle", "breathing_lost": return "Subtle motion"
        case "quiet", "breathing_nearby": return "Quiet"
        case "active": return "Active"
        case "together": return "Together"
        default: return "Sensing…"
        }
    }

    /// BPM is shown only when the device itself says "confirmed" — the same
    /// bar the dashboard applies (never a tentative number on a vital sign).
    var confirmedBPM: Int? {
        guard confidence == "confirmed", let bpm, bpm > 0 else { return nil }
        return bpm
    }
}

struct WapStreamSupply: Codable, Sendable {
    var fps: Int?
    var probe: Bool?
    var silentMs: Int?         // SIGNED: -1 = no frames ever received

    enum CodingKeys: String, CodingKey {
        case fps, probe
        case silentMs = "silent_ms"
    }

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        fps = try? c.decode(Int.self, forKey: .fps)
        probe = try? c.decode(Bool.self, forKey: .probe)
        silentMs = try? c.decode(Int.self, forKey: .silentMs)
    }
}
