// Theme.swift  (SHARED — compiled into the app, both widget targets, and the
// watch app; everything here is SwiftUI that exists on iOS and watchOS alike)
//
// One place for color, type, and spacing so light/dark and Dynamic Type stay
// coherent on every screen size — phone, pad, and wrist. Colors are SEMANTIC
// (calm/info/warn/alert/tamper) — views ask for a role, never a raw hex —
// which is how the palette stays honest and how "pure red only for a real
// alarm" (a firmware lint, scripts/lint_no_impersonation.sh) is respected on
// every Apple surface too.

// SecuraCV-Parity: every Apple surface that shows a device compiles this.
// (the one place a severity becomes a color)

import SwiftUI

enum Theme {
    enum Role { case calm, info, warn, alert, tamper, neutral }

    static func color(_ role: Role) -> Color {
        switch role {
        case .calm:    return .green
        case .info:    return .accentColor
        case .warn:    return .orange
        case .alert:   return .red
        case .tamper:  return Color(red: 0.72, green: 0.11, blue: 0.20)  // deep crimson, distinct from plain alert red
        case .neutral: return .secondary
        }
    }

    // Spacing scale (points) — a 4pt rhythm keeps layouts tidy across sizes.
    // `xxs` and `inline` name the micro-gaps the views already used as bare
    // 2s and 6s; nothing here invents a new distance.
    static let xxs: CGFloat = 2    // hairline air: line gaps inside a row
    static let xs: CGFloat = 4
    /// Gap between an inline glyph and the text it belongs to. Off the 4pt
    /// rhythm on purpose — optical, not structural.
    static let inline: CGFloat = 6
    static let s: CGFloat = 8
    static let m: CGFloat = 12
    static let l: CGFloat = 20
    // `xl` (32) is deleted: zero call sites anywhere. If a hero ever needs it
    // back, it returns WITH its first use — a token nobody spends is a shadow
    // scale. Bare `.padding()` (system default) stays deliberately
    // untokenized: it is the platform's own token, and the 9 sites using it
    // are correct as-is.

    static let corner: CGFloat = 16

    // Opacity ramp — the only three alphas a role color may wear from now on.
    // Color still never carries meaning alone (invariant C5); these set
    // WEIGHT, never meaning. Every decorative alpha snaps to the nearest
    // stop; a value that can't is a design smell, not a fourth stop. Full
    // strength is deliberately not on the ramp — a color at 1 is a
    // statement, and statements don't get softened.
    static let soft: Double = 0.60   // present but yielded: the dashed add-cell, the far ring
    static let dim: Double = 0.35    // recedes without vanishing: quiet rings, the ghost caret, selection grounds
    static let faint: Double = 0.12  // an atmosphere, not an element: severity washes, track rings, chip grounds
}

/// A calm card container reused across every surface.
///
/// Two shapes, one recipe:
///   .regular — a full content card (timeline rows, empty states).
///   .chip    — a one-line whisper: a strip of material carrying a sentence
///              and at most one action (the away summary, the demo banner,
///              the bird's bubble). Panels hold content; chips hold a
///              remark. Tighter vertical padding, and a chip sizes to its
///              content — same material, same continuous corner, so every
///              floating strip in the app is visibly the same object and a
///              change here reaches all of them.
struct Card<Content: View>: View {
    enum Variant { case regular, chip }
    var variant: Variant = .regular
    @ViewBuilder var content: Content

    var body: some View {
        switch variant {
        case .regular:
            content
                .padding(Theme.m)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(.ultraThinMaterial,
                            in: RoundedRectangle(cornerRadius: Theme.corner, style: .continuous))
        case .chip:
            content
                .padding(.horizontal, Theme.m)
                .padding(.vertical, Theme.s)
                .background(.ultraThinMaterial,
                            in: RoundedRectangle(cornerRadius: Theme.corner, style: .continuous))
        }
    }
}

/// A severity pip used in lists and the Dynamic Island.
struct SeverityPip: View {
    let severity: Severity
    var body: some View {
        Image(systemName: severity.sfSymbol)
            .foregroundStyle(Theme.color(severity.role))
            .imageScale(.medium)
            .accessibilityLabel(severity.label)
    }
}
