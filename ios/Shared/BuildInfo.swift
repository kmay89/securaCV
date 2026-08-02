// BuildInfo.swift  (SHARED — every target's About surface tells the same story)
//
// Build identity, read from the target's own Info.plist where
// scripts/stamp_build.sh baked it. RFC rule (apple_watch_and_notifications.md
// §7): show the build rev and firmware train on EVERY surface's About screen,
// so "which version are you on?" is never a support question — on the phone,
// the pad, or the wrist. Pure Foundation.

import Foundation

enum BuildInfo {
    static var version: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "—"
    }
    /// Baked at build time by scripts/stamp_build.sh into Info.plist.
    static var buildRev: String {
        Bundle.main.object(forInfoDictionaryKey: "SECURACV_BUILD_REV") as? String ?? "dev"
    }
    static var firmwareTrain: String {
        Bundle.main.object(forInfoDictionaryKey: "SECURACV_FW_TRAIN") as? String ?? "0.x"
    }
}
