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

import Foundation

enum DeviceNaming {
    /// The product names, keyed on the wire-canonical device type. These are
    /// the CD_DEVICE_TYPE values in firmware/configs/canary-display/*/config.h
    /// and their siblings across the fleet.
    private static let titles: [String: String] = [
        "canary-dash": "Canary Dash",
        "canary-dash7": "Canary Dash 7",
        "canary-nightstand": "Canary Nightstand",
        "canary-nightstand7": "Canary Nightstand 7",
        "canary-nightstand-touch": "Canary Nightstand Touch",
        "canary-nightlight": "Canary Nightlight",
        "canary-watch": "Canary Watch Station",
        "canary-display": "Canary display",
        "canary-wap": "Canary WAP",
        "canary-vision": "Canary Vision",
        "canary-sense": "Canary Sense",
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
}
