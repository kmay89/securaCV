// SettingsRevert.swift
//
// The safety net under the display settings sheet: settings people worked
// hard on must survive a session of fiddling. When the sheet opens it takes
// a snapshot of every knob AS THE DEVICE REPORTS IT; if the session leaves
// things worse, one button replays the snapshot — the same one-key-per-POST
// contract every ordinary write uses, so the device's own clamping and
// validation still have the last word.
//
// Pure arithmetic, host-tested. The diff is computed against the device's
// CURRENT truth (the re-read after every write), never against what the app
// remembers sending — reverting a value the device already holds would be
// noise, and reverting a knob the firmware refused would repeat the refusal.

import Foundation

enum SettingsRevert {
    /// The snapshot knobs whose values drifted — each one, written back
    /// through the ordinary write path (wireValue translation included),
    /// walks the device to where the session started. Empty when nothing
    /// drifted; the sheet uses that emptiness to decide whether "Undo
    /// changes" has anything honest to offer.
    ///
    /// Keys present only on one side are skipped: a knob the device stopped
    /// reporting mid-session cannot be written back, and a knob it GAINED
    /// mid-session has no snapshot value to restore.
    static func writes(toRestore snapshot: [GlassKnob], from now: [GlassKnob]) -> [GlassKnob] {
        let current = Dictionary(now.map { ($0.key, $0.value) }, uniquingKeysWith: { a, _ in a })
        return snapshot.filter { was in
            guard let live = current[was.key] else { return false }
            return live != was.value
        }
    }
}
