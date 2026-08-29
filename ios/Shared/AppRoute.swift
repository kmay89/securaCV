// AppRoute.swift  (SHARED — pure Foundation)
//
// Where a tap OUTSIDE the app should land INSIDE it. Every competitor's
// notification "just opens the app" — and until this type existed, so did
// ours (the tap-through case in AlertCenter was a comment). A route is a
// value, not a navigation call, so the same vocabulary serves all three
// doors: the notification tap (AlertCenter → FleetStore.pendingRoute), the
// widget tap (`.widgetURL`), and any future Shortcuts/link entry — and the
// whole thing is host-testable without a UI.
//
// The URL dialect is deliberately tiny: securacv://today and
// securacv://alerts[?witness=<id>]. Unknown hosts and schemes parse to nil —
// a stale or hostile link renders as nothing, never as a guess. The witness
// id is an ANCHOR HINT, not a capability: routing can only scroll a list the
// user already owns; there is nothing a deep link can open, export, or
// silence.

import Foundation

/// A destination inside the app, carried as a value.
enum AppRoute: Equatable, Sendable {
    /// The Today tab — the one honest answer.
    case today
    /// The Alerts tab, optionally anchored at one witness's newest record.
    /// nil = the tab itself is the answer (storm summaries, away wakes).
    case alerts(witnessID: String?)

    /// The scheme widgets and links speak. Registered in Info.plist
    /// (CFBundleURLTypes) — change both together.
    static let scheme = "securacv"

    /// Parse a deep-link URL. nil for anything this build doesn't speak —
    /// the unknown-vocabulary rule EventVocabulary uses, applied to links.
    init?(url: URL) {
        guard url.scheme?.lowercased() == Self.scheme else { return nil }
        switch url.host?.lowercased() {
        case "today":
            self = .today
        case "alerts":
            let witness = URLComponents(url: url, resolvingAgainstBaseURL: false)?
                .queryItems?
                .first(where: { $0.name == "witness" })?
                .value
            self = .alerts(witnessID: (witness?.isEmpty == false) ? witness : nil)
        default:
            return nil
        }
    }

    /// The URL form, for `.widgetURL` and anything else that speaks links.
    /// Round-trips through `init(url:)` — AppRouteTests pins it.
    var url: URL {
        var parts = URLComponents()
        parts.scheme = Self.scheme
        switch self {
        case .today:
            parts.host = "today"
        case .alerts(let witnessID):
            parts.host = "alerts"
            if let witnessID, !witnessID.isEmpty {
                parts.queryItems = [URLQueryItem(name: "witness", value: witnessID)]
            }
        }
        // A components set built only from the closed vocabulary above always
        // yields a URL; the fallback exists so a future case can't crash.
        return parts.url ?? URL(string: "\(Self.scheme)://today")!
    }
}
