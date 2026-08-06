// Coverage.swift — the one question this app owes an answer to.
//
// "If smoke happened right now, would I be told?"
//
// Every surface here answered a piece of that and none answered it: away
// reach knew about iCloud, the HomeKit standing knew about a home hub, the
// alert rules knew what was armed, and nothing joined them. A person asking
// the question had to hold five screens in their head and do the reasoning
// the app should have done. That is exactly the moment a life-safety product
// loses someone's trust — not when it fails, but when it cannot say whether
// it would.
//
// So: one model, one verdict, and lanes that each state what they cover and
// what they don't. The rules are pure and total; the view renders them and
// adds nothing. Where a lane cannot be observed from this device (the ntfy
// relay runs on the hub, the HA blueprint lives in someone's Home Assistant),
// the answer is "we can't see this from here" — never a cheerful guess.

import Foundation

/// One delivery path, and whether it can carry an alert right now.
struct CoverageLane: Identifiable, Equatable {
    enum Standing: Equatable {
        /// Working as far as this device can tell.
        case covered
        /// Definitely not going to reach you.
        case broken(String)
        /// Off because nobody turned it on.
        case off(String)
        /// Real, but not observable from this device — say so rather than
        /// claiming either way.
        case unobservable(String)

        var isCovered: Bool { self == .covered }
    }

    let id: String
    let name: String
    let standing: Standing
    /// What this lane covers when it works — so a person can tell whether
    /// the paths still standing are the ones that matter to them.
    let carries: String
}

/// The joined answer.
struct Coverage: Equatable {
    let lanes: [CoverageLane]

    /// How many paths could actually deliver right now.
    var workingCount: Int { lanes.filter(\.standing.isCovered).count }

    /// The headline. Deliberately counts paths rather than saying "you're
    /// fine": redundancy is the design here (the lanes fail independently),
    /// so the honest summary is how many are standing, not a green tick.
    var headline: String {
        switch workingCount {
        case 0: return "Nothing would reach you"
        case 1: return "One way to reach you"
        default: return "\(workingCount) ways to reach you"
        }
    }

    /// The sentence under the headline — the actual answer to the question.
    var summary: String {
        switch workingCount {
        case 0:
            return "If something happened right now, this app could not tell you. Fix one of the paths below."
        case 1:
            return "One path is standing. It works — but nothing is behind it if it fails."
        default:
            return "These paths fail independently, so more than one is the point."
        }
    }

    /// Build the verdict from what each surface honestly knows.
    ///
    /// Pure and total: no clock, no network, no HomeKit. Everything it needs
    /// is passed in, so every rung is testable and a reviewer can read the
    /// whole policy in one place.
    static func evaluate(
        notificationsAuthorized: Bool,
        anyRuleArmed: Bool,
        awayReachReady: Bool,
        awayReachExplanation: String,
        homeKitEnabled: Bool,
        homeKitHubPresent: Bool,
        residentKnown: Bool
    ) -> Coverage {
        var lanes: [CoverageLane] = []

        // 1. This phone, in the house. The floor everything else stands on:
        // if iOS will not show a notification, no lane below can be seen.
        lanes.append(CoverageLane(
            id: "local",
            name: "This iPhone, at home",
            standing: {
                if !notificationsAuthorized {
                    return .broken("Notifications are off for SecuraCV — turn them on in Settings.")
                }
                if !anyRuleArmed {
                    return .off("No alert is armed. Choose what should reach you in Alerts.")
                }
                return .covered
            }(),
            carries: "Everything, while you are on the home network."
        ))

        // 2. Away, through the household's own iCloud.
        lanes.append(CoverageLane(
            id: "away",
            name: "Away, through your iCloud",
            standing: awayReachReady
                ? .covered
                : .broken(awayReachExplanation),
            carries: "A coarse word — tamper, integrity, offline, pattern — that opens the app."
        ))

        // 3. The resident. Not observable from the phone: whether an Apple TV
        // is standing watch is a fact about that Apple TV. Claiming coverage
        // we cannot see would be the exact failure this screen exists to end.
        lanes.append(CoverageLane(
            id: "resident",
            name: "An Apple TV standing watch",
            standing: residentKnown
                ? .covered
                : .unobservable("Turn on \"Stand watch\" on the Apple TV showing the Witness Wall. Without something home, away alerts have nobody to send them."),
            carries: "A Canary going dark or a chain failing, while you are out."
        ))

        // 4. Apple Home. The app can see the projection's own standing, but
        // whether the household wrote an automation is Apple's to know.
        lanes.append(CoverageLane(
            id: "applehome",
            name: "Apple Home",
            standing: {
                if !homeKitEnabled {
                    return .off("Not publishing to Apple Home. Turn it on in Keys → Apple Home.")
                }
                if !homeKitHubPresent {
                    return .broken("No home hub. Apple needs a HomePod or Apple TV to run automations or reach you away.")
                }
                return .covered
            }(),
            carries: "Whatever automations you told the house to run."
        ))

        // 5. The hub's own relay. It runs on the Pi, not here — and the only
        // honest way to know it works is the drill that actually sends one.
        lanes.append(CoverageLane(
            id: "relay",
            name: "The hub's alert relay",
            standing: .unobservable("Runs on your hub, so this phone cannot check it. Prove it with `alert_relay --send-test` — it sends a real test to your phone."),
            carries: "Smoke, CO, tamper and integrity, with no Apple or Google in the path."
        ))

        return Coverage(lanes: lanes)
    }
}
