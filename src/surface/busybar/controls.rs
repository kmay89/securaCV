//! The controls, and the one invariant that governs all of them.
//!
//! # No control on this device may affect witnessing
//!
//! Not the center button, not the dial, not the mode switch, not the lever.
//! They change what this **surface shows**. The kernel keeps sealing, the
//! sealed log keeps growing, the Canaries keep reporting, and nothing any
//! finger does to a desk gadget alters, pauses, exports, or unseals any of
//! it.
//!
//! This is not a nice-to-have. A physical mute on a desk accessory that
//! quiets a witness system is the precise failure this project exists to
//! prevent: it converts "the house is witnessing" into "the house is
//! witnessing unless someone reached over and stopped it," and the second
//! sentence is worth nothing in the moment it matters. Guarantees here are
//! `can't`, not `won't` (`AGENTS.md`), so the property is built three ways
//! and each is tested:
//!
//! 1. **The effect vocabulary is drawing.** [`Effect`] has two variants,
//!    both about pixels. There is no `Publish`, no `Append`, no `Ack`, no
//!    `Suppress`. A control handler literally cannot ask for anything else,
//!    because there is nothing else to ask for.
//! 2. **Witness state arrives immutably.** [`apply`] takes
//!    `&super::state::WitnessView`, never `&mut`. Controls hold no mutable
//!    path to witness state, so they cannot edit what they are shown, and
//!    the scrubber in particular is a read over a snapshot the ingest built.
//! 3. **The surface has no publish path.** The daemon's broker client is
//!    wrapped so it exposes `subscribe` and nothing else
//!    (`src/bin/busybar_surface.rs`), and the kernel is reached only through
//!    `GET` requests. Even the acknowledgement is one-way: pressing the
//!    center button clears the card *from this glass*, and deliberately does
//!    not publish to the household's shared `securacv/fleet/ack` topic.
//!
//! Point 3 costs something real and it is worth naming: you cannot
//! acknowledge the fleet from the bar. An ack made in Home Assistant or on
//! the wall glass still clears here, because the ingest subscribes that
//! topic — but the flow is one-way. That is the price of the surface having
//! no write path at all, and for a witness system it is the right price.
//!
//! # The dial
//!
//! Each detent steps one verified event. The front shows that event's public
//! card; the rear shows what was actually verified about it. Scrubbing is a
//! read over `super::state::WitnessView::timeline`, which the ingest built
//! from a signed export bundle — so the dial cannot alter, export, or unseal
//! anything, and there is no function reachable from here that could.
//!
//! # Where the events come from — status: **design, not built**
//!
//! The bar does expose its inputs: `GET /api/status/ws` upgrades to a
//! WebSocket that pushes `InputEvent { key, state, timestamp_ms }` over
//! **protobuf**, with the key enum `up, down, ok, back, start, busy, custom,
//! off, apps, settings` (firmware OpenAPI 27.7.0, `openapi/input.yaml`; the
//! stream is confirmed local-only — the official Python client refuses it in
//! cloud mode).
//!
//! This module implements the whole decision layer for those events and
//! ships **no transport for them**, for two honest reasons:
//!
//! - The frames are protobuf and the `BSB_State.State` schema is not
//!   published with field numbers anywhere reachable. Decoding it would mean
//!   guessing a wire format, which is worse than not doing it.
//! - Which physical control each key name denotes is not stated anywhere.
//!   `up`/`down` are plausibly the dial and `busy`/`start` plausibly the
//!   lever, and "plausibly" is not a thing to build a mapping on. One bench
//!   session with a real bar settles it; see
//!   `docs/design/busybar_surface.md` §9.
//!
//! So [`apply`] is driven today by [`ControlSource`] implementations that do
//! not need either answer, and the physical-input implementation is a shim
//! away once the two questions above are answered on a bench.

use super::state::{SurfaceMode, SurfaceState, Timings, WitnessView};

/// Which way the dial turned.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Turn {
    /// Back through the timeline, toward older events.
    Back,
    /// Toward the present.
    Forward,
}

/// Something a person did to the surface.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum Control {
    /// Center button, short press: acknowledge the card on the glass and
    /// advance to the next one waiting.
    CenterPress,
    /// Center button, long press: enter or leave the timeline scrubber.
    CenterLongPress,
    /// One or more dial detents.
    Dial { turn: Turn, detents: u16 },
    /// Mode switch: cycle Live -> Timeline -> Chain -> Dark.
    ModeSwitch,
    /// Jump straight to a named mode. The physical switch only cycles; a
    /// dashboard button naturally names the position it wants, and both go
    /// through the same vocabulary so the two lanes cannot diverge in what
    /// they are able to ask for.
    SetMode(SurfaceMode),
    /// Top lever: open a quiet-display window. `window_ms` overrides the
    /// configured default.
    LeverPull { window_ms: Option<u64> },
    /// Top lever released: close the quiet window early.
    LeverRelease,
}

/// What a control asks the surface to do next.
///
/// The complete vocabulary, and it is entirely about pixels. Adding a variant
/// that is not is how this invariant would be lost, which is why
/// [`Effect::is_display_only`] matches exhaustively — a new variant will not
/// compile until someone has looked at it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Effect {
    /// Redraw both displays from the current state.
    Redraw,
    /// Remove everything this surface has drawn from both displays.
    Clear,
}

impl Effect {
    /// Every effect this surface can produce touches only the glass.
    ///
    /// If you ever find yourself wanting to return `false` from an arm here,
    /// stop: you are adding a control that reaches past the display, and the
    /// module documentation explains why that is the one thing this surface
    /// must not grow. Add the capability somewhere with a write path and an
    /// audit trail, not on a desk accessory.
    pub fn is_display_only(self) -> bool {
        match self {
            Effect::Redraw => true,
            Effect::Clear => true,
        }
    }
}

/// Apply one control.
///
/// `view` is `&`, not `&mut`: witness state is a read here and the compiler
/// enforces it.
pub fn apply(
    state: &mut SurfaceState,
    view: &WitnessView,
    control: Control,
    timings: &Timings,
    now_ms: u64,
) -> Effect {
    match control {
        Control::CenterPress => {
            state.acknowledge(now_ms, timings);
            Effect::Redraw
        }
        Control::CenterLongPress => {
            if state.mode() == SurfaceMode::Timeline {
                state.set_mode(SurfaceMode::Live);
            } else {
                state.set_mode(SurfaceMode::Timeline);
                state.scrub_back(0, view.timeline.len());
            }
            Effect::Redraw
        }
        Control::Dial { turn, detents } => {
            // A turn is also how you enter the scrubber, so the dial works
            // from Live without anyone having to know about the long press.
            if state.mode() != SurfaceMode::Timeline {
                state.set_mode(SurfaceMode::Timeline);
            }
            match turn {
                Turn::Back => state.scrub_back(usize::from(detents), view.timeline.len()),
                Turn::Forward => state.scrub_forward(usize::from(detents)),
            }
            Effect::Redraw
        }
        Control::ModeSwitch => {
            state.cycle_mode();
            Effect::Redraw
        }
        Control::SetMode(mode) => {
            state.set_mode(mode);
            Effect::Redraw
        }
        Control::LeverPull { window_ms } => {
            state.begin_quiet(now_ms, window_ms.unwrap_or(timings.quiet_window_ms));
            Effect::Redraw
        }
        Control::LeverRelease => {
            state.end_quiet();
            Effect::Redraw
        }
    }
}

/// A place controls come from.
///
/// Implementations poll for the next pending control, or return `None`. Kept
/// as a trait so the physical-input lane can be added later without touching
/// the decision layer above — see the module docs for why that lane is not
/// here yet.
pub trait ControlSource {
    /// The next pending control, if any. Must not block.
    fn poll(&mut self) -> Option<Control>;

    /// A short operator-facing name for the rear's diagnostics.
    fn name(&self) -> &'static str;
}

/// Controls carried over MQTT, from Home Assistant or any other local client.
///
/// This is the lane that works today. It is **read-only with respect to
/// witnessing** for exactly the same reason every other lane is: whatever a
/// message asks for, the only thing it can produce is an [`Effect`], and
/// every [`Effect`] is a drawing. Anyone who can write to the broker can
/// change what this glass shows; nobody, through this path, can change what
/// the house witnesses.
///
/// Topic suffixes under the configured command prefix:
///
/// | suffix | payload | control |
/// |---|---|---|
/// | `ack` | anything | [`Control::CenterPress`] |
/// | `mode` | `live` / `timeline` / `chain` / `dark` / `next` | mode |
/// | `scrub` | signed integer, detents (negative = back) | [`Control::Dial`] |
/// | `quiet` | seconds, or `off` | lever |
#[derive(Default)]
pub struct MqttControlSource {
    queue: std::collections::VecDeque<Control>,
}

/// Most controls queued from the wire before new ones are dropped (FR-4).
pub const MAX_QUEUED_CONTROLS: usize = 32;

impl MqttControlSource {
    /// A fresh, empty source.
    pub fn new() -> Self {
        Self::default()
    }

    /// Offer one received command message. Returns `true` when it parsed to a
    /// control. Unknown suffixes and unparsable payloads are dropped without
    /// comment — a surface must not be steerable by malformed input.
    pub fn offer(&mut self, suffix: &str, payload: &[u8], timings: &Timings) -> bool {
        let raw = String::from_utf8_lossy(payload);
        let raw = raw.trim();
        let control = match suffix {
            "ack" => Some(Control::CenterPress),
            "mode" => match raw.to_ascii_lowercase().as_str() {
                "live" => Some(Control::SetMode(SurfaceMode::Live)),
                "timeline" => Some(Control::SetMode(SurfaceMode::Timeline)),
                "chain" => Some(Control::SetMode(SurfaceMode::ChainHealth)),
                "dark" => Some(Control::SetMode(SurfaceMode::Dark)),
                "next" => Some(Control::ModeSwitch),
                _ => None,
            },
            "scrub" => raw.parse::<i32>().ok().map(|n| Control::Dial {
                turn: if n < 0 { Turn::Back } else { Turn::Forward },
                detents: n.unsigned_abs().min(u32::from(u16::MAX)) as u16,
            }),
            "quiet" => {
                if raw.eq_ignore_ascii_case("off") {
                    Some(Control::LeverRelease)
                } else {
                    raw.parse::<u64>().ok().map(|secs| Control::LeverPull {
                        window_ms: Some(secs.saturating_mul(1000).min(timings.quiet_window_ms * 8)),
                    })
                }
            }
            _ => None,
        };
        match control {
            Some(c) => self.push(Some(c)),
            None => false,
        }
    }

    fn push(&mut self, control: Option<Control>) -> bool {
        let Some(c) = control else { return false };
        if self.queue.len() >= MAX_QUEUED_CONTROLS {
            return false;
        }
        self.queue.push_back(c);
        true
    }
}

impl ControlSource for MqttControlSource {
    fn poll(&mut self) -> Option<Control> {
        self.queue.pop_front()
    }

    fn name(&self) -> &'static str {
        "mqtt"
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::surface::busybar::card::Badge;
    use crate::surface::busybar::phrase::PublicPhrase;
    use crate::surface::busybar::state::{DeviceCards, LinkState, TimelineEntry};

    fn view() -> WitnessView {
        WitnessView {
            link: LinkState {
                broker_live: true,
                kernel_live: Some(true),
            },
            devices: vec![DeviceCards {
                device_id: "porch".into(),
                name: None,
                zone: None,
                last_seen_secs_ago: Some(3),
                online: true,
                chain_len: 12,
                badge: Badge::Verified,
                cards: Vec::new(),
            }],
            timeline: (0..4)
                .map(|i| TimelineEntry {
                    phrase: PublicPhrase::Presence { zone: None },
                    bucket_start_epoch_s: 1_700_000_000 + i * 600,
                    bucket_size_s: 600,
                    attestation: None,
                    bundle_badge: Badge::Signed,
                })
                .collect(),
            verified_recent: 4,
            on_call: false,
        }
    }

    /// Constraint 1, stated as plainly as a test can state it: whatever a
    /// person does to this device, the only thing that can come out is a
    /// drawing.
    ///
    /// The real enforcement is the type — [`Effect`] has no variant that
    /// reaches past the glass, so a control handler cannot ask for one. This
    /// test walks every control anyway, because a reader looking for the
    /// guarantee should find it asserted somewhere and not only argued.
    #[test]
    fn no_control_can_do_anything_but_draw() {
        let timings = Timings::default();
        let v = view();
        let every_control = [
            Control::CenterPress,
            Control::CenterLongPress,
            Control::Dial {
                turn: Turn::Back,
                detents: 1,
            },
            Control::Dial {
                turn: Turn::Forward,
                detents: 3,
            },
            Control::ModeSwitch,
            Control::SetMode(SurfaceMode::Dark),
            Control::SetMode(SurfaceMode::Live),
            Control::LeverPull { window_ms: None },
            Control::LeverPull {
                window_ms: Some(60_000),
            },
            Control::LeverRelease,
        ];
        for control in every_control {
            let mut state = SurfaceState::new();
            let effect = apply(&mut state, &v, control, &timings, 1_000);
            assert!(
                effect.is_display_only(),
                "{control:?} produced an effect that reaches past the glass"
            );
        }
    }

    /// The witness snapshot reaches controls immutably, and comes back
    /// unchanged. If this ever needs `&mut`, the invariant is gone.
    #[test]
    fn controls_cannot_touch_witness_state() {
        let timings = Timings::default();
        let before = view();
        let mut state = SurfaceState::new();
        for control in [
            Control::CenterPress,
            Control::ModeSwitch,
            Control::Dial {
                turn: Turn::Back,
                detents: 2,
            },
            Control::LeverPull { window_ms: None },
        ] {
            apply(&mut state, &before, control, &timings, 1_000);
        }
        assert_eq!(before, view(), "witness state changed under a control");
    }

    /// Dark is a display mode. It is not a mute button, and the surface's own
    /// view of witness state is identical on either side of it.
    #[test]
    fn dark_does_not_pause_witnessing() {
        let timings = Timings::default();
        let v = view();
        let mut state = SurfaceState::new();
        let before = v.clone();
        apply(
            &mut state,
            &v,
            Control::SetMode(SurfaceMode::Dark),
            &timings,
            0,
        );
        assert_eq!(state.mode(), SurfaceMode::Dark);
        assert_eq!(v, before);
        assert_eq!(v.devices.len(), 1, "the fleet is still there");
        assert_eq!(v.timeline.len(), 4, "the log is still there");
    }

    /// Scrubbing is a read. The timeline it walks is the same length and the
    /// same content afterward.
    #[test]
    fn scrubbing_is_read_only() {
        let timings = Timings::default();
        let v = view();
        let snapshot = v.clone();
        let mut state = SurfaceState::new();
        for _ in 0..20 {
            apply(
                &mut state,
                &v,
                Control::Dial {
                    turn: Turn::Back,
                    detents: 1,
                },
                &timings,
                0,
            );
        }
        assert_eq!(v.timeline, snapshot.timeline);
        assert_eq!(state.mode(), SurfaceMode::Timeline);
        assert_eq!(
            state.scrub_index(),
            Some(v.timeline.len() - 1),
            "the cursor stops at the oldest event rather than wrapping"
        );
    }

    #[test]
    fn the_mode_switch_cycles_and_comes_home() {
        let timings = Timings::default();
        let v = view();
        let mut state = SurfaceState::new();
        assert_eq!(state.mode(), SurfaceMode::Live);
        for _ in 0..4 {
            apply(&mut state, &v, Control::ModeSwitch, &timings, 0);
        }
        assert_eq!(state.mode(), SurfaceMode::Live);
    }

    #[test]
    fn the_center_button_advances_through_waiting_cards() {
        let timings = Timings::default();
        let v = view();
        let mut state = SurfaceState::new();
        state.note_presence("porch", None, 0, &timings);
        state.note_presence("gate", None, 1_000, &timings);
        assert_eq!(state.pending_count(), 1);
        apply(&mut state, &v, Control::CenterPress, &timings, 2_000);
        assert_eq!(state.pending_count(), 0, "the next card came forward");
        apply(&mut state, &v, Control::CenterPress, &timings, 3_000);
        assert_eq!(state.pending_count(), 0);
    }

    // ---- the MQTT control lane -------------------------------------------

    #[test]
    fn the_command_lane_parses_its_whole_vocabulary() {
        let timings = Timings::default();
        let mut src = MqttControlSource::new();
        assert!(src.offer("ack", b"", &timings));
        assert!(src.offer("mode", b"dark", &timings));
        assert!(src.offer("mode", b"next", &timings));
        assert!(src.offer("scrub", b"-3", &timings));
        assert!(src.offer("scrub", b"2", &timings));
        assert!(src.offer("quiet", b"600", &timings));
        assert!(src.offer("quiet", b"off", &timings));

        assert_eq!(src.poll(), Some(Control::CenterPress));
        assert_eq!(src.poll(), Some(Control::SetMode(SurfaceMode::Dark)));
        assert_eq!(src.poll(), Some(Control::ModeSwitch));
        assert_eq!(
            src.poll(),
            Some(Control::Dial {
                turn: Turn::Back,
                detents: 3
            })
        );
        assert_eq!(
            src.poll(),
            Some(Control::Dial {
                turn: Turn::Forward,
                detents: 2
            })
        );
        assert_eq!(
            src.poll(),
            Some(Control::LeverPull {
                window_ms: Some(600_000)
            })
        );
        assert_eq!(src.poll(), Some(Control::LeverRelease));
        assert_eq!(src.poll(), None);
    }

    /// A surface must not be steerable by malformed input.
    #[test]
    fn junk_on_the_command_topic_is_dropped() {
        let timings = Timings::default();
        let mut src = MqttControlSource::new();
        assert!(!src.offer("mode", b"purple", &timings));
        assert!(!src.offer("scrub", b"seventeen", &timings));
        assert!(!src.offer("quiet", b"soon", &timings));
        assert!(!src.offer("unseal", b"yes please", &timings));
        assert!(!src.offer("export", b"1", &timings));
        assert_eq!(src.poll(), None);
    }

    /// Bounded (FR-4): a flood on the command topic must not grow this
    /// process, and it must not be able to queue work indefinitely.
    #[test]
    fn the_command_queue_is_bounded() {
        let timings = Timings::default();
        let mut src = MqttControlSource::new();
        let mut accepted = 0;
        for _ in 0..(MAX_QUEUED_CONTROLS * 10) {
            if src.offer("ack", b"", &timings) {
                accepted += 1;
            }
        }
        assert_eq!(accepted, MAX_QUEUED_CONTROLS);
    }

    /// A quiet window asked for over the wire is clamped. Someone who can
    /// publish must not be able to quiet the presence cards for a year.
    #[test]
    fn a_wire_requested_quiet_window_is_clamped() {
        let timings = Timings::default();
        let mut src = MqttControlSource::new();
        assert!(src.offer("quiet", b"999999999", &timings));
        let Some(Control::LeverPull { window_ms }) = src.poll() else {
            panic!("expected a lever pull");
        };
        assert_eq!(window_ms, Some(timings.quiet_window_ms * 8));
    }
}
