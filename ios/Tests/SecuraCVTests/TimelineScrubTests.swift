// TimelineScrubTests.swift
//
// The Swift half of the timeline's cross-language parity proof.
//
// tests/fixtures/timeline/scrub_parity.json is generated from the JavaScript
// implementation (viewer/timeline_core.js) by viewer/tools/gen_timeline_parity.mjs
// and asserted by BOTH sides — viewer/timeline_core.test.js and this file. The
// point is not coverage for its own sake: the same window of the same log has
// to look like the same day on the phone, on the TV, and in the offline
// evidence viewer. Two surfaces that fold quiet differently disagree about
// what the day WAS, and then the record stops being a record.
//
// So a rule change here is a two-sided change: edit both implementations,
// regenerate the fixture, and commit all three together. Whichever side lags
// fails on its own test.

import XCTest
@testable import SecuraCV

final class TimelineScrubTests: XCTestCase {

    // MARK: - fixture plumbing

    /// #filePath → ios/Tests/SecuraCVTests/… → repo root is four up. Skips
    /// (rather than fails) when the checkout isn't visible from the test
    /// host, matching EventVocabularyTests: the JS side plus the generator's
    /// `--check` remain the durable gates in CI.
    private func parityFixture() throws -> [String: Any] {
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // SecuraCVTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // ios
            .deletingLastPathComponent()   // repo root
        let url = repoRoot.appendingPathComponent("tests/fixtures/timeline/scrub_parity.json")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: url.path),
                          "repo checkout not visible from the test host; " +
                          "viewer/timeline_core.test.js remains the primary parity gate")
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf: url))
        return try XCTUnwrap(json as? [String: Any])
    }

    private func records(from raw: [[String: Any]]) throws -> [TimelineRecord] {
        try raw.map { r in
            TimelineRecord(
                t0: try XCTUnwrap(r["t0"] as? Int),
                size: (r["size"] as? Int) ?? TimelineScrub.defaultBucketSeconds,
                kind: try XCTUnwrap(TimelineKind(rawValue: try XCTUnwrap(r["kind"] as? String))),
                label: try XCTUnwrap(r["label"] as? String),
                family: try XCTUnwrap(TimelineFamily(rawValue: try XCTUnwrap(r["family"] as? String))),
                zone: (r["zone"] as? String) ?? "",
                confidence: r["conf"] as? Double,
                details: (r["details"] as? String) ?? "")
        }
    }

    /// The fixture stores offsets rounded to 3 decimals; compare on the same
    /// grid so a float's last bit never fails a parity test.
    private func round3(_ v: Double) -> Double { (v * 1000).rounded() / 1000 }

    // MARK: - the parity run

    func testEveryScenarioMatchesTheGeneratedFixture() throws {
        let fx = try parityFixture()
        let height = Double(try XCTUnwrap(fx["layout_height"] as? Int))
        let minGap = Double(try XCTUnwrap(fx["grid_min_gap"] as? Int))
        let scenarios = try XCTUnwrap(fx["scenarios"] as? [[String: Any]])
        XCTAssertFalse(scenarios.isEmpty, "the fixture must carry scenarios")

        for scenario in scenarios {
            let name = try XCTUnwrap(scenario["name"] as? String)
            let recs = try records(from: try XCTUnwrap(scenario["records"] as? [[String: Any]]))
            let expected = try XCTUnwrap(scenario["expected"] as? [String: Any])

            let model = TimelineScrub.model(
                for: recs,
                coverageT0: scenario["coverage_t0"] as? Int,
                coverageT1: scenario["coverage_t1"] as? Int)
            let layout = TimelineScrub.layout(model.segments, height: height)

            XCTAssertEqual(model.bucketSeconds, expected["bucket_seconds"] as? Int, "\(name): bucket")
            XCTAssertEqual(model.heartbeats, expected["heartbeats"] as? Int, "\(name): heartbeats")
            XCTAssertEqual(model.zones, (expected["zones"] as? [String]) ?? [], "\(name): zones")

            // counts — the fixture omits families with no records at all.
            let wantCounts = try XCTUnwrap(expected["counts"] as? [String: Int])
            var gotCounts: [String: Int] = [:]
            for (family, n) in model.counts { gotCounts[family.rawValue] = n }
            XCTAssertEqual(gotCounts, wantCounts, "\(name): counts")

            // segments, including how many heartbeats each fold swallowed.
            let wantSegments = try XCTUnwrap(expected["segments"] as? [[String: Any]])
            XCTAssertEqual(model.segments.count, wantSegments.count, "\(name): segment count")
            for (i, want) in wantSegments.enumerated() where i < model.segments.count {
                let got = model.segments[i]
                XCTAssertEqual(got.kind.rawValue, want["kind"] as? String, "\(name): segment \(i) kind")
                XCTAssertEqual(got.t0, want["t0"] as? Int, "\(name): segment \(i) t0")
                XCTAssertEqual(got.t1, want["t1"] as? Int, "\(name): segment \(i) t1")
                XCTAssertEqual(got.heartbeats, want["heartbeats"] as? Int, "\(name): segment \(i) beats")
            }

            // laid-out offsets — folds are a fixed pleat, spans share the rest.
            let wantLayout = try XCTUnwrap(expected["layout"] as? [[String: Any]])
            XCTAssertEqual(layout.count, wantLayout.count, "\(name): layout count")
            for (i, want) in wantLayout.enumerated() where i < layout.count {
                XCTAssertEqual(round3(layout[i].y0), want["y0"] as? Double, "\(name): layout \(i) y0")
                XCTAssertEqual(round3(layout[i].y1), want["y1"] as? Double, "\(name): layout \(i) y1")
            }

            let wantGrid = try XCTUnwrap(expected["grid_lines"] as? [[String: Any]])
            let gotGrid = TimelineScrub.gridLines(layout, minGap: minGap)
            XCTAssertEqual(gotGrid.count, wantGrid.count, "\(name): grid line count")
            for (i, want) in wantGrid.enumerated() where i < gotGrid.count {
                XCTAssertEqual(gotGrid[i].t, want["t"] as? Int, "\(name): grid \(i) t")
                XCTAssertEqual(round3(gotGrid[i].y), want["y"] as? Double, "\(name): grid \(i) y")
                XCTAssertEqual(gotGrid[i].isMajor, want["major"] as? Bool, "\(name): grid \(i) major")
            }

            // days and their density strips
            let wantDays = try XCTUnwrap(expected["days"] as? [[String: Any]])
            XCTAssertEqual(model.days.count, wantDays.count, "\(name): day count")
            for (i, want) in wantDays.enumerated() where i < model.days.count {
                let got = model.days[i]
                XCTAssertEqual(got.dayT0, want["day_t0"] as? Int, "\(name): day \(i) t0")
                XCTAssertEqual(got.label, want["label"] as? String, "\(name): day \(i) label")
                XCTAssertEqual(got.count, want["count"] as? Int, "\(name): day \(i) count")
                XCTAssertEqual(got.gapCount, want["gap_count"] as? Int, "\(name): day \(i) gaps")
                XCTAssertEqual(got.tamperCount, want["tamper_count"] as? Int, "\(name): day \(i) tamper")
                XCTAssertEqual(got.cellsPerDay, want["cells_per_day"] as? Int, "\(name): day \(i) cells/day")
                XCTAssertEqual(got.firstIndex, want["first_index"] as? Int, "\(name): day \(i) firstIndex")
                XCTAssertEqual(round3(got.coverageFrom), want["coverage_from"] as? Double,
                               "\(name): day \(i) coverage from")
                XCTAssertEqual(round3(got.coverageTo), want["coverage_to"] as? Double,
                               "\(name): day \(i) coverage to")
                // JSON null decodes to NSNull, which fails the NSNumber cast —
                // so an absent severity compares equal to Swift's nil.
                XCTAssertEqual(got.worstSeverityRaw,
                               (want["worst_severity"] as? NSNumber).map { UInt8(truncating: $0) },
                               "\(name): day \(i) worst severity")
                let wantCells = try XCTUnwrap(want["cells"] as? [[String: Any]])
                XCTAssertEqual(got.cells.count, wantCells.count, "\(name): day \(i) cell count")
                for (j, wc) in wantCells.enumerated() where j < got.cells.count {
                    XCTAssertEqual(got.cells[j].index, wc["i"] as? Int, "\(name): day \(i) cell \(j) index")
                    XCTAssertEqual(got.cells[j].count, wc["count"] as? Int, "\(name): day \(i) cell \(j) count")
                    XCTAssertEqual(got.cells[j].family.rawValue, wc["family"] as? String,
                                   "\(name): day \(i) cell \(j) family")
                    XCTAssertEqual(got.cells[j].hasGap, wc["has_gap"] as? Bool,
                                   "\(name): day \(i) cell \(j) gap")
                    XCTAssertEqual(got.cells[j].worstSeverityRaw,
                                   (wc["worst_severity"] as? NSNumber).map { UInt8(truncating: $0) },
                                   "\(name): day \(i) cell \(j) worst severity")
                }
            }

            // reading order: headers, pleats and rows interleaved
            let wantItems = try XCTUnwrap(expected["items"] as? [[String: Any]])
            XCTAssertEqual(model.items.count, wantItems.count, "\(name): item count")
            for (i, want) in wantItems.enumerated() where i < model.items.count {
                switch model.items[i] {
                case .day(let d):
                    XCTAssertEqual("day", want["kind"] as? String, "\(name): item \(i) kind")
                    XCTAssertEqual(d.dayT0, want["day_t0"] as? Int, "\(name): item \(i) day")
                case .fold(let t0, let t1, let beats):
                    XCTAssertEqual("fold", want["kind"] as? String, "\(name): item \(i) kind")
                    XCTAssertEqual(t0, want["t0"] as? Int, "\(name): item \(i) fold t0")
                    XCTAssertEqual(t1, want["t1"] as? Int, "\(name): item \(i) fold t1")
                    XCTAssertEqual(beats, want["heartbeats"] as? Int, "\(name): item \(i) fold beats")
                case .record(let r, let index):
                    XCTAssertEqual(r.kind.rawValue, want["kind"] as? String, "\(name): item \(i) kind")
                    XCTAssertEqual(index, want["index"] as? Int, "\(name): item \(i) index")
                    XCTAssertEqual(r.t0, want["t0"] as? Int, "\(name): item \(i) t0")
                    XCTAssertEqual(r.label, want["label"] as? String, "\(name): item \(i) label")
                }
            }

            // the time <-> offset mapping, sampled across the folded space
            for probe in try XCTUnwrap(expected["time_probes"] as? [[String: Any]]) {
                let t = try XCTUnwrap(probe["t"] as? Int)
                XCTAssertEqual(round3(TimelineScrub.y(of: t, in: layout)), probe["y"] as? Double,
                               "\(name): y(of: \(t))")
            }
            for probe in try XCTUnwrap(expected["offset_probes"] as? [[String: Any]]) {
                let y = try XCTUnwrap(probe["y"] as? Double)
                XCTAssertEqual(TimelineScrub.time(atY: y, in: layout), probe["t"] as? Int,
                               "\(name): time(atY: \(y))")
            }
        }
    }

    func testFormattingMatchesTheGeneratedFixture() throws {
        let fx = try parityFixture()
        let f = try XCTUnwrap(fx["formatting"] as? [String: Any])

        for (raw, want) in try XCTUnwrap(f["humanize"] as? [String: String]) {
            XCTAssertEqual(TimelineScrub.humanize(raw), want, "humanize(\(raw))")
        }
        for v in try XCTUnwrap(f["hour_minute"] as? [[String: Any]]) {
            let t = try XCTUnwrap(v["t"] as? Int)
            XCTAssertEqual(TimelineScrub.hourMinute(t), v["s"] as? String, "hourMinute(\(t))")
        }
        for v in try XCTUnwrap(f["bucket_range"] as? [[String: Any]]) {
            let t = try XCTUnwrap(v["t"] as? Int)
            let size = try XCTUnwrap(v["size"] as? Int)
            XCTAssertEqual(TimelineScrub.bucketRange(t, size), v["s"] as? String, "bucketRange(\(t))")
        }
        for v in try XCTUnwrap(f["day_label"] as? [[String: Any]]) {
            let t = try XCTUnwrap(v["t"] as? Int)
            XCTAssertEqual(TimelineScrub.dayLabel(t), v["s"] as? String, "dayLabel(\(t))")
        }
        for v in try XCTUnwrap(f["duration"] as? [[String: Any]]) {
            let s = try XCTUnwrap(v["s"] as? Int)
            XCTAssertEqual(TimelineScrub.duration(s), v["out"] as? String, "duration(\(s))")
        }
        for v in try XCTUnwrap(f["greek_width"] as? [[String: Any]]) {
            let label = try XCTUnwrap(v["label"] as? String)
            XCTAssertEqual(TimelineScrub.greekWidth(label), v["w"] as? Double, "greekWidth(\(label))")
        }
        let indents = try XCTUnwrap(f["zone_indent"] as? [[String: Any]])
        let zones = indents.compactMap { $0["zone"] as? String }.filter { $0 != "zone:missing" }
        for v in indents {
            let zone = try XCTUnwrap(v["zone"] as? String)
            XCTAssertEqual(TimelineScrub.zoneIndent(zones, zone), v["indent"] as? Double,
                           "zoneIndent(\(zone))")
        }
    }

    // MARK: - properties the fixture cannot express

    func testEveryDictionaryEventHasATimelineFamily() {
        // A new event type must not fall into an unlabeled color: the switch in
        // `family(for:)` is exhaustive over WitnessEvent, so adding a case to
        // the dictionary breaks the build here rather than shipping a blank mark.
        for kind in WitnessEvent.allCases {
            let family = TimelineScrub.family(for: kind)
            XCTAssertFalse(family.label.isEmpty, "\(kind.rawValue) needs a legend label")
        }
        XCTAssertEqual(TimelineScrub.family(for: .tamperDetected), .tamper,
                       "tamper must keep its reserved status role")
    }

    func testADeclaredGapIsNeverInsideAFold() {
        // The invariant the whole design turns on: quiet may fold, a declared
        // blind spot may not. Checked directly, not only through the fixture.
        let base = 1_750_809_600
        let recs = [
            TimelineRecord(t0: base, kind: .event, label: "Contact state change", family: .touch),
            TimelineRecord(t0: base + 9 * 3600, kind: .gap, label: "Power loss", family: .gap),
            TimelineRecord(t0: base + 20 * 3600, kind: .event, label: "Vehicle arrival/departure", family: .move),
        ]
        let model = TimelineScrub.model(for: recs)
        let gapRecord = recs[1]
        for segment in model.segments where segment.kind == .fold {
            XCTAssertFalse(gapRecord.t0 >= segment.t0 && gapRecord.t0 < segment.t1,
                           "a declared gap was folded out of sight")
        }
        // …and it still reaches the reading pane.
        let listed = model.items.contains { item in
            if case .record(let r, _) = item { return r.kind == .gap }
            return false
        }
        XCTAssertTrue(listed, "the gap must appear as a row")
    }

    func testSortIsStableWithinABucket() {
        // JS sorts stably by spec and Swift does not, so ties in one bucket
        // would silently reorder between the two implementations.
        let t = 1_750_809_600
        let recs = (0..<8).map {
            TimelineRecord(t0: t, kind: .event, label: "Event \($0)", family: .move)
        }
        XCTAssertEqual(TimelineScrub.sorted(recs).map(\.label), recs.map(\.label))
    }

    func testDaysGroupByTheSuppliedCalendarNotAlwaysUTC() throws {
        // The phone's ribbon must agree with the Alerts list under it, which
        // groups with Calendar.current. Grouping by UTC put a Sunday-evening
        // Pacific alert under a "Monday" heading above the Sunday rows.
        var pacific = Calendar(identifier: .gregorian)
        pacific.timeZone = try XCTUnwrap(TimeZone(identifier: "America/Los_Angeles"))

        // 2025-06-30 01:00 UTC == 2025-06-29 18:00 Pacific — a Sunday evening
        // that UTC calls Monday.
        let sundayEveningPacific = 1_751_245_200
        let record = TimelineRecord(t0: sundayEveningPacific, kind: .event,
                                    label: "Contact state change", family: .touch)

        let utcDays = TimelineScrub.days(for: [record])
        XCTAssertEqual(utcDays.count, 1)
        XCTAssertTrue(utcDays[0].label.hasPrefix("Monday"), "UTC default unchanged: \(utcDays[0].label)")

        let localDays = TimelineScrub.days(for: [record], calendar: pacific)
        XCTAssertEqual(localDays.count, 1)
        XCTAssertTrue(localDays[0].label.hasPrefix("Sunday"),
                      "the phone must group by the user's day, got \(localDays[0].label)")

        // The day starts at LOCAL midnight, and the record sits inside it.
        let dayT0 = localDays[0].dayT0
        XCTAssertEqual(dayT0, Int(pacific.startOfDay(
            for: Date(timeIntervalSince1970: TimeInterval(sundayEveningPacific))).timeIntervalSince1970))
        XCTAssertGreaterThanOrEqual(record.t0, dayT0)
        XCTAssertLessThan(record.t0, dayT0 + TimelineScrub.daySeconds)
        // …and its cell lands in the evening, not at the head of the strip.
        let cell = try XCTUnwrap(localDays[0].cells.first)
        XCTAssertEqual(cell.index, (record.t0 - dayT0) / TimelineScrub.defaultBucketSeconds)
        XCTAssertGreaterThan(cell.index, localDays[0].cellsPerDay / 2, "18:00 is in the back half of the day")
    }

    func testTheRibbonAndTheAlertsListAgreeOnWhichDayARecordBelongsTo() {
        // The two groupings are written in different places, so pin that they
        // answer the same question: same calendar in, same day boundary out.
        let calendar = Calendar.current
        let now = Date()
        let alerts = (0..<12).map { i in
            AlertRecord(id: "a\(i)", witnessID: "w", name: "Canary",
                        severity: .notice, headline: "Something crossed the boundary",
                        at: now.addingTimeInterval(Double(i) * -7200))
        }
        let model = TimelineScrub.model(for: TimelineScrub.records(from: alerts), calendar: calendar)
        let sectionDays = Set(AlertHistory.daySections(alerts, calendar: calendar)
            .map { Int($0.day.timeIntervalSince1970) })
        let ribbonDays = Set(model.days.map(\.dayT0))
        XCTAssertEqual(ribbonDays, sectionDays,
                       "the ribbon's days and the list's sections must be the same days")
    }

    func testHeartbeatsNeverLightADensityCell() {
        let base = 1_750_809_600
        let recs = [
            TimelineRecord(t0: base, kind: .event, label: "Contact state change", family: .touch),
            TimelineRecord(t0: base + 600, kind: .heartbeat, label: "Heartbeat", family: .other),
        ]
        let days = TimelineScrub.days(for: recs)
        XCTAssertEqual(days.count, 1)
        XCTAssertEqual(days[0].count, 1, "a heartbeat is proof of life, not activity")
        XCTAssertEqual(days[0].cells.count, 1)
    }

    func testEmptyInputProducesAnEmptyModelRatherThanACrash() {
        let model = TimelineScrub.model(for: [])
        XCTAssertTrue(model.isEmpty)
        XCTAssertTrue(model.segments.isEmpty)
        XCTAssertTrue(model.items.isEmpty)
        XCTAssertEqual(TimelineScrub.y(of: 0, in: []), 0)
        XCTAssertEqual(TimelineScrub.time(atY: 10, in: []), 0)
    }
}
