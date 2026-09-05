// CanaryVoiceKeeper.swift
//
// What the bird remembers about talking to you — the durable half of the
// helper, mirroring the website's localStorage flags key-for-key in this
// repo's UserDefaults convention: the once-ever introduction, which sections
// have had their orientation line, and the brush-off count that turns into
// quiet. Pure over injected defaults, host-tested, same shape as
// CanaryMoodKeeper next door.
//
// Two rules ride along from the site:
//   * Quiet mutes only CHATTER — the small talk the bird volunteered. A
//     message the app actually asked to send always gets through.
//   * Once ever means once: the flags are set when a line is actually
//     TAKEN, not when it is scheduled. The site burns its hello at schedule
//     time, which was a fine trade there (the only way to miss it was
//     closing the page); here the calm gate can refuse chatter, and a hello
//     burned because the bird happened to be worried on first launch would
//     never be heard at all. The stage marks these flags on delivery.
//
// One conscious fix over the origin: the site stores oriented routes as a
// concatenated string and asks `includes(route)`, which is a substring
// check that only works because no route is a prefix of another. Here the
// ledger is a string array and membership is exact.

import Foundation

struct CanaryVoiceKeeper {
    static let quietKey = "canary_voice_quiet_v1"   // "stop volunteering things"
    static let helloKey = "canary_voice_hello_v1"   // the once-ever introduction
    static let tipsKey = "canary_voice_tips_v1"     // sections already oriented
    static let shushKey = "canary_voice_shush_v1"   // chatter dismissals so far

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    /// True once the bird has been brushed off enough times to stop
    /// volunteering. Never expires; nothing clears it — a preference, not
    /// a punishment cooldown.
    var isQuiet: Bool { defaults.bool(forKey: Self.quietKey) }

    var hasGreeted: Bool { defaults.bool(forKey: Self.helloKey) }
    func markGreeted() { defaults.set(true, forKey: Self.helloKey) }

    func hasOriented(_ section: AppSection) -> Bool {
        (defaults.stringArray(forKey: Self.tipsKey) ?? []).contains(section.rawValue)
    }

    func markOriented(_ section: AppSection) {
        var seen = defaults.stringArray(forKey: Self.tipsKey) ?? []
        guard !seen.contains(section.rawValue) else { return }
        seen.append(section.rawValue)
        defaults.set(seen, forKey: Self.tipsKey)
    }

    /// Only the ✕ on volunteered small talk counts. Dismissing a real
    /// notice, tapping an action, and a message timing out never do — the
    /// site counted exactly the same way.
    func noteChatterDismissed() {
        let n = defaults.integer(forKey: Self.shushKey) + 1
        defaults.set(n, forKey: Self.shushKey)
        if n >= CanaryVoiceEngine.dismissalsToQuiet {
            defaults.set(true, forKey: Self.quietKey)
        }
    }
}
