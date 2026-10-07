// SetupGuide.swift  (SHARED — pure Foundation)
//
// The two walkthroughs this app explains and drives — a hub on a Raspberry
// Pi, and a Canary joining the fleet — as DATA: every step's title, what it
// says, and what the phone itself does at that step. The Set up screen
// renders this; the tests read it. Copy that lives in a view can't be
// pinned; copy that lives here can.
//
// Honesty rules, the same as everywhere else in the app:
//   * A step names what the PHONE does only where the phone does it. The
//     card is flashed by the desktop Flasher (or Raspberry Pi Imager); the
//     phone finishes the hub. A display's key is on its glass; the phone
//     joins the setup network. Nothing here promises a mechanism that does
//     not exist on the device in front of you.
//   * Every Canary family gets its own path, because they differ: a WAP
//     speaks Bluetooth, a display shows a QR, a camera or radar Canary's
//     key comes from the Flasher that hatched it.

import Foundation

/// What a step asks the phone to do, if anything. The view maps each case
/// to the control it shows; a step with none is explanation.
enum SetupAction: String, Equatable, Sendable {
    /// Find Home Assistant on the LAN and watch it through first boot.
    case watchHub
    /// Collect the owner account and finish Home Assistant's wizard.
    case ownerAccount
    /// Run the provisioning plan through the Supervisor proxy.
    case provisionHub
    /// Scan a setup-network QR (a display's glass) or type its key.
    case readSetupKey
    /// Join the Canary's setup network and post the home Wi-Fi.
    case joinSetupNetwork
    /// Send the home Wi-Fi over the bonded Bluetooth provisioning service.
    case bluetoothProvision
    /// Watch for the device to appear on the home network.
    case watchForCanary
}

struct SetupStep: Equatable, Identifiable, Sendable {
    var id: String
    var title: String
    var body: String
    var action: SetupAction?
}

/// The Canary families the walkthrough tells apart. Coarser than the
/// product line (every display flavor shares a path) and finer than
/// DeviceType (the flagship `firmware/canary` build has its own page).
enum CanaryFamily: String, CaseIterable, Identifiable, Sendable {
    case display, vision, sense, wap
    var id: String { rawValue }

    var title: String {
        switch self {
        case .display: return "A display — Watch, Dash, Nightstand, Nightlight"
        case .vision: return "Canary Vision (camera witness)"
        case .sense: return "Canary Sense (radar witness)"
        case .wap: return "Canary WAP (witness beacon)"
        }
    }

    /// The coarse DeviceType this family publishes, for the figure.
    var deviceType: DeviceType {
        switch self {
        case .display: return .display
        case .vision: return .vision
        case .sense: return .sense
        case .wap: return .wap
        }
    }

    /// One sentence a newcomer can pick a family by.
    var tagline: String {
        switch self {
        case .display: return "A screen for the fleet — it shows, it never senses."
        case .vision: return "Detects people on the module; never stores or streams video."
        case .sense: return "Senses presence and breathing through the air — no camera, no microphone."
        case .wap: return "GPS, a signed event log and its own hotspot; pairs with this phone directly."
        }
    }

    /// How this family's Wi-Fi gets set — the path the walkthrough drives.
    var path: CanarySetupPath {
        switch self {
        case .display: return .setupNetwork(keySource: .glassQR)
        case .vision, .sense: return .setupNetwork(keySource: .flasher)
        case .wap: return .bluetooth
        }
    }

    /// How this family reaches the hub once it is on Wi-Fi, in a sentence.
    var hubNote: String {
        switch self {
        case .display:
            return "It finds the broker on its own: any Canary on the network that already talks to the hub tells it where, and Home Assistant's broker answers on the LAN."
        case .vision, .sense:
            return "The broker address and the Canary login go in when you flash it — the Flasher's MQTT fields. Type the login this phone minted for the hub, or read it from the hub's Mosquitto configuration."
        case .wap:
            return "Its own setup page has an optional Home Assistant step for the broker, or scan a hub QR from a display. It pairs with this phone over Wi-Fi with a receipt — the Fleet tab offers that once it appears."
        }
    }
}

/// Where a family's setup key comes from.
enum SetupKeySource: Equatable, Sendable {
    /// The glass draws a `WIFI:` QR for its own setup network.
    case glassQR
    /// The Flasher prints the key after the flash (the device has no screen).
    case flasher
}

enum CanarySetupPath: Equatable, Sendable {
    /// Join `SecuraCV-XXXX`, post the home Wi-Fi to its portal.
    case setupNetwork(keySource: SetupKeySource)
    /// The bonded Bluetooth provisioning service the WAP serves from first boot.
    case bluetooth
}

enum SetupGuide {
    /// The hub walkthrough: a Raspberry Pi, a card, and this phone.
    static let hub: [SetupStep] = [
        SetupStep(
            id: "need",
            title: "What you need",
            body: "A Raspberry Pi 4 (4 GB or more) or Pi 5, a 32 GB or larger microSD card (or an SSD on a Pi 5), its power supply, and your Wi-Fi name and password. The hub runs Home Assistant OS with the SecuraCV witness kernel beside it — headless: it never needs a screen, and this phone is the screen it gets.",
            action: nil),
        SetupStep(
            id: "flash",
            title: "Write the card",
            body: "On a Mac or Linux computer, open the free SecuraCV Flasher and choose Build a Hub: pick your board, type your Wi-Fi, insert the card, type ERASE. It downloads Home Assistant OS, writes it, reads every byte back, and puts your Wi-Fi into the image so the Pi joins your network on its own. No Flasher? Raspberry Pi Imager with Home Assistant OS works too — this phone finishes either.",
            action: nil),
        SetupStep(
            id: "boot",
            title: "Boot it and let this phone watch",
            body: "Put the card in the Pi and power it on. First boot takes 10–20 minutes while Home Assistant installs itself — the blinking light is it working. This phone finds the hub on your Wi-Fi the moment it announces itself, and tells the difference between \"on the network, still installing\" and \"ready\".",
            action: .watchHub),
        SetupStep(
            id: "account",
            title: "Your Home Assistant account",
            body: "Type your name, a username and a password here, once. When the hub is ready, this phone creates that account on it over Home Assistant's own setup API, finishes the wizard's remaining pages, and checks the login works — so the first time you open the hub it is a sign-in, not a wizard. The password goes only to your own hub on your own network and is not kept on this phone.",
            action: .ownerAccount),
        SetupStep(
            id: "finish",
            title: "Install the brain",
            body: "With your account, this phone asks Home Assistant for exactly what its own Settings page would: the Mosquitto broker, a login for your Canaries (minted here, kept in this phone's Keychain), the broker connection, Frigate, and the SecuraCV witness kernel — each step narrated, nothing done twice. What stays yours is said out loud: Frigate's camera config, and the integration's files, which install from the hub's terminal in one command.",
            action: .provisionHub),
        SetupStep(
            id: "next",
            title: "Then the Canaries",
            body: "Your hub is the meeting point. Add a Canary from the other walkthrough: this phone gives it your Wi-Fi, and the broker login it minted goes into any Canary that asks for one.",
            action: nil),
    ]

    /// The Canary walkthrough for one family.
    static func canary(_ family: CanaryFamily) -> [SetupStep] {
        var steps: [SetupStep] = [
            SetupStep(
                id: "what",
                title: family.title,
                body: family.tagline + " " + firstBootLine(family),
                action: nil),
        ]
        switch family.path {
        case .setupNetwork(let keySource):
            steps.append(SetupStep(
                id: "key",
                title: keySource == .glassQR ? "Read the key off its glass" : "The key the Flasher printed",
                body: keySource == .glassQR
                    ? "Power it on. Its screen shows a QR code for its own setup network, named SecuraCV-XXXX. Scan it with this phone, or type the four characters and the key printed under it."
                    : "A camera or radar Canary has no screen, so its setup-network key is printed by the Flasher that hatched it (and if you typed Wi-Fi into the Flasher, the Canary is already on your network — skip to the last step). Type the network name and key here.",
                action: .readSetupKey))
            steps.append(SetupStep(
                id: "join",
                title: "Hand it your Wi-Fi",
                body: "This phone joins the Canary's setup network itself — iOS asks once — posts your home Wi-Fi name and password to the Canary's own setup page, and watches it join. The device stores the credentials only once it is actually connected, and tears its setup network down after. Your password goes only to the Canary, over its setup network.",
                action: .joinSetupNetwork))
        case .bluetooth:
            steps.append(SetupStep(
                id: "bluetooth",
                title: "Send your Wi-Fi over Bluetooth",
                body: "Power it on. A WAP broadcasts over Bluetooth from the moment it boots, so this phone hears it within seconds — no setup network to join. Choose it below, type your Wi-Fi, and the credentials go across the bonded Bluetooth provisioning link (iOS shows the pairing sheet once). The Canary answers with its own verdict: connected, or why not.",
                action: .bluetoothProvision))
        }
        steps.append(SetupStep(
            id: "appear",
            title: "Watch it appear",
            body: "Once it is on your Wi-Fi, it announces itself and shows up on the Fleet tab by itself — nothing to pair for a display, a camera or a radar Canary. " + family.hubNote,
            action: .watchForCanary))
        return steps
    }

    private static func firstBootLine(_ family: CanaryFamily) -> String {
        switch family.path {
        case .setupNetwork: return "Out of the box it has no Wi-Fi, so it raises a small setup network of its own and waits for a phone."
        case .bluetooth: return "Out of the box it has no Wi-Fi, and it is listening over Bluetooth for exactly this phone."
        }
    }

    /// Every step across both walkthroughs — what the tests sweep for the
    /// voice rules (US spellings, fleet never the other bird word).
    static var allSteps: [SetupStep] {
        hub + CanaryFamily.allCases.flatMap { canary($0) }
    }
}
