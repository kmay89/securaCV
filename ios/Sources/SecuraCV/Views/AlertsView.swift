// AlertsView.swift
//
// The tab answers the question people actually open it with: **did something
// need me, and did it reach me?** It used to answer a different one — it was a
// rules editor wearing the word "Alerts", so the app's own history of alarms
// existed nowhere, and the reach picker promised an away path the app had not
// built. Both are fixed here: history is the tab, rules moved behind one
// plainly-worded button, and every claim about reach comes from AwayPush's
// real capability instead of a segmented control's optimism.
//
// The shape, top to bottom:
//   * The heartbeat card stays the hero — proof the path works BEFORE the
//     emergency is the most valuable thing this screen can show.
//   * "Needs you" — unhandled AND still happening, the only rows that may
//     ever feel urgent. A condition that cleared on its own files itself
//     into history; urgency is about the present, never a backlog chore.
//   * History, one section per day — so last Tuesday reads as last Tuesday,
//     not as an undifferentiated "Earlier". Rows the user never saw wear a
//     dot (and the app badge counts them) until the tab has been visited;
//     settled rows can be swiped away, and seen-and-settled history ages out
//     on its own after a month (AlertLedger.retention).
//   * When nothing has ever happened, the canary holds the empty state and
//     says so. A quiet fleet is the product working, and it should read like
//     an achievement rather than a blank screen.

import SwiftUI

struct AlertsView: View {
    @EnvironmentObject var store: FleetStore
    @State private var showingRules = false
    @State private var confirmingClear = false

    private var urgent: [AlertRecord] {
        store.alertLog.records.filter(\.needsYou)
    }
    private var history: [AlertDaySection] {
        AlertHistory.daySections(store.alertLog.records.filter { !$0.needsYou })
    }

    /// The row a scrubbed bucket should bring into view: the newest record at
    /// or before that bucket. Scrubbing into a quiet stretch therefore lands
    /// on the last thing that actually happened rather than doing nothing —
    /// the ledger is sparse, and a scrubber that only answers on exact hits
    /// feels broken.
    private func anchorID(for bucket: Date) -> String? {
        let target = bucket.timeIntervalSince1970
        // The ledger is newest-first, so the first row at or before the target
        // is the closest one behind it.
        let records = store.alertLog.records
        if let hit = records.first(where: { $0.lastBucket.timeIntervalSince1970 <= target }) {
            return hit.id
        }
        return records.last?.id
    }

    /// Land a deep link: consume the pending route and, when its anchor hint
    /// names a witness with records, bring that witness's newest row into
    /// view. A hint that matches nothing (a storm summary's thread, an
    /// unpaired id) lands on the tab itself — an anchor is a courtesy, and
    /// a courtesy that fails must fail into the right room, not a crash or
    /// a wrong scroll.
    private func consumeRoute(_ scroller: ScrollViewProxy) {
        guard case .alerts(let witnessID) = store.pendingRoute else { return }
        store.pendingRoute = nil
        guard let witnessID,
              let hit = store.alertLog.records.first(where: { $0.witnessID == witnessID })
        else { return }
        withAnimation { scroller.scrollTo(hit.id, anchor: .top) }
    }

    private static func dayTitle(_ day: Date) -> String {
        let cal = Calendar.current
        if cal.isDateInToday(day) { return "Earlier today" }
        if cal.isDateInYesterday(day) { return "Yesterday" }
        return day.formatted(.dateTime.weekday(.wide).month().day())
    }

    /// Computed once on entry, deliberately: the tab stamps everything seen
    /// when it closes, so re-reading it live would make the line vanish
    /// under the reader. It is a "here's what you missed", not a live gauge.
    @State private var awayLine: String?

    var body: some View {
        NavigationStack {
            ScrollViewReader { scroller in
            List {
                if let awayLine {
                    Section {
                        AwaySummaryRow(text: awayLine)
                            .listRowInsets(EdgeInsets())
                            .listRowBackground(Color.clear)
                    }
                }

                Section {
                    HeartbeatCard().listRowInsets(EdgeInsets())
                        .listRowBackground(Color.clear)
                }

                // The heartbeat proves *a* path works. This answers the
                // bigger question standing right behind it — how many paths
                // there are, and which ones are down — because the person
                // who thinks to ask it is standing on this screen.
                Section {
                    CoverageRow()
                }

                if store.alertLog.isQuiet {
                    Section {
                        QuietStateCard(trustDays: store.canaryTrustDays)
                            .listRowInsets(EdgeInsets())
                            .listRowBackground(Color.clear)
                    }
                }

                if !urgent.isEmpty {
                    Section("Needs you") {
                        ForEach(urgent) { record in
                            AlertRecordRow(record: record).id(record.id)
                        }
                    }
                }

                // The shape of the day, above the day itself. A list answers
                // "what happened" one row at a time; this answers "what KIND
                // of day was it" before you read a single row, and scrubbing
                // it walks the list underneath.
                if !store.alertLog.isQuiet {
                    Section {
                        TimelineScrubSection(records: store.alertLog.records) { bucket in
                            if let id = anchorID(for: bucket) {
                                // Animated, or the release of a scrub reads as
                                // a teleport — continuity is the confirmation.
                                withAnimation(.snappy) { scroller.scrollTo(id, anchor: .top) }
                            }
                        }
                        .listRowInsets(EdgeInsets())
                        .listRowBackground(Color.clear)
                    }
                }

                ForEach(history) { section in
                    Section(Self.dayTitle(section.day)) {
                        ForEach(section.records) { record in
                            AlertRecordRow(record: record).id(record.id)
                        }
                    }
                }
            }
            // The deep link's landing: attached to the List (inside the
            // reader's scope, which the outer modifiers are not) and run on
            // both doors — appear covers "route set, then tab switched";
            // onChange covers "already standing on the tab when the tap
            // landed". consumeRoute clears the route, so the pair is
            // idempotent.
            .onAppear { consumeRoute(scroller) }
            .onChange(of: store.pendingRoute) { _, _ in consumeRoute(scroller) }
            }
            .navigationTitle("Alerts")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Tell me when…") { showingRules = true }
                }
                if !store.alertLog.isQuiet {
                    ToolbarItem(placement: .topBarTrailing) {
                        Menu {
                            Button("Clear history…", role: .destructive) {
                                confirmingClear = true
                            }
                        } label: {
                            Label("More", systemImage: "ellipsis.circle")
                        }
                    }
                }
            }
            .confirmationDialog("Clear alert history?",
                                isPresented: $confirmingClear,
                                titleVisibility: .visible) {
                Button("Clear history", role: .destructive) {
                    store.clearAlertHistory()
                }
            } message: {
                // Honest about what it is and isn't: this is the phone's
                // notebook, not the fleet's sealed witness chain — and it
                // clears history, never a live alarm.
                Text("Removes this phone's settled alert history. Anything that still needs you stays, and the fleet's signed witness logs stay on your devices, untouched.")
            }
            .sheet(isPresented: $showingRules) {
                AlertRulesSheet(center: store.alerts).environmentObject(store)
            }
            // What you missed, answered before you have to scroll for it.
            .onAppear { awayLine = AlertFreshness.awaySummary(store.alertLog.records) }
            // Leaving the tab is "I've looked": the badge and the unseen
            // dots clear together. On the way out, not on the way in, so
            // rows don't reshuffle under the reader's thumb.
            .onDisappear {
                store.markAlertsSeen()
                awayLine = nil
            }
        }
    }
}

// MARK: - one thing that happened

struct AlertRecordRow: View {
    @EnvironmentObject var store: FleetStore
    let record: AlertRecord
    /// A swipe action can't open a menu, so the duration choice arrives as a
    /// dialog — the mute still gets its "how long?", which is the whole point
    /// of having durations at all.
    @State private var choosingSnooze = false

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.xs) {
            HStack(spacing: Theme.s) {
                Image(systemName: record.severity.sfSymbol)
                    .foregroundStyle(Theme.color(record.severity.role))
                    .accessibilityHidden(true)
                Text(record.name).font(.body.weight(.medium))
                if record.isUnseen {
                    // "You haven't looked at this yet" — the same count the
                    // app badge carries. Cleared by visiting, not by acking:
                    // seen and handled are different questions.
                    Circle().fill(Theme.color(.warn)).frame(width: 7, height: 7)
                        .accessibilityLabel("New")
                }
                Spacer(minLength: Theme.s)
                if record.count > 1 {
                    // The collapse, made visible: a flapping Canary is one
                    // line that says how often, never sixty lines.
                    Text("\(record.count)×")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }
            Text(record.headline)
                .font(.subheadline)
                .foregroundStyle(.secondary)
            HStack(spacing: Theme.s) {
                // Coarse by construction — the record only ever knew a
                // 10-minute bucket (Invariant III).
                Text(record.lastBucket, format: .relative(presentation: .named))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                // Still happening, or over? The one fact that decides how
                // this row should make the reader feel.
                if record.isOpen {
                    Label("Ongoing", systemImage: "waveform.path.ecg")
                        .font(.caption2)
                        .foregroundStyle(Theme.color(record.severity.role))
                        .labelStyle(.titleAndIcon)
                } else {
                    Label("Cleared", systemImage: "checkmark.circle")
                        .font(.caption2).foregroundStyle(Theme.color(.calm))
                        .labelStyle(.titleAndIcon)
                }
                ReachChip(delivery: record.delivery, reason: record.undeliveredReason)
                if record.handling == .acknowledged {
                    Label("Acknowledged", systemImage: "checkmark")
                        .font(.caption2).foregroundStyle(Theme.color(.calm))
                        .labelStyle(.titleAndIcon)
                } else if record.handling == .muted {
                    // A mute with a visible end. "Muted" alone is how a
                    // silence becomes one nobody remembers setting.
                    Label(mutedLabel, systemImage: "bell.slash")
                        .font(.caption2).foregroundStyle(.secondary)
                        .labelStyle(.titleAndIcon)
                }
                if record.wasEscalated {
                    // Rationed to the top tier, so it stays worth reading.
                    Label("Escalated", systemImage: "bell.and.waves.left.and.right")
                        .font(.caption2).foregroundStyle(Theme.color(.tamper))
                        .labelStyle(.titleAndIcon)
                }
            }
        }
        .padding(.vertical, 2)
        .swipeActions(edge: .leading, allowsFullSwipe: true) {
            if record.needsYou {
                Button {
                    store.acknowledgeAlert(for: record.witnessID)
                } label: { Label("Acknowledge", systemImage: "checkmark") }
                    .tint(Theme.color(.calm))
            }
        }
        .swipeActions(edge: .trailing) {
            if record.needsYou {
                Button {
                    choosingSnooze = true
                } label: { Label("Mute", systemImage: "bell.slash") }
                    .tint(Theme.color(.warn))
            } else {
                // Settled rows are the user's to discard. Live ones are not:
                // a condition that's still happening can be acked or muted,
                // never made to look like it didn't happen.
                Button(role: .destructive) {
                    store.dismissAlert(id: record.id)
                } label: { Label("Remove", systemImage: "trash") }
            }
        }
        .confirmationDialog("Quiet \(record.name) for how long?",
                            isPresented: $choosingSnooze,
                            titleVisibility: .visible) {
            SnoozeButtons(store: store, witnessID: record.witnessID)
        } message: {
            Text("Tamper and a failed signature still get through, however long you choose.")
        }
    }

    /// "Muted until 9:00 PM" when we know the end, plain "Muted" for a row
    /// written before mutes carried one.
    private var mutedLabel: String {
        guard let until = record.mutedUntil, until > Date() else { return "Muted" }
        return "Muted until \(until.formatted(date: .omitted, time: .shortened))"
    }
}

/// The three durations, offered wherever a mute is chosen — and only the ones
/// that make sense at this hour (MuteDuration.offered). One list, so the
/// phone's rows, the detail screen and the wrist can never drift on what
/// "until tonight" means.
struct SnoozeButtons: View {
    /// Handed in rather than read from the environment: a confirmation
    /// dialog's actions are presented in their own context, and environment
    /// objects have never propagated into it reliably — the failure mode is a
    /// crash on tap, in the one flow that exists to calm things down.
    let store: FleetStore
    let witnessID: String

    var body: some View {
        ForEach(MuteDuration.offered(at: Date())) { duration in
            Button(duration.title) { store.mute(witnessID, duration: duration) }
        }
    }
}

/// "Here's what you missed" — the recovery surface the industry leaves to
/// scrolling. Shown on entry, gone once the tab has been seen.
struct AwaySummaryRow: View {
    let text: String

    var body: some View {
        HStack(spacing: Theme.s) {
            Image(systemName: "clock.arrow.circlepath")
                .foregroundStyle(Theme.color(.info))
                .accessibilityHidden(true)
            Text(text).font(.subheadline)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, Theme.m)
        .padding(.vertical, Theme.s)
        .background(.ultraThinMaterial,
                    in: RoundedRectangle(cornerRadius: Theme.corner, style: .continuous))
        .accessibilityElement(children: .combine)
    }
}

/// How far this one actually got. The whole point of showing it: "we told you"
/// and "we couldn't tell you" must never look the same.
struct ReachChip: View {
    let delivery: AlertDelivery
    var reason: String?

    var body: some View {
        HStack(spacing: 3) {
            Image(systemName: delivery.sfSymbol)
            Text(delivery == .notDelivered ? (reason ?? delivery.label) : delivery.label)
        }
        .font(.caption2)
        .foregroundStyle(delivery == .notDelivered ? Theme.color(.warn) : .secondary)
        .accessibilityLabel(delivery == .notDelivered
                            ? "Not delivered. \(reason ?? "")"
                            : "Reached you \(delivery.label)")
    }
}

// MARK: - the state we want people to live in

struct QuietStateCard: View {
    var trustDays: Int

    var body: some View {
        VStack(spacing: Theme.s) {
            CanaryActor(face: .calm, height: 64)
            Text("Nothing has needed you.")
                .font(.headline)
            Text(trustDays >= 7
                 ? "\(trustDays) clean days together."
                 : "When something does, it lands here — and you'll know how it reached you.")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, Theme.l)
    }
}

// MARK: - "Tell me when…" (the rules, in plain words)

struct AlertRulesSheet: View {
    @EnvironmentObject var store: FleetStore
    @ObservedObject var center: AlertCenter
    @ObservedObject private var away = AwayPush.shared
    @ObservedObject private var household = HouseholdShare.shared
    @Environment(\.dismiss) private var dismiss
    @State private var showingHousehold = false

    var body: some View {
        NavigationStack {
            List {
                // The app noticing it's being annoying, and asking. Never
                // acting on its own — the counters are evidence, not a
                // mandate (AlertTuning).
                if let advice = store.tuningAdvice {
                    Section {
                        VStack(alignment: .leading, spacing: Theme.s) {
                            Text(advice.sentence).font(.subheadline)
                            Text(advice.question)
                                .font(.footnote).foregroundStyle(.secondary)
                            HStack {
                                Button("Stop pushing these") { store.applyTuning(advice) }
                                    .buttonStyle(.borderedProminent)
                                Button("Keep them") { store.declineTuning(advice) }
                                    .buttonStyle(.bordered)
                            }
                        }
                        .padding(.vertical, 2)
                    } header: {
                        Text("Noticed")
                    } footer: {
                        Text("Counted on this phone only — how often you act on each kind of alert, never what they were about. Nothing about this leaves your device.")
                    }
                }

                Section {
                    ForEach($center.rules) { $rule in
                        AlertRuleRow(rule: $rule, awayReach: away.reach)
                    }
                } header: {
                    Text("Tell me when")
                } footer: {
                    Text("We push only what you arm here, and only what matters — like a smoke alarm, silent until it isn't. Everyday activity stays in Today, never a buzz.")
                }

                Section {
                    Toggle("Quiet hours", isOn: $center.quietHours.enabled)
                    if center.quietHours.enabled {
                        DatePicker("From", selection: quietStart,
                                   displayedComponents: .hourAndMinute)
                        DatePicker("Until", selection: quietEnd,
                                   displayedComponents: .hourAndMinute)
                    }
                } header: {
                    Text("Quiet hours")
                } footer: {
                    Text("Everyday alerts wait in the app during these hours. Tamper and panic still come through — a smoke alarm that honors quiet hours isn't one.")
                }

                if store.narrowedWitnessCount > 0 {
                    Section {
                        Label(store.narrowedWitnessCount == 1
                              ? "1 Canary is set to tell you less than your rules would."
                              : "\(store.narrowedWitnessCount) Canaries are set to tell you less than your rules would.",
                              systemImage: "slider.horizontal.3")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    } footer: {
                        Text("Set per Canary on its own screen. A choice made months ago should never be an invisible reason an alert didn't arrive.")
                    }
                }

                // The last rung of the ladder, one tap away from the rules
                // that decide the rungs below it.
                Section {
                    Button { showingHousehold = true } label: {
                        HStack {
                            Label("If nobody answers", systemImage: "person.2")
                            Spacer()
                            Text(household.members.isEmpty ? "Nobody" : "\(household.joinedCount)")
                                .foregroundStyle(.secondary)
                        }
                    }
                } footer: {
                    Text(HouseholdRelay.summary(household.members))
                }

                Section {
                    Label(away.reach.explanation,
                          systemImage: away.reach.isReady
                              ? "antenna.radiowaves.left.and.right"
                              : "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(away.reach.isReady ? .secondary : Theme.color(.warn))
                    if !away.reach.isReady {
                        Button("Set up away alerts") {
                            Task { await away.enable() }
                        }
                    }
                } header: {
                    Text("Away from home")
                } footer: {
                    Text("Away alerts travel through your own iCloud: a device that's home posts a wake carrying nothing but how serious it is — no name, no time, no footage — and your iPhone writes the words itself. SecuraCV runs no server in that path and can't read any of it.")
                }
            }
            .navigationTitle("Alerts")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .sheet(isPresented: $showingHousehold) { HouseholdSheet() }
            .task {
                if !away.reach.isReady { await away.enable() }
                // Who is actually on the share is only ever learned by
                // asking — acceptance happens on someone else's device.
                await household.refreshMembers()
            }
        }
    }

    /// The pickers speak Dates; the setting stores wall-clock hour+minute, so
    /// a quiet hour is 10pm wherever the user is rather than a frozen instant.
    private var quietStart: Binding<Date> {
        Binding(get: { center.quietHours.startDate() },
                set: { center.quietHours.setStart($0) })
    }

    private var quietEnd: Binding<Date> {
        Binding(get: { center.quietHours.endDate() },
                set: { center.quietHours.setEnd($0) })
    }
}

struct AlertRuleRow: View {
    @Binding var rule: AlertRule
    var awayReach: AwayReach

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.xs) {
            Toggle(isOn: $rule.enabled) {
                Text(rule.title).font(.body)
            }
            Picker("Reach", selection: $rule.reach) {
                Text("On Wi-Fi only").tag(AlertRule.Reach.onWiFiOnly)
                Text("Anywhere").tag(AlertRule.Reach.anywhere)
            }
            .pickerStyle(.segmented)
            .disabled(!rule.enabled)
            // Honesty about reach is the feature: if the away path can't
            // carry anything right now, the rule says so instead of leaving
            // "Anywhere" looking like a working promise.
            if rule.reach == .anywhere, rule.enabled, !awayReach.isReady {
                Label("Can't reach you away yet — \(awayReach.explanation)",
                      systemImage: "exclamationmark.triangle")
                    .font(.caption2)
                    .foregroundStyle(Theme.color(.warn))
            }
        }
        .padding(.vertical, 2)
    }
}

#Preview("Alerts — demo fleet") {
    AlertsView().environmentObject(DemoFleet.previewStore())
}
