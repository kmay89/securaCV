// EcosystemMap.swift  (SHARED — pure Foundation)
//
// The family, named once: every other SecuraCV surface, its one-sentence
// job, and an HONEST availability line. This exists because the audit that
// compared us to Ring/Wyze/Eufy/Reolink (docs/research/
// competitor_app_landscape.md) found we beat their substance and matched
// their confusion: the surfaces are engineered to agree with each other,
// but no surface ever told a user the others exist — the Flasher mints
// birth certificates "to match the one the iPhone will show" without ever
// mentioning an iPhone app, and this app returned the favor.
//
// Rules for entries, learned from the competitors' failures:
//   * One sentence of job, present tense — what it DOES, not marketing.
//   * Availability is honest-status copy (non-negotiable #4): a surface
//     that is built and tested but not yet installable by a normal person
//     SAYS SO, because "get this app for every platform" over a gated
//     pipeline is exactly the overclaim this project refuses.
//   * No account, no store push, no telemetry behind any link — routing,
//     never a funnel. The links are the same stable securacv.com paths the
//     desktop Flasher's Atlas uses.
//
// The long-term home for this data is a machine-readable family map every
// surface consumes (the roadmap's `ecosystem.json`); until that exists this
// list is the app's copy, and EcosystemMapTests pins its honesty rules.

import Foundation

/// One member of the family: another surface a fleet owner can stand at.
struct EcosystemSurface: Identifiable, Hashable, Sendable {
    let id: String
    let name: String
    /// SF Symbol for the row glyph.
    let sfSymbol: String
    /// What it does, one present-tense sentence.
    let job: String
    /// Honest availability — where you get it, or why you can't yet.
    let availability: String
    let url: URL
}

enum EcosystemMap {
    /// Stable brand-domain entry points (the same paths the desktop
    /// Flasher's Atlas links, so the two apps can never disagree on where
    /// the family lives).
    static let site = "https://securacv.com"
    static let repo = "https://github.com/kmay89/securaCV"

    static let labURL = URL(string: site + "/lab")!
    static let downloadURL = URL(string: site + "/download")!

    /// The family, in the order a fleet owner meets it: try it, flash it,
    /// live with it on the other screens.
    static let surfaces: [EcosystemSurface] = [
        EcosystemSurface(
            id: "lab",
            name: "The Lab",
            sfSymbol: "testtube.2",
            job: "The real firmware, compiled to WebAssembly, running in your browser — meet every Canary and watch one boot before you own any hardware.",
            availability: "Free, in any browser",
            url: labURL),
        EcosystemSurface(
            id: "flasher",
            name: "SecuraCV Flasher",
            sfSymbol: "cable.connector",
            job: "The desktop app that flashes a blank board into a Canary over USB and tends the fleet after — one-click verified updates, health at a glance.",
            availability: "Free download for Mac & Linux",
            url: downloadURL),
        EcosystemSurface(
            id: "lab-app",
            name: "SecuraCV Lab (desktop)",
            sfSymbol: "macwindow",
            job: "The whole Lab as a small native app — the same benches, offline, nothing phoning home.",
            availability: "Free download for Mac & Linux",
            url: downloadURL),
        EcosystemSurface(
            id: "witness-wall",
            name: "The Witness Wall",
            sfSymbol: "tv",
            job: "Your fleet's record on the TV — a calm status wall, never a wall of video. An Apple TV that stands watch can also post away wakes while you're out.",
            availability: "Built and tested; App Store availability pending. Try it in the Lab today.",
            url: URL(string: repo + "/blob/main/tvos/RUN_ON_APPLE_TV.md")!),
        EcosystemSurface(
            id: "hub",
            name: "The Hub (Home Assistant)",
            sfSymbol: "house",
            job: "Home Assistant on a Raspberry Pi running the witness kernel — the timeline card, automations, and voice, all local.",
            availability: "Free — the Flasher writes the card for you",
            url: labURL),
    ]
}
