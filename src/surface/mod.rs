//! Display surfaces — rendering witness state onto glass a person looks at.
//!
//! A *surface* is a third thing, next to [`adapter`](crate::adapter) (claims
//! come **in**) and [`bridge`](crate::bridge) (coarse state goes **out** to a
//! home-automation ecosystem). A surface sends state out too, but to a piece
//! of glass on the owner's own LAN, for a person to read — not to a platform
//! that will store, index, or forward it.
//!
//! That difference is why surfaces are not filed under `bridge`. The bridge
//! rule is a **fixed publication cadence** that does not vary with event
//! occurrence, because a bridge's consumer (an Apple Home controller, a cloud
//! account) is a party whose logs the owner does not hold: a publication rate
//! that tracks the event rate hands that party a timing oracle, which is what
//! Invariant III exists to remove. A surface's consumer is a human standing in
//! the room. Holding a presence card on the glass for a readable dwell is the
//! entire job, and metering it to a metronome would make the surface useless
//! without protecting anyone. So surfaces get their own rule instead:
//!
//! > A surface may render only what its **class ceiling** admits, it may
//! > render it only through a **closed phrase vocabulary** that no network
//! > payload can extend, and every drawing it makes must **expire on its own**
//! > sooner than the surface promises to redraw it.
//!
//! The three clauses are the three failures a status display can have. The
//! first is showing something the room should not see. The second is
//! composing what it shows out of attacker- or accident-supplied text. The
//! third — the quiet one — is a display that keeps showing a calm state after
//! the thing feeding it has died, which is worse than a blank display,
//! because a blank display is honest.
//!
//! Surfaces in this module today:
//!
//! - [`busybar`] — the BUSY Bar surface. A Flipper Devices BUSY Bar
//!   ([busy.app](https://busy.app)) driven over its local HTTP API as a
//!   two-audience readout: the room-facing LED matrix carries public-class
//!   card content only, the operator-facing rear display carries the detail.
//!   Design of record: `docs/design/busybar_surface.md`.
//!
//! Not to be confused with the *shipped* SecuraCV display surfaces, which are
//! firmware, not kernel code: Canary Display (the wall glass,
//! `firmware/projects/canary-display/`), the Witness Wall (tvOS), and the
//! companion app. A surface in this module drives **someone else's hardware**
//! from the hub, which is exactly why its rules are written down rather than
//! assumed.

#[cfg(feature = "surface-busybar")]
pub mod busybar;
