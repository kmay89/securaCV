//! The BUSY Bar HTTP protocol, as a pure mapping.
//!
//! Everything here is decision, not delivery: a [`Frame`] becomes the JSON
//! body of one `POST /api/display/draw`, and no function in this file opens a
//! socket. Same split as `crate::relay::busybar`, and for the same reason —
//! the privacy posture of what we draw is then unit-testable without a device
//! on the bench.
//!
//! # What is confirmed, and what is not
//!
//! The shapes below were read from the firmware's own OpenAPI specification
//! (`busy-app/busybar-firmware`, `applications/services/web_server/openapi/`,
//! `info.version` 27.7.0 on the `dev` branch) and cross-checked against the
//! two official clients, `busylib-py` and `busylib-ts` (the latter is npm's
//! `@busy-app/busy-lib`). That spec is the in-development contract, not
//! necessarily what a given unit ships, so:
//!
//! **Nothing in this module has been exercised against real hardware.** The
//! design doc marks the whole surface *pending bench validation*
//! (`docs/design/busybar_surface.md` §9) and lists the specific things a
//! bench session has to settle. Two of them are already visible from here:
//!
//! - **`DELETE /api/display/draw` parameter location.** The spec names a
//!   `DeletionParameters` schema with `application_name` and `element_ids`
//!   but the retrieved fragment did not pin whether they travel as query
//!   parameters or a request body. [`clear_query`] builds the query form,
//!   which is what the shipped Home Assistant recipe
//!   (`docs/integrations/busy-bar.md` §4.1) already uses; if a bench unit
//!   disagrees, that is a one-function change.
//! - **`scroll_rate` units.** The spec documents pixels **per minute**. The
//!   relay lane has shipped `scroll_rate: 20` since it was written, which
//!   under that reading is roughly three and a half minutes to cross the
//!   72-pixel matrix. Either the units differ on shipped firmware or that
//!   value is far too low. This module does not guess: it scrolls nothing,
//!   because its whole vocabulary is short enough not to need it (see
//!   [`super::phrase::MAX_ZONE_WORD_CHARS`]), and the open question is
//!   recorded rather than papered over.
//!
//! # Colors are RGBA, not RGB
//!
//! The spec's pattern for `led_notification_color` — and for an element's
//! `color` — is `^#[a-fA-F0-9]{8}$`: eight hex digits, `#RRGGBBAA`. Six-digit
//! CSS colors do not match it. [`Rgba`] can only render the eight-digit form,
//! so this surface cannot emit the shorter one by accident.

use serde_json::{json, Value};

/// The `application_name` this surface draws under.
///
/// Deliberately **not** `crate::relay::busybar::APPLICATION_NAME`. The bar
/// scopes drawings and clears by application name, so if the alert relay and
/// this surface shared one, each would silently wipe the other's content
/// every time it drew — two SecuraCV lanes fighting over one matrix. With two
/// names the bar's own `priority` field arbitrates between them, which is
/// what it is for: an alert-relay poke draws at 60..100 and a calm surface
/// pulse draws at [`PRIORITY_CALM`], so a real alarm wins the glass and the
/// surface resumes when the alert's hold expires.
///
/// Matches the spec's `^[a-zA-Z0-9._-]+$`, max 32 characters.
pub const APPLICATION_NAME: &str = "securacv-surface";

/// `POST` here to draw; `DELETE` here to clear.
pub const DRAW_PATH: &str = "/api/display/draw";

/// Draw priority for the calm and presence states — below the alert relay's
/// whole range (60..=100) so a poke always takes the glass.
pub const PRIORITY_CALM: u8 = 40;

/// Draw priority for a degraded or unknown surface state.
pub const PRIORITY_DEGRADED: u8 = 70;

/// Draw priority for a relayed life-safety advisory: the top of the scale,
/// matching what the relay uses for a heard alarm.
pub const PRIORITY_ADVISORY: u8 = 100;

// The ladder is ordered, and the whole ladder is inside the protocol's 1..=100
// range. Checked at compile time rather than by a test: a mis-ordered ladder
// would let a calm pulse outrank an advisory on the glass, and that is a thing
// to catch before the binary exists rather than while it runs.
const _: () = assert!(PRIORITY_CALM < PRIORITY_DEGRADED);
const _: () = assert!(PRIORITY_DEGRADED < PRIORITY_ADVISORY);
const _: () = assert!(PRIORITY_CALM >= 1 && PRIORITY_ADVISORY <= 100);

/// Which physical display an element is drawn on.
///
/// Both are reachable through the same `POST /api/display/draw`; the element's
/// `display` field selects one (`front` | `back`). This is the confirmed
/// mechanism the whole two-audience design rests on — there is no separate
/// rear-display endpoint, and none is needed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Display {
    /// The 72x16 RGB LED matrix. Room-facing, and therefore public.
    Front,
    /// The 160x80 monochrome display. Operator-facing.
    Back,
}

impl Display {
    /// The wire word.
    pub fn wire(self) -> &'static str {
        match self {
            Display::Front => "front",
            Display::Back => "back",
        }
    }

    /// Pixel width, from the two clients' display tables. Used only to bound
    /// how much text this module will build, never to claim a glyph metric.
    pub fn width_px(self) -> u16 {
        match self {
            Display::Front => 72,
            Display::Back => 160,
        }
    }

    /// Pixel height, same source and same caveat.
    pub fn height_px(self) -> u16 {
        match self {
            Display::Front => 16,
            Display::Back => 80,
        }
    }
}

/// An `#RRGGBBAA` color. The only constructor takes components, so the
/// six-digit form the spec's pattern rejects cannot be built here.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Rgba {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

impl Rgba {
    /// Fully opaque.
    pub const fn rgb(r: u8, g: u8, b: u8) -> Self {
        Rgba { r, g, b, a: 0xFF }
    }

    /// The same color at a given alpha — how the breathing pulse breathes.
    pub const fn with_alpha(self, a: u8) -> Self {
        Rgba { a, ..self }
    }

    /// The wire form, `#RRGGBBAA`.
    pub fn hex(self) -> String {
        format!("#{:02X}{:02X}{:02X}{:02X}", self.r, self.g, self.b, self.a)
    }
}

/// One element of a drawing. Only the element types this surface actually
/// uses are modeled; the device supports more (`image`, `animation`,
/// `xpmbitmap`, `rectangle`) and the design doc says why they are not here
/// yet.
#[derive(Clone, Debug, PartialEq)]
pub enum Element {
    /// Printable-ASCII text. The spec constrains this field to printable
    /// ASCII, which the closed public vocabulary already satisfies.
    Text {
        id: &'static str,
        text: String,
        font: Font,
        color: Rgba,
        display: Display,
        align: Align,
    },
    /// A self-ticking countdown. The device runs the clock, so a quiet-window
    /// timer on the rear keeps counting without the bridge saying anything —
    /// and keeps counting correctly across a bridge restart.
    Countdown {
        id: &'static str,
        /// Unix UTC seconds the countdown runs to.
        timestamp: u64,
        color: Rgba,
        display: Display,
        align: Align,
    },
}

/// Fonts the firmware spec enumerates. `Superscript` is present in the
/// firmware spec and absent from `busylib-py`'s literal type, so it is
/// modeled but unused here.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Font {
    Tiny,
    Small,
    Normal,
    Condensed,
    Bold,
    Large,
    ExtraLarge,
    Global,
    Superscript,
}

impl Font {
    /// The wire word.
    pub fn wire(self) -> &'static str {
        match self {
            Font::Tiny => "tiny",
            Font::Small => "small",
            Font::Normal => "normal",
            Font::Condensed => "condensed",
            Font::Bold => "bold",
            Font::Large => "large",
            Font::ExtraLarge => "extra_large",
            Font::Global => "global",
            Font::Superscript => "superscript",
        }
    }
}

/// Element alignment within its display.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Align {
    TopLeft,
    TopMid,
    TopRight,
    MidLeft,
    Center,
    MidRight,
    BottomLeft,
    BottomMid,
    BottomRight,
}

impl Align {
    /// The wire word.
    pub fn wire(self) -> &'static str {
        match self {
            Align::TopLeft => "top_left",
            Align::TopMid => "top_mid",
            Align::TopRight => "top_right",
            Align::MidLeft => "mid_left",
            Align::Center => "center",
            Align::MidRight => "mid_right",
            Align::BottomLeft => "bottom_left",
            Align::BottomMid => "bottom_mid",
            Align::BottomRight => "bottom_right",
        }
    }
}

/// The longest a drawing may live on the glass before it expires by itself.
///
/// This is the dead-man's switch, and it is the whole answer to constraint 5
/// ("fail visible, not silent"). Every element this module emits carries a
/// `timeout`, and the surface promises to redraw well inside it. So if the
/// bridge process dies, the hub loses power, the LAN drops, or the broker
/// goes away and takes the surface's confidence with it, the last drawing
/// ages off and the matrix goes **blank** — which is honest — instead of
/// holding a calm green pulse that means "everything is proved" while
/// nothing is proving anything.
///
/// A stale-but-plausible calm state is the failure a witness display must not
/// have, and no amount of error handling inside this process can prevent it,
/// because the dangerous cases are the ones where this process is not running
/// to handle anything. Only the device expiring the drawing prevents it.
pub const MAX_DRAW_TIMEOUT_MS: u64 = 60_000;

/// A complete drawing: what to put on which display, and how long it may
/// outlive the bridge that drew it.
#[derive(Clone, Debug, PartialEq)]
pub struct Frame {
    pub elements: Vec<Element>,
    /// The LED notification color, or `None` to leave it alone.
    pub led: Option<Rgba>,
    pub priority: u8,
    /// Milliseconds. Clamped to [`MAX_DRAW_TIMEOUT_MS`] by [`Frame::body`].
    pub timeout_ms: u64,
}

impl Frame {
    /// The complete `POST /api/display/draw` body.
    ///
    /// The `timeout` is stamped onto every element rather than the drawing as
    /// a whole, because the API carries it per element; clamping happens here
    /// so no caller can construct an unbounded drawing (FR-4, and the
    /// dead-man's switch above).
    pub fn body(&self) -> Value {
        let timeout = self.timeout_ms.clamp(1, MAX_DRAW_TIMEOUT_MS);
        let elements: Vec<Value> = self
            .elements
            .iter()
            .map(|e| element_json(e, timeout))
            .collect();
        let mut doc = json!({
            "application_name": APPLICATION_NAME,
            "priority": self.priority.clamp(1, 100),
            "elements": elements,
        });
        if let Some(led) = self.led {
            doc["led_notification_color"] = Value::String(led.hex());
        }
        doc
    }
}

fn element_json(e: &Element, timeout_ms: u64) -> Value {
    match e {
        Element::Text {
            id,
            text,
            font,
            color,
            display,
            align,
        } => json!({
            "id": id,
            "type": "text",
            "text": text,
            "font": font.wire(),
            "color": color.hex(),
            "display": display.wire(),
            "align": align.wire(),
            "width": display.width_px(),
            "timeout": timeout_ms,
        }),
        Element::Countdown {
            id,
            timestamp,
            color,
            display,
            align,
        } => json!({
            "id": id,
            "type": "countdown",
            // The spec types this as a digit string, not a number.
            "timestamp": timestamp.to_string(),
            "direction": "time_left",
            "show_hours": "when_non_zero",
            "color": color.hex(),
            "display": display.wire(),
            "align": align.wire(),
            "timeout": timeout_ms,
        }),
    }
}

/// Query string for `DELETE /api/display/draw`, scoped to this surface so a
/// clear never removes the alert relay's drawing or the bar's own content.
pub fn clear_query() -> String {
    format!("?application_name={APPLICATION_NAME}")
}

/// What an HTTP response to a draw means. Named rather than inferred at the
/// call site, because one of them is a trap.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DrawOutcome {
    /// The drawing landed.
    Drawn,
    /// **409: the drawing did not land** — something with a higher priority
    /// owns the display. This is a success-shaped failure: the request was
    /// well formed and the server was happy, and the glass shows something
    /// else. A surface that counted it as drawn would believe it had
    /// reaffirmed a state it never put up, which is exactly the stale-calm
    /// failure the timeout exists to prevent. It is therefore its own
    /// outcome, and the caller must not record it as a draw.
    RefusedLowerPriority,
    /// 403: the bar has an HTTP access key set and this request did not carry
    /// it, or carried the wrong one. Note 403, not 401 — the device answers
    /// Forbidden, and USB and loopback bypass the check entirely, so this is
    /// a Wi-Fi-only symptom.
    Forbidden,
    /// Any other status.
    Failed { status: u16 },
}

impl DrawOutcome {
    /// Classify a response status.
    pub fn from_status(status: u16) -> Self {
        match status {
            200..=299 => DrawOutcome::Drawn,
            409 => DrawOutcome::RefusedLowerPriority,
            403 => DrawOutcome::Forbidden,
            other => DrawOutcome::Failed { status: other },
        }
    }

    /// Did our content actually reach the glass?
    pub fn on_glass(self) -> bool {
        matches!(self, DrawOutcome::Drawn)
    }
}

/// Refuse the vendor cloud; accept any local address the owner points at.
///
/// The surface is local-first by construction (constraint 4). The bar's API
/// is also reachable through busy.app's cloud proxy under an account-linked
/// bearer token, and routing a *surface* through it would be worse than
/// routing an alert through it: the surface reaffirms its state on a cadence,
/// so the proxy would receive a continuous, timestamped record of when the
/// household's witness state changes — a finer timing oracle than the event
/// log itself is allowed to expose (Invariant III).
///
/// Reuses `crate::relay::busybar::validate_url`, which already refuses
/// `busy.app` and any subdomain, rather than writing a second rule that could
/// drift from it.
pub fn validate_url(url: &str) -> Result<(), String> {
    crate::relay::busybar::validate_url(url)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn text(display: Display, s: &str) -> Element {
        Element::Text {
            id: "front",
            text: s.to_string(),
            font: Font::Small,
            color: Rgba::rgb(0, 0xBE, 0x50),
            display,
            align: Align::Center,
        }
    }

    /// The firmware spec's pattern for a color is `^#[a-fA-F0-9]{8}$`. Six
    /// digits do not match it, and this type cannot produce six.
    #[test]
    fn a_color_is_always_eight_hex_digits() {
        let c = Rgba::rgb(0xFF, 0x8C, 0x00);
        assert_eq!(c.hex(), "#FF8C00FF");
        assert_eq!(c.with_alpha(0x46).hex(), "#FF8C0046");
        for hex in [c.hex(), c.with_alpha(0).hex(), Rgba::rgb(0, 0, 0).hex()] {
            assert_eq!(hex.len(), 9, "{hex} is not #RRGGBBAA");
            assert!(hex.starts_with('#'));
            assert!(hex[1..].chars().all(|ch| ch.is_ascii_hexdigit()));
        }
    }

    /// Constraint 5's mechanism. Every element carries an expiry, always, and
    /// it is bounded — so a drawing cannot outlive the process that made it
    /// by more than one ceiling.
    #[test]
    fn every_element_expires_and_the_expiry_is_bounded() {
        let frame = Frame {
            elements: vec![
                text(Display::Front, "FRONT DOOR"),
                text(Display::Back, "chain 12 verified"),
                Element::Countdown {
                    id: "quiet",
                    timestamp: 1_700_000_000,
                    color: Rgba::rgb(0xFF, 0xFF, 0xFF),
                    display: Display::Back,
                    align: Align::BottomMid,
                },
            ],
            led: None,
            priority: PRIORITY_CALM,
            timeout_ms: u64::MAX,
        };
        let body = frame.body();
        let elements = body["elements"].as_array().expect("elements");
        assert_eq!(elements.len(), 3);
        for e in elements {
            let t = e["timeout"].as_u64().expect("timeout");
            assert!(t > 0);
            assert!(
                t <= MAX_DRAW_TIMEOUT_MS,
                "an unbounded expiry would let a dead bridge leave a calm state up"
            );
        }
    }

    /// Both displays travel in one request, distinguished by the element's
    /// `display` field. This is the confirmed mechanism the whole
    /// two-audience design rests on.
    #[test]
    fn one_request_addresses_both_displays() {
        let frame = Frame {
            elements: vec![text(Display::Front, "TAMPER"), text(Display::Back, "detail")],
            led: None,
            priority: PRIORITY_DEGRADED,
            timeout_ms: 6_000,
        };
        let body = frame.body();
        let displays: Vec<&str> = body["elements"]
            .as_array()
            .unwrap()
            .iter()
            .map(|e| e["display"].as_str().unwrap())
            .collect();
        assert_eq!(displays, vec!["front", "back"]);
    }

    /// The surface and the alert relay must not share an application name, or
    /// each would wipe the other's drawing off the same matrix.
    #[test]
    fn the_surface_draws_under_its_own_application_name() {
        assert_ne!(APPLICATION_NAME, crate::relay::busybar::APPLICATION_NAME);
        // The spec constrains the name to `^[a-zA-Z0-9._-]+$`, max 32.
        assert!(APPLICATION_NAME.len() <= 32);
        assert!(APPLICATION_NAME
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '_' | '-')));
        assert!(clear_query().contains(APPLICATION_NAME));
    }

    /// A real alert must always win the glass over the calm surface, and the
    /// surface's own ladder must be ordered.
    /// The surface's own ladder is ordered at compile time (see the
    /// `const _: () = assert!` block above). What a test still has to check is
    /// the relationship with the OTHER SecuraCV lane drawing on this bar: a
    /// real alert must always win the glass from the idle surface.
    #[test]
    fn an_alert_outranks_the_calm_surface() {
        for class in [
            crate::relay::PokeClass::Tamper,
            crate::relay::PokeClass::Pattern,
            crate::relay::PokeClass::Integrity,
            crate::relay::PokeClass::Offline,
        ] {
            assert!(
                class.busybar_priority() > PRIORITY_CALM,
                "{class:?} must outrank the idle surface"
            );
        }
    }

    /// A 409 is a success-shaped failure: the request was fine and our
    /// content is not on the glass. Counting it as drawn is how a surface
    /// comes to believe it reaffirmed a state it never put up.
    #[test]
    fn a_refused_draw_is_not_a_draw() {
        assert_eq!(DrawOutcome::from_status(200), DrawOutcome::Drawn);
        assert!(DrawOutcome::from_status(200).on_glass());
        assert_eq!(
            DrawOutcome::from_status(409),
            DrawOutcome::RefusedLowerPriority
        );
        assert!(!DrawOutcome::from_status(409).on_glass());
        assert_eq!(DrawOutcome::from_status(403), DrawOutcome::Forbidden);
        assert!(!DrawOutcome::from_status(403).on_glass());
        assert_eq!(
            DrawOutcome::from_status(500),
            DrawOutcome::Failed { status: 500 }
        );
    }

    /// Constraint 4: local-first, and the vendor cloud is refused rather than
    /// discouraged.
    #[test]
    fn the_vendor_cloud_is_refused() {
        assert!(validate_url("http://10.0.4.20").is_ok());
        assert!(validate_url("http://192.168.1.20").is_ok());
        assert!(validate_url("https://api.busy.app").is_err());
        assert!(validate_url("https://busy.app/busybar").is_err());
        assert!(validate_url("https://api.dev.busy.app").is_err());
    }
}
