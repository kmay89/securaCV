// SenseModality.swift
//
// HOW a Canary senses — camera, radar, WiFi-CSI, or a contact switch — as
// a fact about the DEVICE, derived from the type it publishes about itself.
// Radar presence reads very differently from a camera person-detection even
// when both map to the same coarse event, and no competitor shows sensing
// provenance at all; this is the honest slice of that story the phone can
// tell today.
//
// Why device-level and not per-event: no payload the phone reads carries a
// per-event modality field (the witness record's signed preimage is frozen
// at seven fields), so a per-row chip would be a guess wearing a glyph. A
// device-level derivation is sound because every shipping sensing device is
// single-modality — the same reasoning the Home Assistant integration's
// fallback uses.
//
// The raw values are the witness dictionary's modality ids, and the
// device-type → modality pairs mirror `DEVICE_TYPE_MODALITY` in
// custom_components/securacv/const.py — the wire-contract home of this map.
// That makes this the map's third copy (const.py + the JS timeline card are
// the other two) and it is NOT covered by lint_dictionary_sync.py, so
// EventVocabularyTests pins it against const.py on disk: a pair added or
// changed there fails a test here instead of silently drifting.

import Foundation

/// The kind of sensing a device does, in the dictionary's modality ids.
enum SenseModality: String, Codable, Hashable, Sendable {
    case camera = "camera"
    case radar = "radar"
    case wifiCSI = "wifi-csi"
    case contact = "contact"

    /// From the type string a device publishes about itself (`/api/fleet`
    /// "product", mDNS TXT `dt` — `Witness.publishedType`). Returns nil
    /// for anything unmapped: a device whose senses we can't name gets no
    /// glyph, never a guess (the const.py precedent).
    init?(publishedType: String?) {
        switch publishedType {
        case "canary-vision": self = .camera
        case "canary-sense": self = .radar
        case "canary-wap": self = .wifiCSI
        case "canary-contact": self = .contact
        default: return nil
        }
    }

    var label: String {
        switch self {
        case .camera: return "Camera (events only)"
        case .radar: return "60 GHz radar"
        case .wifiCSI: return "Wi-Fi sensing"
        case .contact: return "Contact switch"
        }
    }

    var sfSymbol: String {
        switch self {
        case .camera: return "camera.metering.matrix"
        case .radar: return "dot.radiowaves.right"
        case .wifiCSI: return "wifi"
        case .contact: return "door.left.hand.closed"
        }
    }
}
