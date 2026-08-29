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
//     not an omission. Until the device learns a clock offset the buckets
//     are boot-relative, so `bucketDate` degrades to the fetch time's bucket
//     rather than inventing a future timestamp (never a guess).
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

    /// The bucket as a wall-clock Date, coarse by construction (both
    /// branches land on a 10-minute boundary — Invariant III).
    ///
    /// A synced device's buckets are wall-aligned, so a bucket at or before
    /// the current one maps to today's bucket start. A bucket AHEAD of now
    /// is a device that hasn't learned a clock offset yet (boot-relative
    /// buckets) — for that row the only honest claim is "in today's ring,
    /// before this fetch", so it degrades to the fetch time's own bucket
    /// instead of inventing a time of day the event never had.
    func bucketDate(now: Date = Date(), calendar: Calendar = .current) -> Date {
        let midnight = calendar.startOfDay(for: now)
        let claimed = midnight.addingTimeInterval(TimeInterval(timeBucket) * 600)
        if claimed <= now { return claimed }
        let t = now.timeIntervalSince1970
        return Date(timeIntervalSince1970: (t / 600).rounded(.down) * 600)
    }
}
