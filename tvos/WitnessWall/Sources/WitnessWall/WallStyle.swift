//  WallStyle.swift — use-case profiles and skins.
//
//  One wall, three rooms it lives in. The same FleetSnapshot drives every
//  profile — a profile changes LAYOUT and EMPHASIS, never the data, so there
//  is no way for "business mode" to claim something "home mode" wouldn't.
//  Mirrors the web emulator's editions (securacv.com/witness-wall) and its
//  skins, so the product reads the same on every surface.

import SwiftUI

/// Which room the TV is in — the use case this Wall serves.
enum WallProfile: String, CaseIterable, Identifiable {
    /// The living-room Witness Wall: calm tiles, furniture for hours.
    case home
    /// The Witness Board behind a counter: denser, attention-first, built to
    /// be read from across a bar in two seconds.
    case business
    /// The apartment peephole: the door leads, everything else follows —
    /// made for "someone's knocking, who is it?" without getting up.
    case apartment

    var id: String { rawValue }

    var label: String {
        switch self {
        case .home: return "Home"
        case .business: return "Business"
        case .apartment: return "Apartment"
        }
    }

    /// The grid a profile wants: the Board packs more Canaries per glance;
    /// the peephole wants fewer, larger cards.
    var tileMinimum: CGFloat {
        switch self {
        case .home: return 380
        case .business: return 300
        case .apartment: return 560
        }
    }

    /// Device ordering. Every profile leads with what needs a person
    /// (troubled, then offline); the apartment additionally pulls the
    /// door-ish Canaries to the front, because that is what the room asks.
    func sorted(_ devices: [FleetSnapshot.Device]) -> [FleetSnapshot.Device] {
        devices.sorted { a, b in
            if a.chainIsTroubled != b.chainIsTroubled { return a.chainIsTroubled }
            if a.online != b.online { return !a.online }
            if self == .apartment {
                let ad = Self.isDoorish(a.name), bd = Self.isDoorish(b.name)
                if ad != bd { return ad }
            }
            return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
        }
    }

    static func isDoorish(_ name: String) -> Bool {
        let n = name.lowercased()
        return ["door", "entry", "entrance", "peep", "hall", "porch"].contains { n.contains($0) }
    }
}

/// How the wall dresses — the same presets as the web emulator, minus the
/// ones that don't survive a 10-foot view. Midnight is the default because
/// this screen is on for hours in a dim room.
enum WallSkin: String, CaseIterable, Identifiable {
    case midnight, daylight, terminal, paper

    var id: String { rawValue }

    var label: String {
        switch self {
        case .midnight: return "Midnight"
        case .daylight: return "Daylight"
        case .terminal: return "Terminal"
        case .paper: return "Paper"
        }
    }

    /// Background gradient, top → bottom.
    var backgroundTop: Color {
        switch self {
        case .midnight: return Color(white: 0.04)
        case .daylight: return Color(red: 0.93, green: 0.94, blue: 0.96)
        case .terminal: return Color(red: 0.01, green: 0.05, blue: 0.02)
        case .paper: return Color(red: 0.96, green: 0.94, blue: 0.89)
        }
    }
    var backgroundBottom: Color {
        switch self {
        case .midnight: return Color(white: 0.10)
        case .daylight: return Color(red: 0.88, green: 0.90, blue: 0.94)
        case .terminal: return Color(red: 0.02, green: 0.09, blue: 0.04)
        case .paper: return Color(red: 0.92, green: 0.89, blue: 0.82)
        }
    }
    /// Card fill.
    var tile: Color {
        switch self {
        case .midnight: return Color(white: 0.13)
        case .daylight: return Color.white
        case .terminal: return Color(red: 0.03, green: 0.12, blue: 0.06)
        case .paper: return Color(red: 0.99, green: 0.97, blue: 0.93)
        }
    }
    /// Primary text.
    var ink: Color {
        switch self {
        case .midnight, .terminal: return Color.white
        case .daylight: return Color(white: 0.12)
        case .paper: return Color(red: 0.18, green: 0.15, blue: 0.10)
        }
    }
    /// The healthy accent — terminal keeps its phosphor.
    var ok: Color {
        switch self {
        case .terminal: return Color(red: 0.30, green: 0.95, blue: 0.45)
        default: return .green
        }
    }
    /// True when this skin is light and system materials should flip.
    var isLight: Bool { self == .daylight || self == .paper }
}
