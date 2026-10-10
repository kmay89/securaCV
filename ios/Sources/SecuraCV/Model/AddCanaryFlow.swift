// AddCanaryFlow.swift
//
// The small decisions behind "add a Canary", pulled out of the views so each
// one is a line in a test (AddCanaryFlowTests): which first-run card Today
// shows, what to say when a radio is off or not allowed, and what the QR
// button does with the camera permission. Pure — no CoreBluetooth, no
// Network, no AVFoundation, no SwiftUI: the transports translate their own
// states into the plain enums here, and the views only draw the answer.
//
// The rule these share is the one the user asked for: never a spinner
// without a reason and a way out. "Listening…" is only ever shown over a
// radio that can actually hear; anything else gets its cause in one plain
// sentence and, where the fix lives in Settings, the button that goes there.

import Foundation

/// The Bluetooth radio as the setup screens need to know it — CBManagerState
/// and CBManagerAuthorization folded into the four answers that change what
/// the screen says. The transport maps into this (BLEConsole.radio).
enum RadioStanding: Equatable, Sendable {
    /// Not known yet (no manager created, or it has not reported). Treated
    /// as "can hear": the honest default before the first report.
    case unknown
    /// Powered on and allowed.
    case ready
    /// Allowed, but switched off (Control Center / Settings).
    case off
    /// The person said no to Bluetooth for this app, or a profile forbids it.
    case denied
    /// This device has no Bluetooth LE at all.
    case unsupported
}

/// One sentence of cause, and whether its fix is in this app's Settings page.
struct RadioAdvice: Equatable, Sendable {
    var text: String
    var opensSettings: Bool
}

enum SetupRadioAdvice {
    static let bluetoothOff = RadioAdvice(
        text: "Bluetooth is off — turn it on in Control Center to hear a new Canary.",
        opensSettings: false)
    static let bluetoothDenied = RadioAdvice(
        text: "SecuraCV isn't allowed to use Bluetooth, so it can't hear a new Canary. Turn it on in Settings.",
        opensSettings: true)
    static let bluetoothUnsupported = RadioAdvice(
        text: "This device has no Bluetooth LE, so it can't hear a Canary's setup door — use its setup network instead.",
        opensSettings: false)
    static let localNetworkBlocked = RadioAdvice(
        text: "SecuraCV can't see devices on your Wi-Fi — Local Network is off for it. Turn it on in Settings.",
        opensSettings: true)

    /// What a screen that listens over Bluetooth should say instead of its
    /// spinner — nil while the radio can hear.
    static func bluetooth(_ radio: RadioStanding) -> RadioAdvice? {
        switch radio {
        case .unknown, .ready: return nil
        case .off: return bluetoothOff
        case .denied: return bluetoothDenied
        case .unsupported: return bluetoothUnsupported
        }
    }

    /// What a screen that watches the Wi-Fi (mDNS) should say instead of
    /// its spinner — nil while the browse can see.
    static func localNetwork(blocked: Bool) -> RadioAdvice? {
        blocked ? localNetworkBlocked : nil
    }

    /// For a screen that needs either radio (Today's first-run card, the
    /// nearby step): Bluetooth first, because that is the one that finds a
    /// brand-new Canary; the Wi-Fi only matters once it has joined.
    static func either(bluetooth radio: RadioStanding, localNetworkBlocked: Bool) -> RadioAdvice? {
        bluetooth(radio) ?? localNetwork(blocked: localNetworkBlocked)
    }
}

/// Today's way in, for a phone with no real fleet yet.
enum FirstRunCard: Equatable, Sendable {
    /// Never asked: one card that asks and starts listening in the same tap
    /// (the tap IS the consent — iOS's prompts follow it, in context).
    case ask
    /// Asked and allowed, nothing heard yet: "Listening — power it on
    /// within a few meters" (or the radio advice, when it can't hear).
    case listening
    /// Said "Not now": the quiet invitation that waits.
    case invite

    /// Which card, if any. None once there is a real fleet (Today is about
    /// the fleet then), and none while a new Canary is being heard — the
    /// nearby card has that moment.
    static func decide(consent: Bool?, hasRealFleet: Bool, hearsNearby: Bool) -> FirstRunCard? {
        guard !hasRealFleet, !hearsNearby else { return nil }
        switch consent {
        case nil: return .ask
        case true?: return .listening
        case false?: return .invite
        }
    }
}

/// What the "Scan the QR on its glass" button does, by camera permission.
/// The scanner used to be offered only when VisionKit already reported
/// itself available — which it does not before the camera is allowed, and
/// nothing ever asked, so a brand-new install never saw the button at all.
enum CameraGate: Equatable, Sendable {
    enum Standing: Equatable, Sendable { case notDetermined, authorized, denied }
    /// Ask iOS first; scan if it says yes.
    case askThenScan
    /// Already allowed: scan.
    case scan
    /// Said no (or restricted): say so, offer Settings, keep the typed fields.
    case explainDenied

    static func action(for standing: Standing) -> CameraGate {
        switch standing {
        case .notDetermined: return .askThenScan
        case .authorized: return .scan
        case .denied: return .explainDenied
        }
    }

    static let deniedNote = "Camera access is off for SecuraCV, so it can't scan the glass. Turn it on in Settings, or type the network and key below."
}
