// FirmwareUpdate.swift
//
// One Canary's software update, as the CANARY tells it. The phone never
// downloads firmware and never holds an image: it asks the device to check,
// asks it to install, and reads back what the device says. The device
// fetches the release, checks its Ed25519 signature against the release key
// it was built with, writes the inactive slot, restarts, and rolls itself
// back if the new image fails its health check. That engine
// (firmware/common/ota) is the same one Home Assistant's update entity
// drives; this is the phone's door to it.
//
// The contract, served with the same field names by two families:
//   * every display (canary-display glass_web.cpp): GET /api/ota/status,
//     POST /api/ota/check, POST /api/ota/install — open on the home network
//     like the display's other writes; a busy engine answers 409;
//   * a paired WAP (canary_wap.ino): the same three routes behind its bearer
//     token, over its pinned https; a busy engine answers 400 "ota_busy",
//     and its status adds release notes.
// `scripts/tests/test_ios_firmware_update.py` holds the field names and the
// state words below to both firmware sources, so a renamed key fails a test
// on Linux instead of silently reading as "not checked".
//
// Honesty rules:
//   * every sentence about the update's outcome is the DEVICE's — its own
//     `error_text`, its own version after the restart; the phone never
//     claims an install worked until the Canary answers with the new version;
//   * a restart that comes back on the OLD version is said out loud: the
//     update did not stick and the device rolled back;
//   * firmware that predates these routes answers 404, which is named as
//     what it is (update it once from a computer), never as an error.

import Foundation

/// What `GET /api/ota/status` answered. Tolerant: unknown keys are ignored
/// and missing ones keep their defaults, so an older or newer Canary still
/// decodes.
struct FirmwareStatus: Equatable, Sendable, Decodable {
    var installed: String = ""
    /// The newest version the device has seen in a manifest — nil until it
    /// has checked at least once this boot.
    var latest: String?
    /// The engine's state word (`securacv_ota_state_str`): Idle, Checking,
    /// Downloading, Verifying, Flashing, Rebooting, Error.
    var state: String = "Idle"
    var stateText: String = ""
    var progress: Int = 0
    /// The engine's technical error word; "No error" when there is none.
    var error: String = ""
    /// The engine's plain-language sentence for `error` — what the screen
    /// shows. Empty when there is nothing to say.
    var errorText: String = ""
    var updateAvailable: Bool = false
    var autoUpdate: Bool?
    /// The WAP serves the manifest's notes; a display does not.
    var releaseNotes: String?

    enum CodingKeys: String, CodingKey {
        case installed = "installed_version"
        case latest = "latest_version"
        case state
        case stateText = "state_text"
        case progress
        case error
        case errorText = "error_text"
        case updateAvailable = "update_available"
        case autoUpdate = "auto_update"
        case releaseNotes = "release_notes"
    }

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        installed = (try? c.decodeIfPresent(String.self, forKey: .installed)) ?? ""
        latest = (try? c.decodeIfPresent(String.self, forKey: .latest)) ?? nil
        if latest?.isEmpty == true { latest = nil }
        state = (try? c.decodeIfPresent(String.self, forKey: .state)) ?? "Idle"
        stateText = (try? c.decodeIfPresent(String.self, forKey: .stateText)) ?? ""
        progress = (try? c.decodeIfPresent(Int.self, forKey: .progress)) ?? 0
        error = (try? c.decodeIfPresent(String.self, forKey: .error)) ?? ""
        errorText = (try? c.decodeIfPresent(String.self, forKey: .errorText)) ?? ""
        updateAvailable = (try? c.decodeIfPresent(Bool.self, forKey: .updateAvailable)) ?? false
        autoUpdate = (try? c.decodeIfPresent(Bool.self, forKey: .autoUpdate)) ?? nil
        releaseNotes = (try? c.decodeIfPresent(String.self, forKey: .releaseNotes)) ?? nil
        if releaseNotes?.isEmpty == true { releaseNotes = nil }
    }
}

/// The engine's working steps, in the order a run goes through them.
enum FirmwareStep: String, Equatable, Sendable, CaseIterable {
    case checking = "Checking"
    case downloading = "Downloading"
    case verifying = "Verifying"
    case flashing = "Flashing"

    var title: String {
        switch self {
        case .checking: return "Checking for an update"
        case .downloading: return "Downloading"
        case .verifying: return "Checking the signature"
        case .flashing: return "Installing"
        }
    }

    /// Download and flash report a real percentage; the other two do not.
    var hasProgress: Bool { self == .downloading || self == .flashing }
}

/// What the update area shows, derived from one status.
enum FirmwarePhase: Equatable, Sendable {
    /// Idle and the device has not fetched a manifest this boot.
    case notChecked
    /// Idle, checked, nothing newer than what is installed.
    case upToDate
    /// Idle, checked, and this version is ready to install.
    case available(String)
    /// A run is in flight. `progress` is 0…100 where the step reports one.
    case working(FirmwareStep, progress: Int?)
    /// The device is restarting into the new image.
    case restarting
    /// The engine stopped with this sentence (the device's own words).
    case failed(String)
}

/// How the device answered a check or install request.
enum FirmwareKick: Equatable, Sendable {
    case started
    /// The engine is already running a check or install.
    case busy
    /// Firmware that predates update requests from the phone.
    case unsupported
    /// Refused, with a sentence for the screen.
    case refused(String)
}

/// What a restart ended in, once the device answers again.
enum FirmwareOutcome: Equatable, Sendable {
    case updated(to: String)
    /// Back on the version it had before: the new image did not stick.
    case rolledBack(still: String, reason: String)
}

enum FirmwareUpdate {
    /// The engine's "nothing went wrong" word (`securacv_ota_error_str`).
    static let noErrorWord = "No error"

    static func phase(_ s: FirmwareStatus) -> FirmwarePhase {
        if let step = FirmwareStep(rawValue: s.state) {
            let pct = step.hasProgress ? max(0, min(100, s.progress)) : nil
            return .working(step, progress: pct)
        }
        switch s.state {
        case "Rebooting":
            return .restarting
        case "Error":
            return .failed(failureSentence(s))
        default:
            if s.updateAvailable, let v = s.latest { return .available(v) }
            return s.latest == nil ? .notChecked : .upToDate
        }
    }

    /// The device's own sentence for a failed run, or a plain fallback when
    /// a firmware sent none.
    static func failureSentence(_ s: FirmwareStatus) -> String {
        let text = s.errorText.trimmingCharacters(in: .whitespacesAndNewlines)
        if !text.isEmpty { return text }
        return "The update stopped. Your Canary is still on its current software."
    }

    /// A note the device left beside an idle state — for example why the
    /// last run was refused. Nil when it reported no error.
    static func lingeringNote(_ s: FirmwareStatus) -> String? {
        if s.state == "Error" { return nil }  // the failure line already says it
        if s.error.isEmpty || s.error == noErrorWord { return nil }
        let text = s.errorText.trimmingCharacters(in: .whitespacesAndNewlines)
        return text.isEmpty ? nil : text
    }

    /// Read a kick's HTTP answer. Both families agree on 200 for started;
    /// they differ on busy (display 409, WAP 400 with "ota_busy").
    static func kick(status: Int, body: Data) -> FirmwareKick {
        if (200..<300).contains(status) { return .started }
        let reason = errorWord(body)
        switch status {
        case 409:
            return .busy
        case 400 where reason == "ota_busy":
            return .busy
        case 404:
            return .unsupported
        case 401:
            return .refused("This Canary didn't accept this phone's pairing. Pair it again from the Fleet tab.")
        case 403:
            return .refused("This Canary refused the request. Make sure this phone is on the same Wi-Fi as it, then try again.")
        default:
            if reason == "ota_check_failed" || reason == "ota_install_failed" {
                return .refused("The Canary couldn't start the update. Try again in a minute.")
            }
            return .refused("The Canary answered with an error (\(status)). Try again in a minute.")
        }
    }

    /// The `error` (or `err`) word of a JSON error body, if any.
    static func errorWord(_ body: Data) -> String? {
        guard let obj = try? JSONSerialization.jsonObject(with: body) as? [String: Any] else {
            return nil
        }
        return (obj["error"] as? String) ?? (obj["err"] as? String)
    }

    /// How long to wait before reading the status again; nil means the
    /// phone stops polling until the person acts.
    static func pollInterval(_ phase: FirmwarePhase) -> Duration? {
        switch phase {
        case .working: return .seconds(1)
        case .restarting: return .seconds(3)
        default: return nil
        }
    }

    /// The longest the phone waits for a restarted Canary to answer before
    /// it says so, rather than spinning forever.
    static let restartPatience: Duration = .seconds(180)

    /// Compare the version a restart came back on with the one before it.
    static func outcome(before: String, after s: FirmwareStatus) -> FirmwareOutcome {
        if !s.installed.isEmpty, s.installed != before {
            return .updated(to: s.installed)
        }
        let reason = lingeringNote(s)
            ?? "The new version didn't pass its health check, so the Canary went back to the one it had."
        return .rolledBack(still: before, reason: reason)
    }
}
