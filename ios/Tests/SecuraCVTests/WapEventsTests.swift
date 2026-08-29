// WapEventsTests.swift
//
// The first contract holder for GET /api/events/today. The gates scout for
// this wave found that NO test anywhere — iOS, node, or firmware host —
// pinned this endpoint's shape: the only consumer was the device's own
// untested dashboard JS. The phone client changes that, so it brings the
// belt: golden bytes matching the firmware's serializer verbatim, the
// tolerance rules every wire struct here honors, the two pure judgments
// (ambient filter, bucket honesty), and an on-disk read of the serializer
// itself so a firmware key rename fails HERE with a message naming both
// sides — instead of an event feed that silently decodes to zeros.

import XCTest
@testable import SecuraCV

final class WapEventsTests: XCTestCase {

    // MARK: - golden bytes (the printf's output, verbatim)

    func testGoldenRowsDecodeKeyForKey() throws {
        // Two rows exactly as csi_integration.cpp's snprintf emits them —
        // an acoustic anomaly with a bundler-minted id (0x80000000: the
        // Int32 overflow trap), then a presence event. Newest first, as
        // served; order must survive decoding.
        let wire = #"""
        {"events":[{"id":2147483648,"module":"acoustic.events","type":"smoke_alarm_t3","category":"anomaly","state":"heard","confidence":"confirmed","motion":0,"breathing":0,"bpm":0,"duration_sec":12,"bundled":1,"time_bucket":88,"dismissed":0},{"id":41,"module":"core.presence","type":"presence_changed","category":"event","state":"occupied","confidence":"observed","motion":63,"breathing":0,"bpm":0,"duration_sec":600,"bundled":3,"time_bucket":87,"dismissed":1}]}
        """#
        let today = try JSONDecoder().decode(WapEventsToday.self, from: Data(wire.utf8))
        XCTAssertEqual(today.events.count, 2)

        let alarm = today.events[0]
        XCTAssertEqual(alarm.id, 2_147_483_648, "bundler ids start at 0x80000000 — UInt32, never Int32")
        XCTAssertEqual(alarm.module, "acoustic.events")
        XCTAssertEqual(alarm.type, "smoke_alarm_t3")
        XCTAssertEqual(alarm.category, "anomaly")
        XCTAssertEqual(alarm.state, "heard")
        XCTAssertEqual(alarm.confidence, "confirmed")
        XCTAssertEqual(alarm.durationSec, 12)
        XCTAssertEqual(alarm.bundled, 1)
        XCTAssertEqual(alarm.timeBucket, 88)
        XCTAssertFalse(alarm.isDismissed)
        XCTAssertTrue(alarm.isNamedOccurrence)

        let presence = today.events[1]
        XCTAssertEqual(presence.id, 41)
        XCTAssertEqual(presence.type, "presence_changed")
        XCTAssertEqual(presence.motion, 63)
        XCTAssertTrue(presence.isDismissed, "the dismissed flag is on this wire and must survive")
        XCTAssertTrue(presence.isNamedOccurrence)

        // And the vocabulary this wire was built for actually answers it.
        XCTAssertEqual(EventVocabulary.severity(forWire: alarm.type), .alert)
        XCTAssertEqual(EventVocabulary.severity(forWire: presence.type), .notice)
    }

    // MARK: - tolerance (newer firmware never breaks an older app)

    func testMissingAndForeignFieldsDefaultInsteadOfSinkingThePage() throws {
        let sparse = #"{"events":[{"type":"glass_break","category":"anomaly","future_field":{"deep":true}}]}"#
        let today = try JSONDecoder().decode(WapEventsToday.self, from: Data(sparse.utf8))
        let row = try XCTUnwrap(today.events.first)
        XCTAssertEqual(row.type, "glass_break")
        XCTAssertEqual(row.id, 0)
        XCTAssertEqual(row.timeBucket, 0)
        XCTAssertFalse(row.isDismissed)

        let empty = try JSONDecoder().decode(WapEventsToday.self, from: Data(#"{}"#.utf8))
        XCTAssertTrue(empty.events.isEmpty, "no events key decodes as a quiet feed, not a failure")
    }

    func testEmptyFeedIsNormal() throws {
        // The ring is RAM-only and empties on every reboot — an empty array
        // is the device working, and must never read as an error.
        let today = try JSONDecoder().decode(WapEventsToday.self, from: Data(#"{"events":[]}"#.utf8))
        XCTAssertTrue(today.events.isEmpty)
    }

    // MARK: - the two pure judgments

    func testAmbientRowsAreChatterAndNamedCategoriesAreOccurrences() {
        var row = WapEventRow()
        row.category = "ambient"
        XCTAssertFalse(row.isNamedOccurrence, "ambient is room-sense chatter, not a happening")
        row.category = "anomaly"
        XCTAssertTrue(row.isNamedOccurrence)
        row.category = "event"
        XCTAssertTrue(row.isNamedOccurrence)
        row.category = "something_new"
        XCTAssertFalse(row.isNamedOccurrence, "an unknown category is not a claim we render — never a guess")
    }

    func testAnchoredDatesUseDeltasNeverAbsoluteBuckets() {
        // Absolute buckets are boot-relative on every current device (no
        // production caller of the clock-offset setter — review finding on
        // #1611), so ONLY the deltas may be believed. Fixed clock so the
        // expectations are bytes: now = 1_000_000_000, whose 10-minute
        // anchor is 999_999_600.
        let now = Date(timeIntervalSince1970: 1_000_000_000)
        func rowAt(_ bucket: Int) -> WapEventRow {
            var r = WapEventRow(); r.timeBucket = bucket; return r
        }

        // Newest first, as served: newest anchors at the fetch bucket, each
        // older row steps back by its bucket delta.
        let dates = WapEventRow.anchoredDates(for: [rowAt(88), rowAt(87), rowAt(80)],
                                              fetchedAt: now)
        XCTAssertEqual(dates, [Date(timeIntervalSince1970: 999_999_600),
                               Date(timeIntervalSince1970: 999_999_000),
                               Date(timeIntervalSince1970: 999_994_800)])

        // The mod-144 wrap: newest at bucket 2 with an older row at 140 is
        // six buckets — one hour — apart, not minus-138.
        let wrapped = WapEventRow.anchoredDates(for: [rowAt(2), rowAt(140)],
                                                fetchedAt: now)
        XCTAssertEqual(wrapped[1],
                       Date(timeIntervalSince1970: 999_999_600 - 6 * 600))

        // Every result sits on the 10-minute grid (Invariant III) and never
        // in the future; an empty page maps to an empty page.
        for d in dates + wrapped {
            XCTAssertEqual(d.timeIntervalSince1970.truncatingRemainder(dividingBy: 600), 0)
            XCTAssertLessThanOrEqual(d, now)
        }
        XCTAssertTrue(WapEventRow.anchoredDates(for: [], fetchedAt: now).isEmpty)
    }

    // MARK: - /api/status subset

    func testWapStatusDecodesItsSubsetAndIgnoresTheRest() throws {
        let wire = #"{"ok":true,"device_id":"wap-a1b2","device_type":"canary-wap","firmware":"2.4.14","ruleset":"r1","fingerprint":"cafe","pubkey":"00","chain_seq":9001,"gps":{"fix":false},"csi":{"running":true}}"#
        let status = try DeviceAPI.decoder.decode(WapStatus.self, from: Data(wire.utf8))
        XCTAssertEqual(status.deviceID, "wap-a1b2")
        XCTAssertEqual(status.deviceType, "canary-wap")
        XCTAssertEqual(status.firmware, "2.4.14")
        XCTAssertEqual(status.chainSeq, 9001)
    }

    // MARK: - the on-disk contract belt (first client, first holder)

    func testTheSwiftDecoderMirrorsTheFirmwareSerializerOnDisk() throws {
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // SecuraCVTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // ios
            .deletingLastPathComponent()   // repo root
        let source = repoRoot.appendingPathComponent(
            "firmware/projects/canary-wap/arduino/canary_wap/csi_integration.cpp")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: source.path),
                          "repo checkout not visible from the test host")
        let cpp = try String(contentsOf: source, encoding: .utf8)

        // The handler's slice, so a key in some OTHER route can't satisfy us.
        guard let start = cpp.range(of: "esp_err_t handle_events_today") else {
            XCTFail("handle_events_today moved — update this belt")
            return
        }
        let afterStart = cpp[start.upperBound...]
        let end = afterStart.range(of: "handle_events_dismiss")
        let handler = String(end.map { afterStart[..<$0.lowerBound] } ?? afterStart)

        // The envelope and the auth gate. Losing CSI_AUTH_OR_RETURN would
        // open the sensing feed to anyone on the LAN — a privacy regression
        // the client's Bearer header would silently paper over.
        XCTAssertTrue(handler.contains(#"{\"events\":["#),
                      "the events/today envelope moved — update WapEventsToday")
        XCTAssertTrue(handler.contains("CSI_AUTH_OR_RETURN"),
                      "events/today lost its auth gate — the feed must stay token-gated")

        // Every key the serializer prints, in order, against the decoder's
        // key set — exact equality both ways, so a renamed OR added field
        // fails here with both names in hand. Scan only AFTER the envelope
        // literal: the handler's OOM reply (`{\"ok\":…,\"reason\":…}`) sits
        // above it and its keys are not row keys.
        guard let envelope = handler.range(of: #"{\"events\":["#) else { return }
        let rows = String(handler[envelope.upperBound...])
        let keyRegex = try NSRegularExpression(pattern: #"\\"([a-z_]+)\\":"#)
        let range = NSRange(rows.startIndex..., in: rows)
        var printed: [String] = []
        keyRegex.enumerateMatches(in: rows, range: range) { match, _, _ in
            if let m = match, let r = Range(m.range(at: 1), in: rows) {
                let key = String(rows[r])
                if !printed.contains(key) { printed.append(key) }
            }
        }
        XCTAssertEqual(printed,
                       ["id", "module", "type", "category", "state", "confidence",
                        "motion", "breathing", "bpm", "duration_sec", "bundled",
                        "time_bucket", "dismissed"],
                       "the serializer's row keys drifted from WapEventRow — reconcile both")

        // The category vocabulary the ambient filter judges by.
        for word in ["\"ambient\"", "\"anomaly\"", "\"event\""] {
            XCTAssertTrue(handler.contains(word),
                          "category word \(word) left the serializer — isNamedOccurrence judges by it")
        }

        // And the filter's load-bearing assumption: the named-vocabulary
        // modules never emit as ambient, or the filter would hide them.
        for module in ["core_presence.cpp", "acoustic_events_module.cpp",
                       "vault_events_module.cpp"] {
            let path = repoRoot.appendingPathComponent(
                "firmware/projects/canary-wap/arduino/canary_wap/\(module)")
            let text = try String(contentsOf: path, encoding: .utf8)
            XCTAssertFalse(text.contains("CSI_CATEGORY_AMBIENT"),
                           "\(module) now emits ambient rows — the timeline filter would hide them")
        }
    }
}
