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
    static let xs: CGFloat = 4
    static let s: CGFloat = 8
    static let m: CGFloat = 12
    static let l: CGFloat = 20
    static let xl: CGFloat = 32

    static let corner: CGFloat = 16
}

/// A calm card container reused across every surface.
struct Card<Content: View>: View {
    @ViewBuilder var content: Content
    var body: some View {
        content
            .padding(Theme.m)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: Theme.corner, style: .continuous))
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
