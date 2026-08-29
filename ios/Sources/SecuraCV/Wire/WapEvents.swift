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

    enum CodingKeys: String, CodingKey {
        case id, module, type, category, state, confidence
        case motion, breathing, bpm
        case durationSec = "duration_sec"
        case bundled
        case timeBucket = "time_bucket"
        case dismissed
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
    }

    var isDismissed: Bool { dismissed != 0 }

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
