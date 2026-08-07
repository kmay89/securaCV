// HomePresence.swift — "am I home?", and the far more important "do I know?"
//
// This exists because the first answer was wrong in the worst possible
// direction. `awayFromHome = !seesFleet` reads as common sense — a phone that
// can hear a Canary is on the home network — but run it forward: the away
// guard it feeds (`AlertCenter.unknowableFromAway`) suppresses darkness, and
// darkness is the only thing that makes `seesFleet` false. So the guard could
// fire in exactly one situation: **no Canary is answering at all.**
//
// That is not "the owner drove to work." That is also the single Canary
// household whose only Canary just died, and the whole-house power cut that
// took every device at once — the event this product exists to report, and
// the one the user named first when they asked for this. An inference meant
// to remove noise had quietly become a mute switch on the loudest alarm.
//
// The repair is to stop treating absence of evidence as evidence. Being away
// now requires POSITIVE evidence, and there are only two honest kinds of it:
//
//   * Not on Wi-Fi at all. Cellular means not on the home network, full stop.
//     This is the drive-to-work case, which is most of the noise.
//   * On a Wi-Fi network where we have never once seen the fleet. Someone
//     else's house. (No SSID read, no location permission, no geofence —
//     just "did the fleet ever answer during this association?", which
//     NWPathMonitor hands us for free when the path changes.)
//
// Everything else is `.unknown`, and unknown REPORTS. The two errors are not
// symmetric and it is not close: a false "away" is silence during a fire, a
// false "home" is a notification you didn't need. We take the notification.

import Foundation

/// Where this phone is, to the extent it can honestly tell.
enum HomePresence: Equatable {
    /// A Canary is answering right now. Nothing else is as good as this.
    case home
    /// Positive evidence of absence — see the two kinds above.
    case away
    /// On a network the fleet lives on, but nothing is answering. Could be a
    /// blackout, could be the router. Never a reason to go quiet.
    case unknown

    /// Pure and total. `sawFleetOnThisNetwork` means "at some point during the
    /// current network association, at least one Canary answered" — it is what
    /// separates a home blackout from standing in someone else's kitchen.
    static func evaluate(seesFleet: Bool,
                         onWiFi: Bool,
                         sawFleetOnThisNetwork: Bool) -> HomePresence {
        if seesFleet { return .home }
        // Cellular, or no network at all: certainly not on the home LAN.
        guard onWiFi else { return .away }
        // On Wi-Fi. If the fleet has answered here before, this is their
        // network and their silence is news. If it never has, we are a guest.
        return sawFleetOnThisNetwork ? .unknown : .away
    }

    /// May this device suppress a report it cannot substantiate?
    ///
    /// Only from `.away`. `.unknown` deliberately does not qualify: the whole
    /// point of the three-state split is that "I can't tell" and "I am
    /// elsewhere" stopped being the same answer.
    var maySuppressDarkness: Bool { self == .away }
}
