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

    /// One honest sentence per room, for the settings panel — the same three
    /// editions the Mac app's Witness Wall view offers, described the same way.
    var blurb: String {
        switch self {
        case .home:
            return "Calm tiles for the living room. Furniture, on for hours."
        case .business:
            return "The Board: denser, attention-first, readable across a bar in two seconds."
        case .apartment:
            return "The peephole: the door leads, so \u{201C}who\u{2019}s knocking?\u{201D} is answerable from the couch."
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

/// How the wall dresses — the SAME six presets, in the same order, as the Mac
/// app's Witness Wall view and the web emulator (witness.html's skin bar), so
/// "set it to Sunset" means one thing on every surface. Midnight is the
/// default because this screen is on for hours in a dim room.
enum WallSkin: String, CaseIterable, Identifiable {
    case midnight, daylight, noir, sunset, terminal, paper

    var id: String { rawValue }

    var label: String {
        switch self {
        case .midnight: return "Midnight"
        case .daylight: return "Daylight"
        case .noir: return "Noir"
        case .sunset: return "Sunset"
        case .terminal: return "Terminal"
        case .paper: return "Paper"
        }
    }

    /// Background gradient, top → bottom. Noir and Sunset trace the Mac
    /// view's hex values (ground → surface) rather than inventing near-misses.
    var backgroundTop: Color {
        switch self {
        case .midnight: return Color(white: 0.04)
        case .daylight: return Color(red: 0.93, green: 0.94, blue: 0.96)
        case .noir: return Color(red: 0.043, green: 0.043, blue: 0.047)
        case .sunset: return Color(red: 0.098, green: 0.063, blue: 0.129)
        case .terminal: return Color(red: 0.01, green: 0.05, blue: 0.02)
        case .paper: return Color(red: 0.96, green: 0.94, blue: 0.89)
        }
    }
    var backgroundBottom: Color {
        switch self {
        case .midnight: return Color(white: 0.10)
        case .daylight: return Color(red: 0.88, green: 0.90, blue: 0.94)
        case .noir: return Color(red: 0.086, green: 0.086, blue: 0.090)
        case .sunset: return Color(red: 0.141, green: 0.086, blue: 0.192)
        case .terminal: return Color(red: 0.02, green: 0.09, blue: 0.04)
        case .paper: return Color(red: 0.92, green: 0.89, blue: 0.82)
        }
    }
    /// Card fill.
    var tile: Color {
        switch self {
        case .midnight: return Color(white: 0.13)
        case .daylight: return Color.white
        case .noir: return Color(red: 0.086, green: 0.086, blue: 0.094)
        case .sunset: return Color(red: 0.141, green: 0.094, blue: 0.200)
        case .terminal: return Color(red: 0.03, green: 0.12, blue: 0.06)
        case .paper: return Color(red: 0.99, green: 0.97, blue: 0.93)
        }
    }
    /// Primary text.
    var ink: Color {
        switch self {
        case .midnight, .terminal: return Color.white
        case .noir: return Color(red: 0.925, green: 0.925, blue: 0.925)
        case .sunset: return Color(red: 0.953, green: 0.906, blue: 0.925)
        case .daylight: return Color(white: 0.12)
        case .paper: return Color(red: 0.18, green: 0.15, blue: 0.10)
        }
    }
    /// The healthy accent — terminal keeps its phosphor, noir stays gray-green
    /// (its whole point is no loud color), sunset is warm.
    var ok: Color {
        switch self {
        case .terminal: return Color(red: 0.30, green: 0.95, blue: 0.45)
        case .noir: return Color(red: 0.686, green: 0.780, blue: 0.733)
        case .sunset: return Color(red: 0.498, green: 0.820, blue: 0.659)
        default: return .green
        }
    }
    /// True when this skin is light and system materials should flip.
    var isLight: Bool { self == .daylight || self == .paper }
}
