// CanaryVoiceStage.swift
//
// The clocked half of the bird helper: what the pure queue decides, this
// object performs — present, dwell, hide, breathe, next. It is the phone's
// counterpart of the website Herald's DOM layer, and the one place anything
// in the app can hand the bird a line:
//
//     store.voice.say("Saved locally.", tone: .good)
//
// Like the site's `canary:say` event, the channel exists before its callers
// do — the site shipped with no external dispatcher and only the Herald's
// own traffic; here the bird's own traffic is the hello, the orientation
// tips, and the trust milestones (the site's other self-traffic, the
// offline/online pair, stays behind — see CanaryVoice.swift's ledger).
//
// Two gates keep the character honest, both set by its owner (FleetStore):
//   * stagePermitted — during a real unacknowledged alarm the face is
//     .hidden and the whole channel holds its tongue; the instruments own
//     the stage. Messages wait rather than vanish: the owner calls
//     stageSeized() when an alarm begins, which shelves whatever was
//     mid-sentence (chatter excepted — it was never important enough to
//     come back for), and stageChanged() when it ends, which lets the line
//     resume.
//   * chatterPermitted — the bird volunteers small talk only in the calm.
//     A worried bird is busy, an asleep bird is asleep; a missed pleasantry
//     does no harm (the site drops preempted chatter for the same reason).
//
// Nothing here touches the network, and nothing persists beyond the
// keeper's four flags — pinned on the website by tests, kept here by the
// same absence of anything to leak.

import Foundation
#if canImport(UIKit)
import UIKit
#endif

@MainActor
final class CanaryVoiceStage: ObservableObject {
    /// The message currently on stage, if any — the bubble renders exactly
    /// this and nothing else.
    @Published private(set) var showing: CanaryVoiceMessage?

    /// False while the character's face is .hidden (a real alarm) — set by
    /// the owner; defaults keep the stage usable in previews and tests.
    var stagePermitted: () -> Bool = { true }
    /// True only when the character is calm — volunteered small talk waits
    /// for a quiet moment or doesn't happen at all.
    var chatterPermitted: () -> Bool = { true }

    private var queue = CanaryVoiceQueue()
    private let keeper: CanaryVoiceKeeper
    private var hideTask: Task<Void, Never>?
    /// In-session guards so a re-arriving shell (a second window, a
    /// re-appearing root) can't schedule the same once-ever line twice
    /// while its delay is still running. The durable once-ever lives in
    /// the keeper, marked only when a line is actually taken.
    private var helloPending = false
    private var orientPending: Set<AppSection> = []
    /// Where the visitor is right now — kept fresh by arrive/orient so a
    /// delayed line can check it still applies before speaking. On the site
    /// this needs no code: a tip's timer dies with the page it was for; the
    /// app's shell outlives every section switch.
    private var currentSection: AppSection = .today

    init(keeper: CanaryVoiceKeeper = CanaryVoiceKeeper()) {
        self.keeper = keeper
    }

    // MARK: - the one public channel

    /// Say something through the bird. Returns whether the line was taken —
    /// false when there was nothing worth saying, the key was already
    /// queued, or quiet mode swallowed chatter.
    @discardableResult
    func say(_ text: String,
             tone: CanaryVoiceTone = .chat,
             ttl: TimeInterval? = nil,
             action: CanaryVoiceAction? = nil,
             key: String? = nil,
             chatter: Bool = false) -> Bool {
        guard let m = CanaryVoiceEngine.compose(text, tone: tone, ttl: ttl,
                                                action: action, key: key,
                                                chatter: chatter) else { return false }
        return deliver(m)
    }

    /// The ✕. Dismissing volunteered small talk counts toward quiet;
    /// dismissing anything else is just a dismissal.
    func dismiss() {
        if let m = queue.showing, m.chatter { keeper.noteChatterDismissed() }
        hide()
    }

    /// The action button was tapped — the message did its work.
    func actionTaken() {
        hide()
    }

    /// The character's face changed (an alarm began or ended). If the stage
    /// just freed up, whoever was waiting speaks now.
    func stageChanged() {
        presentIfIdle()
    }

    /// An alarm just began: the instruments own the stage. Whatever was
    /// mid-sentence is shelved to the front of the line so it can finish
    /// after the alarm — with its full dwell, not the remainder of a timer
    /// that kept burning invisibly. Chatter is dropped instead: it was
    /// never important enough to come back for.
    func stageSeized() {
        hideTask?.cancel()
        guard let m = queue.showing else { return }
        if m.chatter {
            queue.hide()
        } else {
            queue.restage(m)
        }
        showing = nil
    }

    // MARK: - what the bird says on its own

    /// First contact with the shell: introduce once ever, otherwise offer
    /// this section's orientation line. Same branch order as the site — a
    /// first launch gets the hello, and the section's tip only on a later
    /// visit. One gate of the origin's is consciously not carried: the site
    /// greets only where there is a header bird to tap, because its hello
    /// invites the tap; here the character is on every screen, the greeting
    /// invites nothing, and so it simply fires once. And unlike the site,
    /// the flag burns on DELIVERY, not on scheduling — the calm gate can
    /// refuse chatter, and a hello burned because the bird happened to be
    /// worried on first launch would never be heard at all.
    func arrive(at section: AppSection) {
        currentSection = section
        if !keeper.hasGreeted {
            guard !helloPending else { return }
            helloPending = true
            after(CanaryVoicePace.hello) { [weak self] in
                guard let self else { return }
                self.helloPending = false
                // The pointer suppression reads where the visitor IS, not
                // where they were when the delay started.
                guard !self.keeper.hasGreeted,
                      let m = Self.greeting(arrivingAt: self.currentSection) else { return }
                if self.deliver(m) { self.keeper.markGreeted() }
            }
        } else {
            orient(section)
        }
    }

    /// One orientation line per section, ever — and only for the sections
    /// that don't explain themselves. Marked on delivery, like the hello,
    /// so a tip refused by a worried moment gets another chance on a
    /// calmer visit.
    func orient(_ section: AppSection) {
        currentSection = section
        guard Self.tip(for: section) != nil,
              !keeper.hasOriented(section),
              !orientPending.contains(section) else { return }
        orientPending.insert(section)
        after(CanaryVoicePace.tip) { [weak self] in
            guard let self else { return }
            self.orientPending.remove(section)
            // A tip is for the screen it explains. The site gets this for
            // free — leaving a page kills its timer — so a visitor who
            // moved on within the delay is skipped here, unmarked, and the
            // tip may try again on a later visit.
            guard self.currentSection == section,
                  !self.keeper.hasOriented(section),
                  let m = Self.tip(for: section) else { return }
            if self.deliver(m) { self.keeper.markOriented(section) }
        }
    }

    /// The once-ever introduction. The site's hello points at the Lab; here
    /// the bird points at the fleet — unless that's already where you are
    /// (the site suppresses its pointer on /lab the same way). The line
    /// promises only what this channel is: small things. It does not promise
    /// alarms — those belong to the notification path, which may not even be
    /// authorized yet on a first launch.
    static func greeting(arrivingAt section: AppSection) -> CanaryVoiceMessage? {
        CanaryVoiceEngine.compose(
            "Hi — I’m your Canary. This corner is for small things — a tip, a milestone, never an alarm.",
            tone: .chat,
            action: section == .fleet ? nil
                : CanaryVoiceAction(label: "Meet the fleet", section: .fleet),
            key: "hello",
            chatter: true)
    }

    /// The orientation table — the site's TIPS, rewritten for what these
    /// screens actually do (every line names behavior the app really has;
    /// the honesty rule binds copy too). Today explains itself, so it gets
    /// no line — deliberately tiny, like the origin.
    static func tip(for section: AppSection) -> CanaryVoiceMessage? {
        let line: String?
        switch section {
        case .today:
            line = nil
        case .fleet:
            line = "Every Canary here tells its own story — tap one to see what it is and how it’s doing."
        case .alerts:
            line = "Quiet is the normal state here. An ordinary week says nothing, so you can believe it when something speaks."
        case .keys:
            line = "A Canary’s signing key is pinned the first time you meet it — from then on, nothing else speaks for it."
        }
        guard let line else { return nil }
        return CanaryVoiceEngine.compose(line, tone: .info,
                                         key: "tip-\(section.rawValue)",
                                         chatter: true)
    }

    // MARK: - the machinery

    @discardableResult
    private func deliver(_ m: CanaryVoiceMessage) -> Bool {
        if m.chatter && !chatterPermitted() { return false }
        switch queue.offer(m, quiet: keeper.isQuiet) {
        case .refused:
            return false
        case .interrupted:
            // A warning cut the small talk that was up. A beat between the
            // two, so they read as two things said — not one line changing
            // under you.
            hideTask?.cancel()
            showing = nil
            after(CanaryVoicePace.gap) { [weak self] in self?.presentIfIdle() }
            return true
        case .queued:
            presentIfIdle()
            return true
        }
    }

    private func presentIfIdle() {
        guard queue.showing == nil, stagePermitted() else { return }
        guard let m = queue.advance() else { return }
        showing = m
        announce(m)
        hideTask?.cancel()
        if m.ttl > 0 {
            hideTask = Task { [weak self] in
                try? await Task.sleep(for: .seconds(m.ttl))
                guard !Task.isCancelled else { return }
                self?.hide()
            }
        }
    }

    private func hide() {
        hideTask?.cancel()
        queue.hide()
        showing = nil
        if queue.hasWaiting {
            after(CanaryVoicePace.gap) { [weak self] in self?.presentIfIdle() }
        }
    }

    private func after(_ delay: TimeInterval, _ work: @escaping @MainActor () -> Void) {
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(delay))
            work()
        }
    }

    /// VoiceOver hears what the bubble shows. A warning must interrupt a
    /// screen reader; chat must not — the site swaps role=status for
    /// role=alert per message, and the announcement priority is the same
    /// distinction spoken.
    private func announce(_ m: CanaryVoiceMessage) {
        #if canImport(UIKit)
        let text = NSMutableAttributedString(string: m.text)
        let priority: UIAccessibilityPriority = m.tone == .warn ? .high : .low
        text.addAttribute(.accessibilitySpeechAnnouncementPriority,
                          value: priority.rawValue,
                          range: NSRange(location: 0, length: text.length))
        UIAccessibility.post(notification: .announcement, argument: text)
        #endif
    }
}
