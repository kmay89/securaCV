// TimelineScrubView.swift
//
// The day, as a shape you can drag.
//
// The Alerts tab answers "did something need me?" one row at a time, which is
// the right answer to the wrong question at 7am: the thing people actually do
// with a day of alerts is GLANCE at it. So this is the phone's half of the
// same idea as the offline viewer's minimap — a day drawn as one ribbon, dense
// where the day was busy, blank where the export never covered, and hatched
// where the device declared it could not see. You read the shape first and the
// words only if the shape asks you to.
//
// Three rules it inherits, none of them negotiable:
//
//   1. COARSE TIME ONLY. Every column is a 10-minute bucket (Invariant III).
//      The scrubber snaps to buckets because finer timing was never recorded —
//      a smooth continuous scrub would be a privacy regression wearing a UI hat.
//   2. WORST-FIRST. A column reports the most serious thing in it, never an
//      average, and tamper keeps its own reserved role. A busy hour must never
//      dilute the one thing in it that mattered.
//   3. UNCOVERED IS NOT QUIET. Hours outside the covered window render as
//      absent, not as a flat "nothing happened" — the same distinction the
//      glass histogram makes when the clock was not yet known.
//
// The arithmetic lives in Shared/TimelineScrub.swift (pure, host-tested, and
// pinned against the JavaScript implementation by
// viewer/fixtures/timeline/scrub_parity.json). This file is the thin shell:
// it draws and it handles input, and it decides nothing about what a day was.

import Accessibility
import SwiftUI

/// Audio Graph twin of the ribbon: the same worst-first day, spoken and
/// sonified through VoiceOver's rotor. Dormant for everyone else — no render
/// cost, no gesture interplay. Honesty falls out of the data shape: points
/// exist only for buckets that hold cells (a silent bucket is ABSENT data,
/// never a zero — this feed declares no coverage), and declared gaps ride a
/// second, named series so a blind spot is announced as one, the audio twin
/// of the outline-not-fill drawing rule.
private struct TimelineDayAudioGraph: AXChartDescriptorRepresentable {
    let day: TimelineDay

    func makeChartDescriptor() -> AXChartDescriptor {
        let day = self.day // value copy for the escaping providers
        func clock(_ index: Int) -> String {
            let start = Date(timeIntervalSince1970:
                TimeInterval(day.dayT0 + index * TimelineScrub.defaultBucketSeconds))
            let end = start.addingTimeInterval(TimeInterval(TimelineScrub.defaultBucketSeconds))
            return start.formatted(.dateTime.hour().minute())
                + " to " + end.formatted(.dateTime.hour().minute())
        }
        let xAxis = AXNumericDataAxisDescriptor(
            title: "Time of day, in 10-minute buckets",
            range: 0...Double(max(day.cellsPerDay - 1, 1)),
            gridlinePositions: [36, 72, 108], // 6:00, 12:00, 18:00 — the ribbon's own tall ticks
            valueDescriptionProvider: { clock(Int($0.rounded())) })
        let busiest = Double(max(day.cells.map(\.count).max() ?? 1, 1))
        // "Alert records", never "sealed": this ribbon is fed by the phone's
        // alert notebook (AlertLedger), not the device's sealed witness
        // chain — an integrity claim the notebook cannot make (AGENTS.md
        // rule 4), and the device-detail footer already draws that line.
        let yAxis = AXNumericDataAxisDescriptor(
            title: "Alert records",
            range: 0...busiest,
            gridlinePositions: [],
            valueDescriptionProvider: { value in
                let n = Int(value.rounded())
                return n == 1 ? "1 alert record" : "\(n) alert records"
            })
        let recorded = day.cells.filter { !$0.hasGap }.map { cell in
            AXDataPoint(x: Double(cell.index), y: Double(cell.count),
                        additionalValues: [], label: clock(cell.index))
        }
        let gaps = day.cells.filter(\.hasGap).map { cell in
            AXDataPoint(x: Double(cell.index), y: Double(cell.count),
                        additionalValues: [], label: "Declared gap, " + clock(cell.index))
        }
        var series = [AXDataSeriesDescriptor(name: "Alert records per 10 minutes",
                                             isContinuous: false, dataPoints: recorded)]
        if !gaps.isEmpty {
            series.append(AXDataSeriesDescriptor(
                name: "Declared gaps — blind spots the device reported",
                isContinuous: false, dataPoints: gaps))
        }
        var summary = day.count == 1
            ? "1 alert record, from this phone's notebook."
            : "\(day.count) alert records, from this phone's notebook."
        if day.gapCount > 0 {
            summary += " \(day.gapCount) declared gap\(day.gapCount == 1 ? "" : "s")"
                + " — buckets where the device said it could not see. Blind spots, not quiet."
        }
        if day.coverageFrom == nil {
            summary += " This feed declares no coverage window:"
                + " buckets without records are absent data, not proof of quiet."
        }
        return AXChartDescriptor(title: "Shape of \(day.label)", summary: summary,
                                 xAxis: xAxis, yAxis: yAxis,
                                 additionalAxes: [], series: series)
    }

    // SwiftUI reuses the descriptor across view updates: when the ledger
    // changes while the ribbon stays mounted, this — not makeChartDescriptor
    // — is what runs, and the protocol's do-nothing default would leave
    // VoiceOver reading yesterday's data off today's ribbon.
    func updateChartDescriptor(_ descriptor: AXChartDescriptor) {
        let fresh = makeChartDescriptor()
        descriptor.title = fresh.title
        descriptor.summary = fresh.summary
        descriptor.xAxis = fresh.xAxis
        descriptor.yAxis = fresh.yAxis
        descriptor.series = fresh.series
    }
}

/// Once-per-app-session latch for the gesture-hint shimmer. MainActor
/// isolation makes the mutable static legal under strict concurrency; body
/// and `.task` both run on the main actor.
@MainActor private enum TimelineHintFlag { static var shown = false }

/// Which day is being scrubbed, and where. Scoped to a day on purpose: one
/// shared `Date` made every OTHER ribbon print and announce a time that was
/// not its own — VoiceOver read "Sunday … at 2:30 PM" off a bucket the user
/// had picked on Monday.
struct TimelineScrubPosition: Equatable {
    var dayT0: Int
    var bucket: Date
}

/// One day drawn as a draggable ribbon of 10-minute columns.
struct TimelineDayShapeView: View {
    let day: TimelineDay
    /// The active scrub across ALL ribbons; this one reads it only when it
    /// names this day (see `myBucket`).
    @Binding var position: TimelineScrubPosition?
    /// This day's first tamper bucket, derived by the host FROM THE RECORDS.
    /// The cells cannot answer it: a bucket where a tamper shares ten minutes
    /// with a declared gap is family `.gap` by the worst-first rule, so a
    /// cell scan misses exactly the "lens covered, then pried" pairing that
    /// matters most. Nil when the day holds no tamper records.
    var firstTamperBucket: Date?
    /// Fired whenever an interaction commits a bucket — drag release, tap,
    /// the VoiceOver adjustable action, and both jump paths — always already
    /// snapped, so the caller can bring the matching row into view.
    var onScrub: (Date) -> Void
    /// The newest ribbon shows the one-time gesture-hint shimmer.
    var showsFirstUseHint = false

    /// The scrub position if it belongs to this day, otherwise nil.
    private var myBucket: Date? {
        guard let p = position, p.dayT0 == day.dayT0 else { return nil }
        return p.bucket
    }

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.colorSchemeContrast) private var contrast
    /// Dynamic Type moves the ribbon with the text it sits beside — a
    /// larger-text user gets a proportionally taller, easier target.
    @ScaledMetric(relativeTo: .caption) private var ribbonHeight: CGFloat = 44

    /// The live drag, in ribbon coordinates. `virtualX` is where the scrub
    /// IS; `lastX` is where the finger was — they separate so pulling the
    /// finger down can slow the scrub (see `gain`) without the caret leaping
    /// back to the fingertip. Non-nil exactly while a drag owns the ribbon.
    @State private var scrub: ScrubDrag?
    /// 0 = full speed, 1 = half, 2 = quarter — chosen by how far the finger
    /// has pulled down from the ribbon, the Music-scrubber gesture. Gain
    /// changes input speed only: every output still snaps to a bucket, so
    /// "fine" means reliably landing on ONE 10-minute bucket, never between.
    @State private var gainTier = 0
    /// Bumped by every seat and every scheduled clear, so a pending
    /// caret-clear task can tell "still my landing" from "someone landed
    /// here again" without comparing position values.
    @State private var caretGen = 0
    /// Non-nil only while the one-time gesture-hint shimmer runs (see the
    /// `.task` below). Purely visual — it never touches `position`.
    @State private var hintX: CGFloat?

    private struct ScrubDrag {
        var lastX: CGFloat
        var virtualX: CGFloat
    }

    /// Columns are keyed by index into the day, so a lookup is a dictionary
    /// hit rather than a scan per drawn column. Indices are unique by
    /// construction; `uniquingKeysWith` keeps a malformed model from trapping
    /// inside a view body rather than merely drawing oddly.
    private var cellsByIndex: [Int: TimelineDayCell] {
        Dictionary(day.cells.map { ($0.index, $0) }, uniquingKeysWith: { first, _ in first })
    }

    private var busiest: Int { max(day.cells.map(\.count).max() ?? 1, 1) }

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.xs) {
            header
            GeometryReader { geo in
                let width = max(geo.size.width, 1)
                ZStack(alignment: .topLeading) {
                    Canvas { context, size in
                        draw(in: context, size: size)
                    }
                    .accessibilityHidden(true)

                    if let bucket = myBucket, let x = caretX(for: bucket, width: width) {
                        Rectangle()
                            .fill(Theme.color(.info))
                            .frame(width: 2)
                            .offset(x: x - 1 + edgeOvershoot(width: width))
                            .animation(reduceMotion ? nil : .interactiveSpring(), value: bucket)
                            // Springs the overshoot home when the drag ends.
                            .animation(reduceMotion ? nil : .spring(response: 0.3, dampingFraction: 0.7),
                                       value: scrub == nil)
                            .accessibilityHidden(true)
                    }
                    if let hx = hintX {
                        // The one-time "this drags" shimmer — a ghost caret,
                        // never a seated position: it goes nowhere near
                        // seat()/position, so it announces nothing, fires no
                        // detent haptic, and a real touch dismisses it.
                        Capsule()
                            .fill(Theme.color(.info).opacity(0.35))
                            .frame(width: 3)
                            .offset(x: hx)
                            .animation(.easeInOut(duration: 0.13), value: hx)
                            .accessibilityHidden(true)
                    }
                }
                .contentShape(Rectangle())
                .onTapGesture { location in
                    // The most natural gesture of all: tap the peak, land on
                    // it. seat() snaps, so a tap can never claim a finer
                    // moment than the bucket it touched.
                    hintX = nil // a real touch outranks the hint
                    preview(atX: location.x, width: width)
                    if let bucket = myBucket { onScrub(bucket) }
                    scheduleCaretClear()
                }
                .gesture(
                    // `minimumDistance: 0` claimed the touch on touch-DOWN, so
                    // three 44pt bands of the Alerts list stopped scrolling:
                    // a flick that began on a ribbon scrubbed instead. Require
                    // real movement, and only take the gesture once it is
                    // mostly sideways — a vertical flick belongs to the list
                    // this ribbon is a row of. Once the drag IS claimed,
                    // vertical distance stops meaning "scroll" and starts
                    // meaning "slower": pulling down drops the horizontal
                    // gain to half, then a quarter, the same way the system
                    // media scrubber does.
                    DragGesture(minimumDistance: 8)
                        .onChanged { value in
                            hintX = nil // a real touch outranks the hint
                            if scrub == nil {
                                guard abs(value.translation.width) > abs(value.translation.height) else { return }
                                scrub = ScrubDrag(lastX: value.location.x, virtualX: value.location.x)
                            }
                            guard var drag = scrub else { return }
                            let pull = max(value.translation.height, 0)
                            let tier = pull > 120 ? 2 : pull > 60 ? 1 : 0
                            if tier != gainTier { gainTier = tier }
                            let gain: CGFloat = tier == 2 ? 0.25 : tier == 1 ? 0.5 : 1
                            // The clamp keeps a little slack past the day's
                            // edges so the caret can rubber-band (see
                            // edgeOvershoot); the SEATED bucket still clamps
                            // hard inside preview — the overshoot is caret
                            // travel only, never a time the day doesn't hold.
                            let slack: CGFloat = reduceMotion ? 0 : 80
                            drag.virtualX = min(max(drag.virtualX + (value.location.x - drag.lastX) * gain, -slack), width + slack)
                            drag.lastX = value.location.x
                            scrub = drag
                            preview(atX: drag.virtualX, width: width)
                        }
                        .onEnded { _ in
                            // Scrolling DURING the drag moved the ribbon out
                            // from under the finger — it is a row of the very
                            // list it scrolls. So the drag only previews, and
                            // the list is moved once, on release.
                            if scrub != nil, let bucket = myBucket {
                                onScrub(bucket)
                            }
                            scrub = nil
                            gainTier = 0
                            scheduleCaretClear()
                        }
                )
                // The bucket IS the detent, felt as well as seen: a tick when
                // the caret crosses into a bucket that holds records, a firmer
                // nudge entering a declared gap or a tamper bucket — the
                // tactile twin of the outline-not-fill rule. Silence over
                // empty buckets, and nothing scaled by severity or count: a
                // scrub must never become a Geiger counter.
                .sensoryFeedback(trigger: myBucket) { _, seated in
                    guard let bucket = seated, let cell = cell(at: bucket) else { return nil }
                    if cell.hasGap || cell.family == .tamper { return .impact(weight: .light) }
                    return .selection
                }
                // Once per app session, on the newest ribbon: a ghost caret
                // drifts across two buckets and fades — "this drags", said in
                // motion, no tutorial. Deliberately silent: the haptic
                // vocabulary fires only under a real finger, and this is not
                // one. A drag or tap that starts mid-shimmer dismisses it.
                .task {
                    guard showsFirstUseHint, !reduceMotion, !TimelineHintFlag.shown else { return }
                    TimelineHintFlag.shown = true
                    let colW = width / CGFloat(max(day.cellsPerDay, 1))
                    let start = day.cells.max(by: { $0.count < $1.count })?.index ?? day.cellsPerDay / 2
                    hintX = CGFloat(start) * colW
                    for step in 1...2 {
                        try? await Task.sleep(nanoseconds: 140_000_000)
                        guard hintX != nil else { return } // a real gesture won
                        hintX = CGFloat(min(start + step, day.cellsPerDay - 1)) * colW
                    }
                    try? await Task.sleep(nanoseconds: 250_000_000)
                    hintX = nil
                }
            }
            .frame(height: ribbonHeight)
            .accessibilityElement()
            .accessibilityLabel("Shape of \(day.label)")
            .accessibilityValue(spokenSummary)
            .accessibilityHint("Swipe up or down to move through the day by the hour.")
            .accessibilityAdjustableAction { direction in
                adjust(by: direction == .increment ? 3600 : -3600)
            }
            .accessibilityActions {
                // The records worth the trip get one-gesture routes: the same
                // jumps the sighted footer buttons offer. Navigation order,
                // never a filter — the full day stays exactly as spoken.
                if day.tamperCount > 0 {
                    Button("Jump to tamper") { jump(toTamper: true) }
                }
                if day.gapCount > 0 {
                    Button("Jump to declared gap") { jump(toTamper: false) }
                }
            }
            // VoiceOver's Audio Graphs rotor: the day's shape, sonified.
            .accessibilityChartDescriptor(TimelineDayAudioGraph(day: day))
            footer
        }
    }

    // MARK: - chrome

    private var header: some View {
        HStack(alignment: .firstTextBaseline, spacing: Theme.s) {
            Text(day.label)
                .font(.subheadline.weight(.semibold))
            Spacer(minLength: Theme.xs)
            if gainTier > 0 {
                // Names the gear the finger already felt shift, exactly as the
                // system media scrubber labels its speed while dragging.
                Text(gainTier == 2 ? "¼ speed" : "½ speed")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            if let bucket = myBucket {
                Text(Self.clock(bucket))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(Theme.color(.info))
            }
        }
    }

    /// The one honest sentence about the day, in words — so the shape is never
    /// the only way to get the answer (and VoiceOver, print, and a screenshot
    /// all carry it).
    private var footer: some View {
        HStack(spacing: Theme.s) {
            Text(countPhrase)
            if day.gapCount > 0 {
                // The counts are destinations, not captions: tapping one
                // scrubs straight to the first record behind the number.
                Button { jump(toTamper: false) } label: {
                    Label("\(day.gapCount) gap\(day.gapCount == 1 ? "" : "s")", systemImage: "eye.slash")
                        .foregroundStyle(Theme.color(.warn))
                }
                .buttonStyle(.plain)
            }
            if day.tamperCount > 0 {
                Button { jump(toTamper: true) } label: {
                    Label("\(day.tamperCount) tamper", systemImage: "hand.raised.slash.fill")
                        .foregroundStyle(Theme.color(.tamper))
                }
                .buttonStyle(.plain)
            }
        }
        .font(.caption)
        .foregroundStyle(.secondary)
        .accessibilityHidden(true) // the ribbon speaks this, and carries the same jumps as custom actions
    }

    private var countPhrase: String {
        day.count == 1 ? "1 record" : "\(day.count) records"
    }

    private var spokenSummary: String {
        var parts = [countPhrase]
        if let raw = day.worstSeverityRaw {
            parts.append("worst: " + Severity(tolerant: Int(raw)).label)
        }
        if day.gapCount > 0 { parts.append("\(day.gapCount) declared gap\(day.gapCount == 1 ? "" : "s")") }
        // Spoken explicitly: the footer is accessibility-hidden on the
        // promise that the ribbon says everything it shows, and "worst:
        // tamper" only covers the case where tamper happens to be worst.
        if day.tamperCount > 0 { parts.append("\(day.tamperCount) tamper") }
        if let bucket = myBucket { parts.append("at " + Self.clock(bucket)) }
        return parts.joined(separator: ", ")
    }

    // MARK: - drawing

    private func draw(in context: GraphicsContext, size: CGSize) {
        let columns = max(day.cellsPerDay, 1)
        let colWidth = size.width / CGFloat(columns)
        let barWidth = max(colWidth - 0.5, 0.75)

        // The covered window: the ONLY region allowed to read as "quiet",
        // and only when the source actually declares one. The alert ledger
        // declares nothing, so no track is drawn there — inferring coverage
        // from where the records happen to fall would claim the device was
        // watching on exactly the evidence that cannot show it.
        if let from = day.coverageFrom, let to = day.coverageTo {
            let coverX0 = size.width * CGFloat(from)
            let coverX1 = size.width * CGFloat(to)
            if coverX1 > coverX0 {
                context.fill(
                    Path(roundedRect: CGRect(x: coverX0, y: size.height - 3,
                                             width: coverX1 - coverX0, height: 3),
                         cornerRadius: 1.5),
                    with: .color(Theme.color(.neutral).opacity(0.28)))
            }
        }

        // Faint hour ticks along the baseline, a touch stronger at 6/12/18,
        // so a scrub toward "around 3pm" is aimable. Hours are the visual
        // grid; the felt grid stays the 10-minute bucket — ticking all 144
        // would promise a pointing precision no fingertip has.
        for hour in 1..<24 {
            let x = size.width * CGFloat(hour) / 24
            let tall: CGFloat = hour % 6 == 0 ? 5 : 3
            context.fill(
                Path(CGRect(x: x - 0.5, y: size.height - tall, width: 1, height: tall)),
                with: .color(Theme.color(.neutral).opacity(hour % 6 == 0 ? 0.4 : 0.22)))
        }

        let cells = cellsByIndex
        for index in 0..<columns {
            guard let cell = cells[index] else { continue }
            let x = CGFloat(index) * colWidth
            // Height is the column's count against the day's busiest column,
            // with a floor so a single record is never invisible.
            let ratio = CGFloat(cell.count) / CGFloat(busiest)
            let height = max(4, (size.height - 4) * (0.35 + 0.65 * ratio))
            let rect = CGRect(x: x, y: size.height - 3 - height, width: barWidth, height: height)
            let path = Path(roundedRect: rect, cornerRadius: min(1.5, barWidth / 2))

            if cell.hasGap {
                // A declared blind spot is drawn as an OUTLINE, not a fill:
                // shape carries the meaning so it survives grayscale, color
                // blindness, and a black-and-white printout.
                context.stroke(path, with: .color(Theme.color(.warn)), lineWidth: 1)
            } else {
                context.fill(path, with: .color(color(for: cell)))
            }
        }
    }

    /// Worst-first, in the app's established color vocabulary: severity picks
    /// a `Theme.Role`. Records with no severity at all (a sealed-log export)
    /// fall back to the neutral role rather than borrowing a status color.
    private func color(for cell: TimelineDayCell) -> Color {
        if cell.family == .tamper { return Theme.color(.tamper) }
        guard let raw = cell.worstSeverityRaw else { return Theme.color(.neutral) }
        let role = Severity(tolerant: Int(raw)).role
        // Under increased contrast, a calm column would fade into the track;
        // lift it to the informational role so every drawn column stays read.
        if contrast == .increased && role == .calm { return Theme.color(.info) }
        return Theme.color(role)
    }

    // MARK: - input

    /// The day's cell under a bucket date, if that bucket holds records.
    private func cell(at bucket: Date) -> TimelineDayCell? {
        let offset = bucket.timeIntervalSince1970 - TimeInterval(day.dayT0)
        guard offset >= 0, offset < TimeInterval(TimelineScrub.daySeconds) else { return nil }
        return cellsByIndex[Int(offset) / TimelineScrub.defaultBucketSeconds]
    }

    /// Scrub straight to the day's first tamper (or first declared-gap)
    /// bucket and walk the list there — the footer counts and the VoiceOver
    /// custom actions both land here. Tamper comes from `firstTamperBucket`
    /// (see its comment); gaps have a dedicated per-cell flag, so a cell scan
    /// is exact for them.
    private func jump(toTamper: Bool) {
        let bucket: Date? = toTamper
            ? firstTamperBucket
            : day.cells.filter(\.hasGap).map(\.index).min().map {
                Date(timeIntervalSince1970:
                    TimeInterval(day.dayT0 + $0 * TimelineScrub.defaultBucketSeconds))
            }
        guard let bucket else { return }
        position = TimelineScrubPosition(dayT0: day.dayT0, bucket: bucket)
        onScrub(bucket)
        scheduleCaretClear()
    }

    /// The finger lifts, the list glides — and only then does the caret bow
    /// out. Clearing it the same frame erased the answer ("where did I land?")
    /// at the exact moment the question was asked. The token is a GENERATION,
    /// never the position value: two landings on the same bucket compare
    /// equal, so a value check let the first landing's timer erase the
    /// second's caret — or void a live drag resting on the same bucket.
    private func scheduleCaretClear() {
        caretGen += 1
        let generation = caretGen
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 900_000_000)
            // `position` is shared across every ribbon, but the generation
            // and drag state are this ribbon's own — so also require the
            // caret to still be OURS. Without that, landing here and then
            // scrubbing a second day within 900ms let this timer erase the
            // other ribbon's live caret.
            if caretGen == generation, scrub == nil, position?.dayT0 == day.dayT0 { position = nil }
        }
    }

    private func caretX(for bucket: Date, width: CGFloat) -> CGFloat? {
        let offset = bucket.timeIntervalSince1970 - TimeInterval(day.dayT0)
        guard offset >= 0, offset < TimeInterval(TimelineScrub.daySeconds) else { return nil }
        return width * CGFloat(offset / TimeInterval(TimelineScrub.daySeconds))
    }

    /// Cosmetic caret overshoot past the day's 00:00/24:00 edges — the WWDC
    /// rubber-band: the boundary acknowledges the pull instead of going dead.
    /// NEVER applies at declared-coverage edges; nothing clamps there, so
    /// uncovered time stays freely scrubbable by construction.
    private func edgeOvershoot(width: CGFloat) -> CGFloat {
        guard !reduceMotion, let drag = scrub else { return 0 }
        if drag.virtualX < 0 { return drag.virtualX * 0.15 }
        if drag.virtualX > width { return (drag.virtualX - width) * 0.15 }
        return 0
    }

    /// Moves the caret and the header clock only — the list is left alone
    /// until the drag ends.
    private func preview(atX x: CGFloat, width: CGFloat) {
        let fraction = min(max(x / width, 0), 1)
        seat(secondsIntoDay: Double(TimelineScrub.daySeconds) * Double(fraction))
    }

    /// VoiceOver's adjustable action. Not a drag, so it moves the list
    /// immediately — that is the whole point of the gesture.
    private func adjust(by delta: TimeInterval) {
        let current = myBucket?.timeIntervalSince1970
            ?? TimeInterval(day.dayT0) + Double(TimelineScrub.daySeconds) / 2
        seat(secondsIntoDay: current - TimeInterval(day.dayT0) + delta)
        if let bucket = myBucket { onScrub(bucket) }
    }

    /// Snap to a bucket start and report it. Snapping is the point, not a
    /// rounding convenience: the log holds buckets, so the scrubber may not
    /// offer a position the record cannot justify.
    private func seat(secondsIntoDay: Double) {
        caretGen += 1 // a fresh seating voids any pending caret-clear
        let clamped = min(max(secondsIntoDay, 0), Double(TimelineScrub.daySeconds - 1))
        let bucketSize = Double(TimelineScrub.defaultBucketSeconds)
        let snapped = (clamped / bucketSize).rounded(.down) * bucketSize
        let date = Date(timeIntervalSince1970: TimeInterval(day.dayT0) + snapped)
        position = TimelineScrubPosition(dayT0: day.dayT0, bucket: date)
    }

    /// A bucket is a RANGE, and saying "12:20" for it would narrow a
    /// ten-minute window the log deliberately left wide (Invariant III). The
    /// phone shows the user's own wall clock; the parity-checked UTC forms
    /// stay in the model for the record itself.
    private static func clock(_ date: Date) -> String {
        let end = date.addingTimeInterval(TimeInterval(TimelineScrub.defaultBucketSeconds))
        return date.formatted(.dateTime.hour().minute())
            + " – " + end.formatted(.dateTime.hour().minute())
    }
}

/// The Alerts tab's mount: one ribbon per day that has records, newest first,
/// scrubbing the list it sits above.
struct TimelineScrubSection: View {
    let records: [AlertRecord]
    /// Called with the bucket the user scrubbed to, so the host can scroll.
    var onScrub: (Date) -> Void

    @State private var position: TimelineScrubPosition?

    /// Built ONCE, at init. `position` is @State here, so every per-frame
    /// seat() during a drag re-runs this body — as a computed property the
    /// whole model (stable sort, segments, days, items) was being rebuilt on
    /// every frame of every scrub. Init runs only when the host's body does.
    ///
    /// `Calendar.current`, deliberately: the Alerts list below groups its
    /// sections with the user's own calendar (`AlertHistory.daySections`), and
    /// a ribbon grouped by UTC would put a Sunday-evening alert under a
    /// "Monday" heading sitting directly above the Sunday rows it scrolls to.
    /// The evidence viewer keeps the UTC default — there the record's own
    /// frame is the honest one, and the page says so.
    private let built: TimelineModel
    private let tamperBuckets: [Int: Date]

    init(records: [AlertRecord], onScrub: @escaping (Date) -> Void) {
        self.records = records
        self.onScrub = onScrub
        let model = TimelineScrub.model(for: TimelineScrub.records(from: records), calendar: .current)
        self.built = model
        self.tamperBuckets = Self.firstTamperBuckets(in: model, calendar: .current)
    }

    /// Earliest tamper bucket per calendar day, straight from the records —
    /// the one question the worst-first cells cannot answer (a tamper sharing
    /// a bucket with a declared gap lives in a `.gap`-family cell).
    private static func firstTamperBuckets(in model: TimelineModel, calendar: Calendar) -> [Int: Date] {
        var out: [Int: Date] = [:]
        for record in model.records where record.family == .tamper {
            let bucket = Date(timeIntervalSince1970: TimeInterval(record.t0))
            let dayT0 = Int(calendar.startOfDay(for: bucket).timeIntervalSince1970)
            if out[dayT0] == nil { out[dayT0] = bucket }
        }
        return out
    }

    var body: some View {
        let days = built.days.sorted { $0.dayT0 > $1.dayT0 }
        if !days.isEmpty {
            VStack(alignment: .leading, spacing: Theme.m) {
                ForEach(Array(days.prefix(3))) { day in
                    TimelineDayShapeView(day: day, position: $position,
                                         firstTamperBucket: tamperBuckets[day.dayT0],
                                         onScrub: onScrub,
                                         showsFirstUseHint: day.dayT0 == days.first?.dayT0)
                }
                if days.count > 3 {
                    // The ribbons stop at three; the record does not. Say so,
                    // or the boundary reads as the history ending here.
                    Text("Shapes for the 3 most recent days — earlier days continue in the list below.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.vertical, Theme.xs)
        }
    }
}

/// Preview data, built outside the preview body so the preview itself stays a
/// single expression (the house style, and it keeps the ViewBuilder happy).
private enum TimelineScrubPreviewData {
    static var records: [AlertRecord] {
        let start = Date().addingTimeInterval(-9 * 3600)
        let severities: [Severity] = [.ok, .notice, .notice, .warn, .notice, .tamper]
        return (0..<16).map { i in
            var record = AlertRecord(id: "sample-\(i)",
                                     witnessID: "canary-1",
                                     name: "Canary 1",
                                     severity: severities[i % severities.count],
                                     headline: "Something crossed the boundary",
                                     at: start.addingTimeInterval(Double(i) * 1800))
            record.count = i % 4 == 0 ? 3 : 1
            return record
        }
    }
}

#Preview("Day shape") {
    TimelineScrubSection(records: TimelineScrubPreviewData.records, onScrub: { _ in })
        .padding()
}
