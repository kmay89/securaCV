// DeviceNaming.swift — hand-written.
//
// What to CALL a device, given the type string it published.
//
// This exists because the two obvious answers are both wrong some of the
// time. The figure's title is wrong when one board serves several products
// (the 7" glass is both the Dash 7 and the Nightstand 7, and its one figure
// is titled for one of them). The raw wire string is wrong to show a person:
// "canary-nightstand7" is an identifier, not a name, and putting it on a card
// under a photograph of the device reads like a bug.
//
// So: a small table of the names we actually ship, keyed on the vocabulary
// the firmware publishes. It is deliberately a TABLE and not a transform.
// A "prettifier" that title-cased the string and split the trailing digit
// would produce a plausible name for a type nobody has ever shipped, which is
// exactly the kind of confident guess the rest of this pipeline refuses to
// make. An unknown type returns nil and the caller says something coarser and
// true instead.

// SecuraCV-Parity: every Apple surface that shows a device compiles this.
// (what a device is CALLED)

import Foundation

enum DeviceNaming {
    /// The product names, keyed on the wire-canonical device type.
    ///
    /// EVERY KEY HERE IS A VALUE SOME BUILD ACTUALLY PUBLISHES — the
    /// CD_DEVICE_TYPE values in firmware/configs/canary-display/*/config.h and
    /// their siblings across the fleet. That constraint is the point: an
    /// earlier cut of this table carried "canary-dash7" and
    /// "canary-nightstand-touch", which read like precision and were fiction.
    /// No device publishes either — the Dash 7 answers "canary-dash" and the
    /// Nightstand Touch answers "canary-nightstand", because those flavors
    /// share an OTA product identity with their siblings. A table entry for a
    /// string nothing sends is dead code that looks like a feature.
    private static let titles: [String: String] = [
        "canary-dash": "Canary Dash",
        "canary-nightstand": "Canary Nightstand",
        "canary-nightstand7": "Canary Nightstand 7",
        "canary-nightlight": "Canary Nightlight",
        "canary-watch": "Canary Watch Station",
        "canary-display": "Canary display",
        "canary-wap": "Canary WAP",
        "canary-vision": "Canary Vision",
        "canary-sense": "Canary Sense",
        // Published by firmware/configs/canary-sentinel/door/config.h
        // (SENT_DEVICE_TYPE) — a real wire string, unlike the two fictions
        // this table once carried. The Sentinel has no figure yet (no
        // enclosure CAD), so it draws the honest no-picture card, but it
        // deserves its name rather than an identifier.
        "canary-sentinel": "Canary Sentinel",
    ]

    /// The product name for a published device type, or nil when this build
    /// has never heard of it.
    ///
    /// Nil is a real answer and callers must handle it rather than falling
    /// back to the raw string: a fleet running newer firmware than this app
    /// will publish types that are not in the table, and the honest thing to
    /// show then is the coarse family ("Fleet display"), not an identifier.
    static func productTitle(forPublishedType raw: String) -> String? {
        titles[FleetFigure.canonicalDeviceType(raw)]
    }

    /// What to call this device, from everything it told us — the ONE resolver
    /// every surface uses, so the picture card and the birth certificate can
    /// never print two different names for one Canary.
    ///
    /// Asks the board before the type, because the board is sometimes MORE
    /// product-precise than the type is. A Nightstand Touch publishes the same
    /// `canary-nightstand` as the plain Nightstand, but compiles against its
    /// own pins header, so its figure names it exactly — and reading the name
    /// off the type alone would have called it a Nightstand on its own
    /// certificate while its picture said Nightstand Touch.
    ///
    /// The reverse case is why this is not simply "always use the figure": a
    /// shared board's figure title names ONE of the products it serves, so a
    /// Dash 7 (board shared with the Nightstand 7) must not take it. There the
    /// type is all we have, and it resolves to "Canary Dash" — less specific
    /// than the truth, and not wrong. Saying "Canary Dash 7" would require the
    /// device to say so, and it doesn't.
    ///
    /// Nil when nothing pins a product; callers show the coarse family.
    static func productName(published: String?, hardware: String?) -> String? {
        if let board = hardware, !board.isEmpty,
           FleetFigure.namesItsProduct(hardware: board),
           let figure = FleetFigure.forHardware(board) {
            return figure.title
        }
        guard let raw = published, !raw.isEmpty else { return nil }
        return productTitle(forPublishedType: raw)
    }
}
