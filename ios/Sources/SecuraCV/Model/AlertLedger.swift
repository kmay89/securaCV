// AlertLedger.swift
//
// The memory behind the Alerts tab: every condition that crossed a rule, what
// we managed to do about it, and what the user did back. Persisted in the
// shared app group (the one the app and its widgets already hold — no new
// entitlement, so nothing here can break signing), capped, and collapsed.
//
// Collapse is the whole trick. A Canary that flaps between lost and online for
// an hour must be ONE line that says "6 times", not six lines — a list you
// can't read at 3am is the same as no list. Records collapse on the alert's
// fingerprint, which is exactly the key FleetStore already uses to decide
// whether something is news, so the tab and the notifications agree by
// construction rather than by coincidence.
//
// Freshness is the other half (the doctrine:
// docs/design/alerts_event_history.md — alerts are perishable, history is
// durable, and neither may impersonate the other):
//   * A record is OPEN while its condition is live and RESOLVED once the
//     fleet's truth says it cleared — FleetStore closes the loop at the same
//     moment it decides the next occurrence would be news again.
//   * Settled history the user has seen ages out after `retention`. What has
//     never been seen is never silently deleted by time — "we discarded the
//     thing you missed" is the one failure this ledger exists to prevent —
//     so unseen rows are bounded only by the cap.
//   * Nothing here re-notifies. The ledger is memory; AlertCenter is voice.

import Foundation

@MainActor
final class AlertLedger: ObservableObject {
    /// Newest first. The view binds straight to this.
    @Published private(set) var records: [AlertRecord] = []

    /// Enough to cover "what happened while I was away for a week" without
    /// letting a pathological fleet grow the file without bound.
    static let cap = 200
    /// How long settled, seen history stays before the sweep lets it go.
    /// A month covers "what happened while we traveled"; past that a row is
    /// an archive nobody asked for. (Ring keeps 180 days but sells the
    /// storage; Nest free keeps 3 hours and sells the rest. Local-first
    /// means the number can simply be the honest one.)
    static let retention: TimeInterval = 30 * 86_400
    static let storeKey = "alert_ledger_v1"

    private let defaults: UserDefaults

    init(defaults: UserDefaults? = nil) {
        self.defaults = defaults
            ?? UserDefaults(suiteName: PhoneGlanceCache.appGroupID)
            ?? .standard
        load()
    }

    // MARK: - writing

    /// Note that a condition crossed a rule. Returns the live record — new, or
    /// the existing one with its repeat count and last-seen bucket advanced.
    @discardableResult
    func note(id: String, witnessID: String, name: String, severity: Severity,
              headline: String, now: Date = Date()) -> AlertRecord {
        if let i = records.firstIndex(where: { $0.id == id }) {
            records[i].count += 1
            records[i].lastBucket = AlertRecord.bucket(for: now)
            // A repeat is news again: it re-enters the list at the top, and a
            // condition the user acknowledged that came BACK is not "handled",
            // not "over", and not "seen" — the whole lifecycle reopens.
            records[i].handlingRaw = AlertHandling.new.rawValue
            records[i].resolvedBucket = nil
            records[i].seenBucket = nil
            records[i].mutedUntil = nil
            records[i].escalatedBucket = nil
            let moved = records.remove(at: i)
            records.insert(moved, at: 0)
            save()
            return moved
        }
        let record = AlertRecord(id: id, witnessID: witnessID, name: name,
                                 severity: severity, headline: headline, at: now)
        records.insert(record, at: 0)
        if records.count > Self.cap { records.removeLast(records.count - Self.cap) }
        save()
        return record
    }

    /// Record how it actually reached them. Delivery only ever moves UP
    /// (notDelivered → onLAN → away): if a wake already reached the pocket,
    /// a later local post must not downgrade the history's account of it.
    func markDelivery(_ delivery: AlertDelivery, for id: String, reason: String? = nil) {
        guard let i = records.firstIndex(where: { $0.id == id }) else { return }
        if delivery.rawValue >= records[i].deliveryRaw {
            records[i].deliveryRaw = delivery.rawValue
        }
        records[i].undeliveredReason = delivery == .notDelivered ? reason : nil
        save()
    }

    /// Ack / mute, however it arrived — the notification action, the phone
    /// screen, or the watch. Applies to every live record for that witness,
    /// because "I've seen it" is about the device, not one row. `until`
    /// carries the chosen mute's end, so the row can show when the alerts
    /// come back instead of leaving a silence with no visible edge.
    func mark(_ handling: AlertHandling, forWitness witnessID: String, until: Date? = nil) {
        var touched = false
        for i in records.indices where records[i].witnessID == witnessID
            && records[i].handling == .new {
            records[i].handlingRaw = handling.rawValue
            if handling == .muted, let until {
                records[i].mutedUntil = AlertRecord.bucket(for: until)
            }
            touched = true
        }
        if touched { save() }
    }

    /// This condition was re-alerted for going unanswered. Stamped once —
    /// `EscalationPolicy` reads it back to guarantee at most one escalation
    /// per occurrence, and the stamp survives a relaunch because the whole
    /// ledger does.
    func markEscalated(id: String, now: Date = Date()) {
        guard let i = records.firstIndex(where: { $0.id == id }),
              records[i].escalatedBucket == nil else { return }
        records[i].escalatedBucket = AlertRecord.bucket(for: now)
        save()
    }

    /// One record by its collapse key — the escalation pass needs to read a
    /// condition's own history (when it first landed, whether it was already
    /// escalated) rather than keeping a second, drift-prone copy in memory.
    func record(id: String) -> AlertRecord? {
        records.first { $0.id == id }
    }

    /// Every live record for a witness — the tuning counters ask "what kinds
    /// of alert did this ack/dismiss actually answer?"
    func liveRecords(forWitness witnessID: String) -> [AlertRecord] {
        records.filter { $0.witnessID == witnessID && $0.handling == .new }
    }

    /// The condition behind every open record for this witness has cleared —
    /// FleetStore calls this at the same moment the witness leaves its
    /// news-dedupe ledgers, so "would notify again" and "shown as over" are
    /// one decision, never two drifting ones. Handling is untouched: a
    /// condition that cleared on its own was still never acknowledged, and
    /// the row keeps saying so.
    func resolve(witnessID: String, now: Date = Date()) {
        var touched = false
        for i in records.indices where records[i].witnessID == witnessID
            && records[i].resolvedBucket == nil {
            records[i].resolvedBucket = AlertRecord.bucket(for: now)
            touched = true
        }
        if touched { save() }
    }

    /// The user laid eyes on the list (they left the Alerts tab). Every row
    /// present is now "seen" — the badge and the unseen dots clear together.
    func markSeen(now: Date = Date()) {
        var touched = false
        for i in records.indices where records[i].seenBucket == nil {
            records[i].seenBucket = AlertRecord.bucket(for: now)
            touched = true
        }
        if touched { save() }
    }

    /// One row, gone — the user's swipe. Removal is theirs to do; the sweep
    /// below only ever takes what they have already seen and settled.
    func remove(id: String) {
        let before = records.count
        records.removeAll { $0.id == id }
        if records.count != before { save() }
    }

    /// The freshness rule, applied: settled history the user has seen ages
    /// out after `retention`. Rows still open, and rows never seen, are
    /// exempt — time may not delete a live condition or a missed one.
    func retentionSweep(now: Date = Date()) {
        let cutoff = now.addingTimeInterval(-Self.retention)
        let before = records.count
        records.removeAll { record in
            guard let resolved = record.resolvedBucket,
                  record.seenBucket != nil else { return false }
            return max(record.lastBucket, resolved) < cutoff
        }
        if records.count != before { save() }
    }

    /// The user's "Clear history": settled rows only. Rows that still need a
    /// human survive — an ongoing alarm can be acked or muted, never made to
    /// vanish from the one list whose job is showing it. (A cleared live row
    /// would also stay gone: the news-dedupe still holds its fingerprint, so
    /// nothing would re-create it until the condition changed.)
    func clearSettled() {
        let before = records.count
        records.removeAll { !$0.needsYou }
        if records.count != before { save() }
    }

    // MARK: - reading

    /// The one number the tab's header needs: things still wanting a human.
    var unhandledCount: Int { records.filter { $0.handling == .new }.count }

    /// What the app badge counts: rows the user has never laid eyes on.
    var unseenCount: Int { records.filter(\.isUnseen).count }

    /// True when nothing has ever needed them — the calm empty state, which
    /// is the state we WANT people to live in and should feel earned.
    var isQuiet: Bool { records.isEmpty }

    /// Rebuild FleetStore's news-dedupe state from the persisted history, so
    /// an alarm that outlives an app relaunch does not re-notify as if it
    /// were fresh news. (Before this fold, every relaunch re-posted every
    /// ongoing condition — the exact spam the dedupe ledgers exist to stop,
    /// reintroduced through the one door they didn't cover.) The record id
    /// is "witnessID|fingerprint" by construction, so the fingerprint is
    /// recoverable; newest record per witness wins, matching the live rule.
    static func foldOpenAlerts(records: [AlertRecord])
        -> (posted: [String: String], acked: [String: String]) {
        var posted: [String: String] = [:]
        var acked: [String: String] = [:]
        // Newest-first, so `posted[w] == nil` keeps the newest per witness.
        for record in records where record.isOpen {
            let fingerprint = String(record.id.dropFirst(record.witnessID.count + 1))
            guard !fingerprint.isEmpty else { continue }
            if posted[record.witnessID] == nil {
                posted[record.witnessID] = fingerprint
            }
            if record.handling == .acknowledged, acked[record.witnessID] == nil {
                acked[record.witnessID] = fingerprint
            }
        }
        return (posted, acked)
    }

    // MARK: - persistence

    private func load() {
        guard let data = defaults.data(forKey: Self.storeKey),
              let decoded = try? JSONDecoder().decode([AlertRecord].self, from: data) else { return }
        records = decoded
    }

    private func save() {
        guard let data = try? JSONEncoder().encode(records) else { return }
        defaults.set(data, forKey: Self.storeKey)
    }
}
