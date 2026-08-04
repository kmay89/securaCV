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

import Foundation

@MainActor
final class AlertLedger: ObservableObject {
    /// Newest first. The view binds straight to this.
    @Published private(set) var records: [AlertRecord] = []

    /// Enough to cover "what happened while I was away for a week" without
    /// letting a pathological fleet grow the file without bound.
    static let cap = 200
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
            // condition the user acknowledged that came BACK is not "handled".
            records[i].handlingRaw = AlertHandling.new.rawValue
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
    /// because "I've seen it" is about the device, not one row.
    func mark(_ handling: AlertHandling, forWitness witnessID: String) {
        var touched = false
        for i in records.indices where records[i].witnessID == witnessID
            && records[i].handling == .new {
            records[i].handlingRaw = handling.rawValue
            touched = true
        }
        if touched { save() }
    }

    func clear() {
        records = []
        save()
    }

    // MARK: - reading

    /// The one number the tab's header needs: things still wanting a human.
    var unhandledCount: Int { records.filter { $0.handling == .new }.count }

    /// True when nothing has ever needed them — the calm empty state, which
    /// is the state we WANT people to live in and should feel earned.
    var isQuiet: Bool { records.isEmpty }

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
