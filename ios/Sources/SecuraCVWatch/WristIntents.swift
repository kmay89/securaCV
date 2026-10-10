// WristIntents.swift  (watch app target)
//
// "Ask, don't open" — on the wrist. The phone exports its four verbs as App
// Intents (Sources/SecuraCV/Native/FleetIntents.swift); this is the watch's
// one: Check the Fleet, so Siri on the watch, the Shortcuts app and an Apple
// Watch Ultra's Action button can answer "is everything okay?" without the
// phone in hand.
//
// Mirror, don't clone. The answer is composed by the same shared, host-tested
// sentence builder the phone's intent uses (Shared/GlanceAnswer.swift), from
// the snapshot this watch last adopted (WristCache) — so it carries the same
// honesty rules: an old snapshot says its age, a quiet fleet over a dark
// delivery path says both, sample data says so. Nothing here talks to the
// phone or a radio; the acting verbs (test the path, quiet an hour) stay on
// the phone, where their results can be seen.

import AppIntents

struct CheckFleetIntent: AppIntent {
    static let title: LocalizedStringResource = "Check the Fleet"
    static let description = IntentDescription(
        "The fleet's one honest answer, from what this watch last heard from your iPhone.")

    func perform() async throws -> some IntentResult & ProvidesDialog {
        .result(dialog: IntentDialog(stringLiteral: GlanceAnswer.spoken(WristCache.load())))
    }
}

/// Zero-setup surfacing, the phone's precedent: the phrase makes the verb
/// live in Siri and the Action button picker the moment the app is installed.
struct WristShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(intent: CheckFleetIntent(),
                    phrases: [
                        "Check \(.applicationName)",
                        "How is my fleet in \(.applicationName)",
                        "Is everything okay in \(.applicationName)",
                    ],
                    shortTitle: "Check the Fleet",
                    systemImageName: "bird")
    }
}
