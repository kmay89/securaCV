// FleetFocusFilter.swift
//
// First-class Focus citizenship: this SetFocusFilterIntent appears inside
// iOS's own Focus settings (Settings → Focus → <any Focus> → Add Filter),
// letting the user decide per Focus — Sleep, Work, Personal — whether
// SecuraCV may deliver anything below life-safety. The intent writes one
// bit through FocusGate; AlertCenter honors it at level-decision time.
// When the Focus ends, the system re-runs the filter with default values,
// so the gate opens again by construction — no cleanup code to forget.

import AppIntents

struct FleetFocusFilter: SetFocusFilterIntent {
    static var title: LocalizedStringResource = "SecuraCV Alerts"
    static var description: IntentDescription? =
        "Choose how much of your fleet's voice reaches you during this Focus."

    @Parameter(title: "Only life-safety alerts", default: false)
    var onlyLifeSafety: Bool

    var displayRepresentation: DisplayRepresentation {
        DisplayRepresentation(
            title: "Fleet alerts",
            subtitle: onlyLifeSafety ? "Life-safety only" : "All armed alerts"
        )
    }

    func perform() async throws -> some IntentResult {
        FocusGate.setCriticalOnly(onlyLifeSafety)
        return .result()
    }
}
