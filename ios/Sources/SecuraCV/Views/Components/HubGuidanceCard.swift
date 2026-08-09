// HubGuidanceCard.swift
//
// "This one has no hub" — said once, where the device is, with the next step
// attached.
//
// WHY THIS IS NOT A WARNING. A Canary with no MQTT broker is not broken and is
// not misconfigured. The whole fleet works without one: displays render from
// what they hear on the LAN, and this phone talks to every device directly
// over HTTP. A hub buys you the things that need somewhere to live while
// nobody is looking — history that outlives a reboot, Home Assistant, alerts
// that leave the house. So this card explains a choice; it never raises an
// alarm, and it never colors itself like one.
//
// It also distinguishes the two states an owner can be in, because they have
// different fixes and telling someone to check a hub they never set up is how
// a help message wastes an evening:
//
//   .absent  nobody has pointed this device at a broker  → here is how
//   .down    it has one and cannot reach it right now    → check the hub
//
// Anything else — including a device that said nothing, which is every device
// on older firmware — renders NOTHING. Silence is not evidence of a healthy
// hub, so this must not draw a reassuring row for a device it knows nothing
// about (that would be the same overstatement the alert history was fixed to
// stop making).

import SwiftUI

struct HubGuidanceCard: View {
    let hub: HubState

    /// Where the hub instructions live: securacv.com/linux, the page that asks
    /// "do you need a computer?" and answers it — which is precisely the
    /// question a hub-less Canary raises. It is an EXISTING page (the site's
    /// glossary entry for The Hub points at the same one), not a docs path
    /// invented to fill this link; a settings screen that sends someone to a
    /// 404 is worse than one that says nothing.
    ///
    /// A link rather than a flow the app drives, and that is deliberate:
    /// setting up a broker means running Home Assistant on a Raspberry Pi or
    /// pointing at one you already have. An iPhone cannot do either, and a
    /// button implying otherwise would lead to a dead end.
    private static let setupURL = URL(string: "https://securacv.com/linux")!

    var body: some View {
        switch hub {
        case .absent:
            card(icon: "externaldrive.badge.questionmark",
                 title: "No hub yet",
                 body: "This Canary is standing on its own. That works — it "
                     + "answers this phone directly and the displays hear it "
                     + "on your network. A hub adds the parts that need "
                     + "somewhere to live: history that survives a reboot, "
                     + "Home Assistant, and alerts that reach you away from "
                     + "home.",
                 link: "Do you need one?")
        case .down:
            card(icon: "externaldrive.badge.xmark",
                 title: "Can't reach its hub",
                 body: "This Canary has a hub configured and can't reach it "
                     + "right now. The device itself is fine — you're reading "
                     + "this because it answered. Check that the hub is "
                     + "powered on and on this network.",
                 link: "About the hub")
        case .ok, .unknown:
            EmptyView()
        }
    }

    private func card(icon: String, title: String, body: String, link: String) -> some View {
        VStack(alignment: .leading, spacing: Theme.s) {
            Label(title, systemImage: icon)
                .font(.subheadline.weight(.medium))
            Text(body)
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Link(link, destination: Self.setupURL)
                .font(.caption.weight(.medium))
        }
        .padding(.vertical, 4)
    }
}

#if DEBUG
#Preview("Hub guidance") {
    List {
        Section("absent") { HubGuidanceCard(hub: .absent) }
        Section("down") { HubGuidanceCard(hub: .down) }
        // Both of these render nothing, on purpose.
        Section("ok") { HubGuidanceCard(hub: .ok) }
        Section("unknown") { HubGuidanceCard(hub: .unknown) }
    }
}
#endif
