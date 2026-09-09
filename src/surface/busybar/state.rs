//! The surface state machine: what each display should show, right now.
//!
//! Pure and clock-free — callers pass monotonic milliseconds — so every rule
//! below is unit-tested without a device, a broker, or a timer.
//!
//! # The animation rule, and why stillness is the alert
//!
//! The living-canary health cue on the shipped glass is a slow breath meaning
//! *the chain is intact and the witnesses are alive*
//! (`docs/hardware/display_living_canary.md`; the mood engine is
//! `firmware/projects/canary-display/include/canary/care/bird_mood.h`). This
//! surface keeps that grammar and sharpens it: when verification fails or a
//! Canary drops out, the pulse **stops** and the color shifts. Stillness is
//! the alert. Motion is reassurance.
//!
//! That inverts the usual convention on purpose, and the reason is not taste.
//! An animated alarm trains a household to feel watched by the very object
//! whose job is to make watching legible; a device that flashes at people in
//! a hallway has become the thing it was built to answer. So there are no
//! flashing alert states here, and a reviewer who asks for one should be
//! pointed at this paragraph.
//!
//! It also happens to be the honest direction. A flashing alarm needs a live
//! process to flash it, so its absence is silence; a breathing calm state
//! needs a live process to breathe, so *its* absence is visible. Which brings
//! the two mechanisms together: the pulse is driven by the same periodic
//! redraw that arms the device-side expiry in
//! [`super::device::MAX_DRAW_TIMEOUT_MS`]. One cadence does both jobs — the
//! breath *is* the liveness proof, and a bridge that dies stops breathing and
//! then goes dark, in that order, on the glass.

use std::collections::BTreeMap;

use super::card::{front_phrase, Badge, Card, ClassOptions, Refusal};
use super::device::{
    Align, Display, Element, Font, Frame, Rgba, PRIORITY_ADVISORY, PRIORITY_CALM, PRIORITY_DEGRADED,
};
use super::phrase::{AdvisoryWord, DegradedWord, PublicPhrase, UnknownWord, ZoneWord};

/// Most Canaries the surface will track. A bound, not a capacity estimate
/// (FR-4): a broker flood must not grow this process without limit.
pub const MAX_TRACKED_DEVICES: usize = 64;

/// Most rear lines rendered. The rear display is 160x80; eight short lines is
/// a comfortable ceiling and, more to the point, a bound.
pub const MAX_REAR_LINES: usize = 8;

/// Most presence cards held waiting for an acknowledgement.
pub const MAX_PENDING_PRESENCE: usize = 8;

/// The "all is proved" green. The same value the alert relay uses for its
/// drill, so one household reads one palette across both lanes.
pub const COLOR_CALM: Rgba = Rgba::rgb(0x00, 0xBE, 0x50);
/// Amber: the chain or a witness is in trouble.
pub const COLOR_DEGRADED: Rgba = Rgba::rgb(0xFF, 0x8C, 0x00);
/// Red: a witness reports interference with itself, or an advisory is up.
pub const COLOR_ALERT: Rgba = Rgba::rgb(0xFF, 0x00, 0x00);
/// Cool blue: this surface cannot see witness state. Distinct from calm and
/// from degraded, because "I do not know" is a third thing.
pub const COLOR_UNKNOWN: Rgba = Rgba::rgb(0x3A, 0x6E, 0xA5);
/// Violet: a microphone or camera in the room is live. Its own color because
/// it is its own kind of fact — neither witness health nor witness trouble.
pub const COLOR_ON_CALL: Rgba = Rgba::rgb(0x9A, 0x4D, 0xD6);

/// How the front matrix is moving.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Motion {
    /// Breathing: the chain is intact and the witnesses are alive.
    Breathing,
    /// Still: something is wrong, or this surface does not know.
    Still,
}

/// Surface modes, cycled by the mode switch. Exhaustive by construction
/// (FR-8: every mode is explicit).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SurfaceMode {
    /// Live witness state.
    #[default]
    Live,
    /// The read-only scrubber over recent verified events.
    Timeline,
    /// Chain and per-Canary health; presence cards do not interrupt.
    ChainHealth,
    /// Both displays blank.
    ///
    /// Dark blanks the **glass**, not the witnessing. The kernel keeps
    /// sealing, the Canaries keep reporting, the log keeps growing; the only
    /// thing that stops is this surface drawing. The rear says so in as many
    /// words whenever Dark is entered, because a blank security display that
    /// does not explain itself is indistinguishable from a broken one — and
    /// because a household must never be able to acquire the belief that the
    /// bar on the desk is a mute button for the witness system.
    Dark,
}

impl SurfaceMode {
    /// The next mode in the switch's cycle.
    pub fn next(self) -> Self {
        match self {
            SurfaceMode::Live => SurfaceMode::Timeline,
            SurfaceMode::Timeline => SurfaceMode::ChainHealth,
            SurfaceMode::ChainHealth => SurfaceMode::Dark,
            SurfaceMode::Dark => SurfaceMode::Live,
        }
    }

    /// The operator-facing word.
    pub fn label(self) -> &'static str {
        match self {
            SurfaceMode::Live => "LIVE",
            SurfaceMode::Timeline => "TIMELINE",
            SurfaceMode::ChainHealth => "CHAIN",
            SurfaceMode::Dark => "DARK",
        }
    }
}

/// How this surface reaches the bar, and therefore where the trust boundary
/// sits. Rendered on the rear so it is legible at a glance rather than
/// buried in a config file.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TransportPath {
    /// USB virtual Ethernet — the bar's own `10.0.4.20`. Nothing leaves the
    /// cable.
    Usb,
    /// The owner's LAN.
    Lan,
    /// The vendor's cloud proxy.
    ///
    /// Modeled so the trust boundary has a name, and refused at config load
    /// by [`super::device::validate_url`]: a surface reaffirms its state on a
    /// cadence, so a cloud path would hand a third party a continuous,
    /// timestamped record of when the household's witness state changes.
    /// The rear can therefore never actually print this word, which is the
    /// point.
    Cloud,
}

impl TransportPath {
    /// Classify a configured base URL. Does not decide whether the URL is
    /// allowed — that is [`super::device::validate_url`]'s job — so the two
    /// can be tested against each other.
    pub fn classify(url: &str) -> TransportPath {
        let rest = url
            .strip_prefix("http://")
            .or_else(|| url.strip_prefix("https://"))
            .unwrap_or(url);
        let host = rest
            .split(['/', '?', '#'])
            .next()
            .unwrap_or_default()
            .rsplit('@')
            .next()
            .unwrap_or_default()
            .split(':')
            .next()
            .unwrap_or_default()
            .trim_end_matches('.')
            .to_ascii_lowercase();
        if host == "busy.app" || host.ends_with(".busy.app") {
            TransportPath::Cloud
        } else if host == "10.0.4.20" {
            TransportPath::Usb
        } else {
            TransportPath::Lan
        }
    }

    /// The rear's word for this path.
    pub fn label(self) -> &'static str {
        match self {
            TransportPath::Usb => "usb",
            TransportPath::Lan => "lan",
            TransportPath::Cloud => "cloud",
        }
    }
}

/// Which of the surface's two upstreams are answering.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LinkState {
    /// The MQTT broker has been heard from inside the staleness window.
    pub broker_live: bool,
    /// The witness kernel's read-only API answered the last poll. `None` when
    /// no kernel URL is configured — a broker-only deployment is a valid
    /// setup and must not report a kernel it was never asked to reach.
    pub kernel_live: Option<bool>,
}

impl LinkState {
    /// The unknown word this link state earns, if any. The broker is named
    /// first: without it there is no fleet state at all, so its loss is the
    /// more complete blindness.
    pub fn unknown(self) -> Option<UnknownWord> {
        if !self.broker_live {
            Some(UnknownWord::BrokerLost)
        } else if self.kernel_live == Some(false) {
            Some(UnknownWord::KernelLost)
        } else {
            None
        }
    }
}

/// One Canary, as the surface sees it. Built by the ingest from
/// `crate::fleet_peers::PeerTable` — which already does the TOFU pinning and
/// the Ed25519 chain check — plus that device's card set.
#[derive(Clone, Debug, PartialEq)]
pub struct DeviceCards {
    /// The topic's `<device_id>` segment. Operator-facing only; it never
    /// reaches the front matrix.
    pub device_id: String,
    /// The owner-authored name from the retained `meta` topic.
    ///
    /// Rear only, and sanitized. It is human-authored but it arrives over
    /// MQTT, so anyone who can write to the broker can set it — which is
    /// exactly why it is not eligible for the room-facing matrix. The front's
    /// only variable word comes from the operator's own config file
    /// (`super::phrase`).
    pub name: Option<String>,
    /// The operator-authored label for this Canary's zone, resolved from the
    /// surface config's zone table. The one word the front may carry.
    pub zone: Option<ZoneWord>,
    /// Seconds since anything was heard from this device.
    pub last_seen_secs_ago: Option<u64>,
    /// A live signed chain publish verified against the pin inside the
    /// freshness window.
    pub online: bool,
    /// Chain length from the retained `chain` topic.
    pub chain_len: u64,
    /// The trust badge. `Verified` only where this process checked a
    /// signature against a pinned key (AD-Core §2.5).
    pub badge: Badge,
    /// This device's cards, one per announced entity.
    pub cards: Vec<Card>,
}

/// One verified event in the scrubber's window.
#[derive(Clone, Debug, PartialEq)]
pub struct TimelineEntry {
    /// The event's public phrase, already classed. Built by the ingest so the
    /// scrubber cannot introduce text the classing gate never saw.
    pub phrase: PublicPhrase,
    /// Coarse bucket start, seconds. Never a precise timestamp: the event
    /// contract buckets time on purpose (§3) and a scrubber that un-buckets
    /// it would rebuild what the bucketing removed.
    pub bucket_start_epoch_s: u64,
    /// Bucket width, seconds.
    pub bucket_size_s: u64,
    /// The event's attestation tier, verbatim from the export
    /// (`device` / `adapter` / `ha-bridged`).
    pub attestation: Option<String>,
    /// The chain-level verdict for the export this event came from.
    ///
    /// Deliberately chain-level: the kernel's `ExportEvent` carries **no
    /// per-event signature** (`src/lib.rs`), so a scrubber that printed
    /// "verified" beside one event would be claiming a check nobody
    /// performed. What was actually verified is the bundle, and that is what
    /// the rear says.
    pub bundle_badge: Badge,
}

/// Everything the surface knows about witness state, as a read-only snapshot.
///
/// Controls receive this by shared reference and never by `&mut`. That is the
/// type-level half of constraint 1 (no control may affect witnessing): a
/// control handler cannot mutate witness state because it is not given a
/// mutable path to any. The other half is [`super::controls::Effect`], whose
/// whole vocabulary is drawing.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct WitnessView {
    pub link: LinkState,
    pub devices: Vec<DeviceCards>,
    /// Recent verified events, oldest first.
    pub timeline: Vec<TimelineEntry>,
    /// Events in the timeline window, for the rear's count line.
    pub verified_recent: u32,
    /// A microphone or camera in this room is live, per the household's own
    /// signal. See [`PublicPhrase::OnCall`] — not a witness claim, and fed
    /// only when the operator configured a topic for it.
    pub on_call: bool,
}

/// Timings. Every one is a bound as well as a preference (FR-4).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Timings {
    /// How long a presence card holds the matrix before decaying to idle.
    pub presence_dwell_ms: u64,
    /// Minimum gap between presence draws for one Canary. The anti-strobe
    /// rule: a motion sensor that retriggers every two seconds must not turn
    /// the hallway into a strobe, so the second and subsequent triggers
    /// inside this window extend nothing and draw nothing.
    pub presence_debounce_ms: u64,
    /// One step of the breath, and the surface's redraw cadence.
    pub pulse_step_ms: u64,
    /// Steps per breath cycle.
    pub pulse_steps: u8,
    /// The device-side expiry is this many redraw steps. Above 1 so an
    /// ordinary late redraw does not blink the glass; low enough that a dead
    /// bridge goes dark within a breath or two.
    pub watchdog_steps: u32,
    /// Broker silence after which the surface reports Unknown rather than
    /// last-known state.
    pub broker_stale_ms: u64,
    /// Default quiet-display window for the lever.
    pub quiet_window_ms: u64,
}

impl Default for Timings {
    fn default() -> Self {
        Timings {
            presence_dwell_ms: 8_000,
            presence_debounce_ms: 30_000,
            pulse_step_ms: 2_000,
            pulse_steps: 4,
            watchdog_steps: 3,
            broker_stale_ms: 90_000,
            quiet_window_ms: 30 * 60_000,
        }
    }
}

impl Timings {
    /// The device-side expiry stamped on every drawing, clamped to the
    /// protocol ceiling.
    pub fn draw_timeout_ms(&self) -> u64 {
        self.pulse_step_ms
            .saturating_mul(u64::from(self.watchdog_steps))
            .clamp(1, super::device::MAX_DRAW_TIMEOUT_MS)
    }
}

/// One presence card waiting to be seen.
#[derive(Clone, Debug, PartialEq)]
struct Pending {
    zone: Option<ZoneWord>,
    queued_ms: u64,
}

/// Surface-only state: everything a control is allowed to move.
///
/// Nothing here is witness state. Every field changes what the glass shows
/// and nothing else, which is what makes constraint 1 checkable by reading
/// one struct.
#[derive(Clone, Debug, Default)]
pub struct SurfaceState {
    mode: SurfaceMode,
    /// Quiet-display window expiry, if the lever is pulled.
    quiet_until_ms: Option<u64>,
    /// The presence card currently dwelling, and until when.
    dwelling: Option<(Option<ZoneWord>, u64)>,
    /// Presence cards queued behind the dwelling one.
    pending: Vec<Pending>,
    /// Last accepted presence draw per device, for the anti-strobe gap.
    last_presence_ms: BTreeMap<String, u64>,
    /// Scrubber cursor: `None` is live, `Some(i)` indexes the timeline from
    /// the newest end (0 = newest).
    scrub: Option<usize>,
}

impl SurfaceState {
    /// A fresh surface in Live mode.
    pub fn new() -> Self {
        Self::default()
    }

    /// The current mode.
    pub fn mode(&self) -> SurfaceMode {
        self.mode
    }

    /// Switch modes. Leaving Timeline drops the scrubber cursor, so the glass
    /// never keeps showing a scrubbed event after the operator has moved on.
    pub fn set_mode(&mut self, mode: SurfaceMode) {
        self.mode = mode;
        if mode != SurfaceMode::Timeline {
            self.scrub = None;
        }
    }

    /// Cycle to the next mode.
    pub fn cycle_mode(&mut self) {
        self.set_mode(self.mode.next());
    }

    /// Open a quiet-display window.
    pub fn begin_quiet(&mut self, now_ms: u64, window_ms: u64) {
        self.quiet_until_ms = Some(now_ms.saturating_add(window_ms));
    }

    /// Close the quiet window early.
    pub fn end_quiet(&mut self) {
        self.quiet_until_ms = None;
    }

    /// Milliseconds left in the quiet window, if one is open.
    pub fn quiet_remaining_ms(&self, now_ms: u64) -> Option<u64> {
        self.quiet_until_ms
            .filter(|&u| u > now_ms)
            .map(|u| u - now_ms)
    }

    /// Offer a presence observation to the surface.
    ///
    /// Returns `false` when the anti-strobe gap swallowed it. A swallowed
    /// observation changes nothing at all — it does not extend the current
    /// dwell and it does not queue — because either would let a retriggering
    /// sensor hold the matrix indefinitely, which is the same failure as a
    /// strobe with extra steps.
    pub fn note_presence(
        &mut self,
        device_id: &str,
        zone: Option<ZoneWord>,
        now_ms: u64,
        timings: &Timings,
    ) -> bool {
        if let Some(&last) = self.last_presence_ms.get(device_id) {
            if now_ms.saturating_sub(last) < timings.presence_debounce_ms {
                return false;
            }
        }
        if self.last_presence_ms.len() >= MAX_TRACKED_DEVICES
            && !self.last_presence_ms.contains_key(device_id)
        {
            // Bounded (FR-4). Drop the oldest stamp rather than refusing the
            // new device: a flood must not lock a real Canary out of the
            // debounce map for good.
            if let Some(oldest) = self
                .last_presence_ms
                .iter()
                .min_by_key(|(_, &t)| t)
                .map(|(k, _)| k.clone())
            {
                self.last_presence_ms.remove(&oldest);
            }
        }
        self.last_presence_ms.insert(device_id.to_string(), now_ms);

        match &self.dwelling {
            Some((_, until)) if *until > now_ms => {
                if self.pending.len() < MAX_PENDING_PRESENCE {
                    self.pending.push(Pending {
                        zone,
                        queued_ms: now_ms,
                    });
                }
            }
            _ => {
                self.dwelling = Some((zone, now_ms.saturating_add(timings.presence_dwell_ms)));
            }
        }
        true
    }

    /// Acknowledge the dwelling card and advance to the next pending one.
    ///
    /// Local to this glass. It does not publish, and in particular it does
    /// not touch the household's shared `securacv/fleet/ack` topic: this
    /// surface has no publish path at all (see [`super::controls`]). An ack
    /// made elsewhere in the household still clears here, because the ingest
    /// subscribes that topic — the flow is one-way on purpose.
    pub fn acknowledge(&mut self, now_ms: u64, timings: &Timings) {
        if self.pending.is_empty() {
            self.dwelling = None;
            return;
        }
        let next = self.pending.remove(0);
        self.dwelling = Some((next.zone, now_ms.saturating_add(timings.presence_dwell_ms)));
    }

    /// Clear the dwelling card and everything queued behind it — what a
    /// household acknowledgement heard on the wire does here.
    pub fn clear_presence(&mut self) {
        self.dwelling = None;
        self.pending.clear();
    }

    /// Number of presence cards waiting behind the dwelling one.
    pub fn pending_count(&self) -> usize {
        self.pending.len()
    }

    /// The scrubber cursor, `0` being the newest verified event.
    pub fn scrub_index(&self) -> Option<usize> {
        self.scrub
    }

    /// Step the scrubber back through the timeline. One detent, one event.
    ///
    /// The first detent out of live lands on the **newest** event rather than
    /// stepping past it: someone reaching for the dial is asking to see what
    /// just happened, and making them turn twice to get there would be a
    /// worse answer than an off-by-one is a purer one. Past the oldest event
    /// the cursor stops rather than wrapping — a witness log has a beginning
    /// and pretending otherwise would be a small lie about the record.
    ///
    /// Read-only over a snapshot the ingest built: it cannot alter, export,
    /// or unseal anything, and there is no code path from here that could.
    pub fn scrub_back(&mut self, steps: usize, timeline_len: usize) {
        if timeline_len == 0 {
            self.scrub = None;
            return;
        }
        let next = self.scrub.map_or(0, |i| i.saturating_add(steps));
        self.scrub = Some(next.min(timeline_len - 1));
    }

    /// Step the scrubber toward the present; past the newest event it
    /// returns to live.
    pub fn scrub_forward(&mut self, steps: usize) {
        self.scrub = match self.scrub {
            None => None,
            Some(i) if i >= steps => Some(i - steps),
            Some(_) => None,
        };
    }

    /// Drop the scrubber cursor and return to live.
    pub fn scrub_live(&mut self) {
        self.scrub = None;
    }

    /// Expire the dwell and the quiet window against the clock. Called once
    /// per redraw; separated from the resolvers so they stay pure reads.
    pub fn tick(&mut self, now_ms: u64, timings: &Timings) {
        if let Some((_, until)) = self.dwelling {
            if until <= now_ms {
                if self.pending.is_empty() {
                    self.dwelling = None;
                } else {
                    let next = self.pending.remove(0);
                    self.dwelling =
                        Some((next.zone, now_ms.saturating_add(timings.presence_dwell_ms)));
                }
            }
        }
        if self.quiet_until_ms.is_some_and(|u| u <= now_ms) {
            self.quiet_until_ms = None;
        }
    }
}

/// What the front matrix should show, resolved.
#[derive(Clone, Debug, PartialEq)]
pub struct FrontRender {
    pub phrase: PublicPhrase,
    pub motion: Motion,
    pub color: Rgba,
    /// Set only for a relayed life-safety advisory — the one state that also
    /// lights the bar's notification LED. That extra channel is what makes an
    /// advisory visually distinct from a red degraded state without adding a
    /// flashing alarm the rest of this module exists to avoid.
    pub led: Option<Rgba>,
    pub priority: u8,
}

/// Collect the phrases the classing gate admits from a whole fleet snapshot.
///
/// Every card goes through [`front_phrase`]. Nothing else builds a phrase, so
/// the room-facing matrix and the classing gate cannot drift apart.
fn admitted_phrases(view: &WitnessView, opts: &ClassOptions) -> Vec<PublicPhrase> {
    let mut out = Vec::new();
    for dev in view.devices.iter().take(MAX_TRACKED_DEVICES) {
        for card in &dev.cards {
            if let Ok(p) = front_phrase(card, dev.zone.as_ref(), opts) {
                out.push(p);
            }
        }
    }
    out
}

/// Resolve the front matrix.
///
/// Returns `None` when the matrix should carry nothing at all — Dark, or a
/// quiet window with nothing that overrides it. `None` means *clear the
/// front*, which is different from a blank drawing: nothing of ours is on the
/// glass and nothing of ours needs to expire off it.
///
/// The precedence ladder, highest first:
///
/// 1. **Advisory.** A heard smoke or CO cadence overrides every mode and
///    every window, Dark included. A display that hid a life-safety advisory
///    because someone had set it to Dark would be indefensible.
/// 2. **Degraded.** Tamper, a failed chain, a lost Canary. Also overrides the
///    quiet window and Dark: the lever quiets *cards*, not the honesty about
///    whether the witness system is working.
/// 3. **Unknown.** This surface cannot see witness state. Overrides the quiet
///    window — a surface that went quiet and then went blind must not look
///    calm — but not Dark, where a blank glass is already the honest answer.
/// 4. **Presence**, while its dwell runs, in Live and Timeline modes.
/// 5. **Calm.** The breath.
pub fn resolve_front(
    state: &SurfaceState,
    view: &WitnessView,
    opts: &ClassOptions,
    now_ms: u64,
) -> Option<FrontRender> {
    let phrases = admitted_phrases(view, opts);

    if let Some(w) = phrases.iter().find_map(|p| match p {
        PublicPhrase::Advisory(w) => Some(*w),
        _ => None,
    }) {
        return Some(advisory_render(w));
    }

    if let Some(w) = worst_degraded(&phrases) {
        return Some(degraded_render(w));
    }

    if state.mode() == SurfaceMode::Dark {
        return None;
    }

    if let Some(w) = view.link.unknown() {
        return Some(FrontRender {
            phrase: PublicPhrase::Unknown(w),
            motion: Motion::Still,
            color: COLOR_UNKNOWN,
            led: None,
            priority: PRIORITY_DEGRADED,
        });
    }

    // The on-call indicator survives the quiet window. The lever quiets
    // witness *cards*; a live microphone in the room is not one, and
    // suppressing it would defeat the only reason to show it.
    if view.on_call {
        return Some(FrontRender {
            phrase: PublicPhrase::OnCall,
            motion: Motion::Still,
            color: COLOR_ON_CALL,
            led: None,
            priority: PRIORITY_CALM,
        });
    }

    if state.quiet_remaining_ms(now_ms).is_some() {
        return None;
    }

    if state.mode() == SurfaceMode::Timeline {
        if let Some(entry) = scrubbed(state, view) {
            return Some(FrontRender {
                phrase: entry.phrase.clone(),
                motion: Motion::Still,
                color: COLOR_CALM,
                led: None,
                priority: PRIORITY_CALM,
            });
        }
    }

    if state.mode() != SurfaceMode::ChainHealth {
        if let Some((zone, until)) = &state.dwelling {
            if *until > now_ms {
                return Some(FrontRender {
                    phrase: PublicPhrase::Presence { zone: zone.clone() },
                    motion: Motion::Breathing,
                    color: COLOR_CALM,
                    led: None,
                    priority: PRIORITY_CALM,
                });
            }
        }
    }

    Some(FrontRender {
        phrase: PublicPhrase::Calm,
        motion: Motion::Breathing,
        color: COLOR_CALM,
        led: None,
        priority: PRIORITY_CALM,
    })
}

/// The timeline entry under the scrubber cursor, if any.
pub fn scrubbed<'a>(state: &SurfaceState, view: &'a WitnessView) -> Option<&'a TimelineEntry> {
    let i = state.scrub_index()?;
    let len = view.timeline.len();
    if len == 0 {
        return None;
    }
    // The cursor counts back from the newest, and the timeline is oldest
    // first.
    view.timeline.get(len - 1 - i.min(len - 1))
}

fn worst_degraded(phrases: &[PublicPhrase]) -> Option<DegradedWord> {
    let mut worst: Option<DegradedWord> = None;
    for p in phrases {
        if let PublicPhrase::Degraded(w) = p {
            let better = match (&worst, w) {
                (None, _) => true,
                (Some(DegradedWord::Tamper), _) => false,
                (_, DegradedWord::Tamper) => true,
                (Some(DegradedWord::ChainFail), _) => false,
                (_, DegradedWord::ChainFail) => true,
                (Some(DegradedWord::WitnessLost), _) => false,
                (_, DegradedWord::WitnessLost) => true,
                _ => false,
            };
            if better {
                worst = Some(*w);
            }
        }
    }
    worst
}

fn advisory_render(w: AdvisoryWord) -> FrontRender {
    FrontRender {
        phrase: PublicPhrase::Advisory(w),
        motion: Motion::Still,
        color: COLOR_ALERT,
        led: Some(COLOR_ALERT),
        priority: PRIORITY_ADVISORY,
    }
}

fn degraded_render(w: DegradedWord) -> FrontRender {
    FrontRender {
        phrase: PublicPhrase::Degraded(w),
        motion: Motion::Still,
        color: match w {
            DegradedWord::Tamper => COLOR_ALERT,
            _ => COLOR_DEGRADED,
        },
        led: None,
        priority: PRIORITY_DEGRADED,
    }
}

/// One step of the breath, as an alpha multiplier over the state color.
///
/// A triangle wave rather than a sine: on a 16-pixel matrix the difference is
/// invisible and the integer form is exact, so the same step index always
/// produces the same byte and a test can pin the cycle.
pub fn breath_alpha(step: u32, steps: u8) -> u8 {
    const FLOOR: u32 = 70;
    const CEIL: u32 = 255;
    let steps = u32::from(steps.max(2));
    let half = steps / 2;
    let pos = step % steps;
    let up = if pos <= half { pos } else { steps - pos };
    let span = half.max(1);
    let a = FLOOR + (CEIL - FLOOR) * up / span;
    a.min(CEIL) as u8
}

/// The operator-facing rear view.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RearView {
    /// Short printable-ASCII lines, already bounded and sanitized.
    pub lines: Vec<String>,
    /// A quiet-window expiry, as unix seconds, for the device's own
    /// self-ticking countdown element. The device runs this clock, so the
    /// countdown stays correct even if the bridge restarts mid-window.
    pub countdown_to_epoch_s: Option<u64>,
}

/// Reduce a wire-supplied string to something safe to put on the rear.
///
/// The rear is operator-facing, not public, but it still renders text that
/// arrived over MQTT (the retained `meta` name). The device's own text
/// element accepts printable ASCII only, so anything else is dropped rather
/// than sent, and the result is bounded.
pub fn sanitize_rear(raw: &str, max: usize) -> String {
    raw.chars()
        .filter(|c| c.is_ascii_graphic() || *c == ' ')
        .take(max)
        .collect::<String>()
        .trim()
        .to_string()
}

/// Build the rear view.
///
/// This is where the higher-class detail lives: chain status, per-Canary
/// liveness with last-seen, the recent verified count, the current mode, and
/// the transport path — so the trust boundary is readable at arm's length
/// without any of it being readable from the doorway.
pub fn resolve_rear(
    state: &SurfaceState,
    view: &WitnessView,
    transport: TransportPath,
    now_ms: u64,
    now_epoch_s: u64,
) -> RearView {
    let mut lines: Vec<String> = Vec::new();
    let mode = state.mode();

    lines.push(format!("{} via {}", mode.label(), transport.label()));

    if mode == SurfaceMode::Dark {
        // The one line Dark must always carry. Blanking the glass is a
        // display choice; a household must never be able to read it as a
        // mute button for the witness system.
        lines.push("displays off - witnessing continues".to_string());
    }

    match view.link.unknown() {
        Some(w) => lines.push(format!("NO LINK: {}", w.operator_text())),
        None => {
            let head = view.devices.iter().map(|d| d.chain_len).max().unwrap_or(0);
            let badge = view
                .devices
                .iter()
                .map(|d| d.badge)
                .min_by_key(|b| badge_rank(*b))
                .unwrap_or(Badge::Unknown);
            lines.push(format!("chain {} {}", head, badge_word(badge)));
            lines.push(format!(
                "{} verified events in window",
                view.verified_recent
            ));
        }
    }

    if mode == SurfaceMode::Timeline {
        if let Some(entry) = scrubbed(state, view) {
            lines.push(format!(
                "bucket {}s wide, starts {}",
                entry.bucket_size_s, entry.bucket_start_epoch_s
            ));
            lines.push(format!(
                "attested {} / bundle {}",
                entry.attestation.as_deref().unwrap_or("device"),
                badge_word(entry.bundle_badge)
            ));
        } else {
            lines.push("timeline: live".to_string());
        }
    }

    for dev in view.devices.iter().take(MAX_TRACKED_DEVICES) {
        if lines.len() >= MAX_REAR_LINES {
            break;
        }
        let label = dev
            .name
            .as_deref()
            .map(|n| sanitize_rear(n, 16))
            .filter(|n| !n.is_empty())
            .unwrap_or_else(|| sanitize_rear(&dev.device_id, 16));
        let seen = match dev.last_seen_secs_ago {
            Some(s) if s < 120 => format!("{s}s"),
            Some(s) => format!("{}m", s / 60),
            None => "never".to_string(),
        };
        lines.push(format!(
            "{} {} {}",
            label,
            if dev.online { "ok" } else { "LATE" },
            seen
        ));
    }

    if state.pending_count() > 0 && lines.len() < MAX_REAR_LINES {
        lines.push(format!("{} cards waiting", state.pending_count()));
    }

    let countdown_to_epoch_s = state
        .quiet_remaining_ms(now_ms)
        .map(|ms| now_epoch_s.saturating_add(ms / 1000));
    if countdown_to_epoch_s.is_some() && lines.len() < MAX_REAR_LINES {
        lines.push("quiet window".to_string());
    }

    lines.truncate(MAX_REAR_LINES);
    RearView {
        lines,
        countdown_to_epoch_s,
    }
}

fn badge_rank(b: Badge) -> u8 {
    match b {
        Badge::Failed => 0,
        Badge::Unknown => 1,
        Badge::Unsigned => 2,
        Badge::Signed => 3,
        Badge::Verified => 4,
    }
}

fn badge_word(b: Badge) -> &'static str {
    match b {
        Badge::Verified => "verified",
        Badge::Signed => "signed",
        Badge::Unsigned => "unsigned",
        Badge::Failed => "FAILED",
        Badge::Unknown => "unknown",
    }
}

/// Build the drawing for one redraw: the front phrase at this breath step,
/// plus the rear lines.
pub fn compose(
    front: Option<&FrontRender>,
    rear: &RearView,
    step: u32,
    timings: &Timings,
) -> Option<Frame> {
    let front = front?;
    let alpha = match front.motion {
        Motion::Breathing => breath_alpha(step, timings.pulse_steps),
        Motion::Still => 0xFF,
    };
    let color = front.color.with_alpha(alpha);
    let mut elements = Vec::with_capacity(2 + rear.lines.len());

    let text = front.phrase.text().to_string();
    elements.push(Element::Text {
        id: "front",
        text,
        font: Font::Small,
        color,
        display: Display::Front,
        align: Align::Center,
    });

    for (i, line) in rear.lines.iter().enumerate().take(MAX_REAR_LINES) {
        elements.push(Element::Text {
            id: REAR_IDS[i],
            text: line.clone(),
            font: Font::Tiny,
            color: Rgba::rgb(0xFF, 0xFF, 0xFF),
            display: Display::Back,
            align: Align::TopLeft,
        });
    }

    if let Some(to) = rear.countdown_to_epoch_s {
        elements.push(Element::Countdown {
            id: "quiet",
            timestamp: to,
            color: Rgba::rgb(0xFF, 0xFF, 0xFF),
            display: Display::Back,
            align: Align::BottomMid,
        });
    }

    Some(Frame {
        elements,
        led: front.led,
        priority: front.priority,
        timeout_ms: timings.draw_timeout_ms(),
    })
}

/// Stable element ids for the rear lines. Fixed strings so a redraw replaces
/// each line in place rather than stacking new elements on the display.
const REAR_IDS: [&str; MAX_REAR_LINES] = [
    "rear0", "rear1", "rear2", "rear3", "rear4", "rear5", "rear6", "rear7",
];

/// Why a card was refused, for the operator log. Never rendered on the front.
pub fn refusal_line(device_id: &str, card_id: &str, r: Refusal) -> String {
    format!("{device_id}/{card_id}: {}", r.reason())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::surface::busybar::card::{CardValue, PrivacyClass, CARD_SCHEMA_V};

    fn zone(s: &str) -> ZoneWord {
        ZoneWord::try_new(s).expect("valid label")
    }

    fn card(id: &str, privacy: PrivacyClass, value: CardValue) -> Card {
        Card {
            v: CARD_SCHEMA_V,
            id: id.to_string(),
            title: id.to_string(),
            privacy: Some(privacy),
            severity: None,
            absent: false,
            value,
        }
    }

    fn device(id: &str, cards: Vec<Card>) -> DeviceCards {
        DeviceCards {
            device_id: id.to_string(),
            name: None,
            zone: Some(zone("FRONT DOOR")),
            last_seen_secs_ago: Some(4),
            online: true,
            chain_len: 12,
            badge: Badge::Verified,
            cards,
        }
    }

    fn live_view(devices: Vec<DeviceCards>) -> WitnessView {
        WitnessView {
            link: LinkState {
                broker_live: true,
                kernel_live: Some(true),
            },
            devices,
            timeline: Vec::new(),
            verified_recent: 0,
            on_call: false,
        }
    }

    // ---- constraint 5: fail visible, not silent ---------------------------

    /// Losing the broker must not leave a calm state on the glass. Silence is
    /// not safety.
    #[test]
    fn losing_the_broker_shows_unknown_not_calm() {
        let state = SurfaceState::new();
        let mut view = live_view(vec![device("porch", vec![])]);
        let opts = ClassOptions::default();

        let calm = resolve_front(&state, &view, &opts, 0).expect("front");
        assert_eq!(calm.phrase, PublicPhrase::Calm);
        assert_eq!(calm.motion, Motion::Breathing);

        view.link.broker_live = false;
        let lost = resolve_front(&state, &view, &opts, 0).expect("front");
        assert_eq!(lost.phrase, PublicPhrase::Unknown(UnknownWord::BrokerLost));
        assert_eq!(
            lost.motion,
            Motion::Still,
            "unknown must not keep breathing"
        );
        assert_ne!(lost.color, COLOR_CALM);
        assert_ne!(lost.color, COLOR_DEGRADED, "unknown is a third state");
    }

    /// Losing only the kernel is its own unknown, and it is distinct from a
    /// degraded chain: one means the house knows something is wrong, the
    /// other means this glass does not know anything.
    #[test]
    fn losing_the_kernel_is_unknown_and_not_degraded() {
        let state = SurfaceState::new();
        let view = WitnessView {
            link: LinkState {
                broker_live: true,
                kernel_live: Some(false),
            },
            ..live_view(vec![device("porch", vec![])])
        };
        let r = resolve_front(&state, &view, &ClassOptions::default(), 0).expect("front");
        assert_eq!(r.phrase, PublicPhrase::Unknown(UnknownWord::KernelLost));
    }

    /// The dead-man's switch. Every drawing expires on the device sooner than
    /// the surface promises to redraw it, so a bridge that dies takes the
    /// calm pulse off the glass instead of freezing it there.
    #[test]
    fn every_drawing_expires_before_the_next_redraw_is_due() {
        let timings = Timings::default();
        let timeout = timings.draw_timeout_ms();
        assert!(
            timeout > timings.pulse_step_ms,
            "would blink on a late redraw"
        );
        assert!(
            timeout <= super::super::device::MAX_DRAW_TIMEOUT_MS,
            "a drawing must not outlive the protocol ceiling"
        );

        let state = SurfaceState::new();
        let view = live_view(vec![device("porch", vec![])]);
        let front = resolve_front(&state, &view, &ClassOptions::default(), 0);
        let rear = resolve_rear(&state, &view, TransportPath::Usb, 0, 1_700_000_000);
        let frame = compose(front.as_ref(), &rear, 0, &timings).expect("frame");
        assert_eq!(frame.timeout_ms, timeout);

        let body = frame.body();
        for element in body["elements"].as_array().expect("elements") {
            let t = element["timeout"].as_u64().expect("every element expires");
            assert!(t > 0 && t <= super::super::device::MAX_DRAW_TIMEOUT_MS);
        }
    }

    // ---- debounce ---------------------------------------------------------

    /// A motion sensor that retriggers must never strobe the matrix.
    #[test]
    fn a_retriggering_sensor_is_swallowed_by_the_anti_strobe_gap() {
        let timings = Timings::default();
        let mut state = SurfaceState::new();
        assert!(state.note_presence("porch", Some(zone("FRONT DOOR")), 0, &timings));
        // Two seconds later, and again, and again.
        for t in [2_000, 4_000, 6_000] {
            assert!(
                !state.note_presence("porch", Some(zone("FRONT DOOR")), t, &timings),
                "a retrigger inside the gap must be swallowed"
            );
        }
        assert_eq!(
            state.pending_count(),
            0,
            "swallowed triggers must not queue"
        );
    }

    /// The swallowed trigger must not extend the dwell either — extending is
    /// how a chattering sensor would hold the matrix forever.
    #[test]
    fn a_swallowed_trigger_does_not_extend_the_dwell() {
        let timings = Timings::default();
        let mut state = SurfaceState::new();
        let view = live_view(vec![device("porch", vec![])]);
        let opts = ClassOptions::default();

        state.note_presence("porch", Some(zone("FRONT DOOR")), 0, &timings);
        state.note_presence("porch", Some(zone("FRONT DOOR")), 5_000, &timings);

        // The dwell started at 0 and is 8 s, so at 9 s it is over regardless
        // of the retrigger at 5 s.
        state.tick(9_000, &timings);
        let r = resolve_front(&state, &view, &opts, 9_000).expect("front");
        assert_eq!(r.phrase, PublicPhrase::Calm);
    }

    /// A separate Canary has its own gap: debouncing is per device, so the
    /// back gate is not silenced by the porch.
    #[test]
    fn the_gap_is_per_device() {
        let timings = Timings::default();
        let mut state = SurfaceState::new();
        assert!(state.note_presence("porch", Some(zone("FRONT DOOR")), 0, &timings));
        assert!(state.note_presence("gate", Some(zone("BACK GATE")), 1_000, &timings));
        assert_eq!(
            state.pending_count(),
            1,
            "the second queues behind the first"
        );
    }

    #[test]
    fn a_presence_card_dwells_then_decays_to_the_breath() {
        let timings = Timings::default();
        let mut state = SurfaceState::new();
        let view = live_view(vec![device("porch", vec![])]);
        let opts = ClassOptions::default();

        state.note_presence("porch", Some(zone("FRONT DOOR")), 0, &timings);
        let held = resolve_front(&state, &view, &opts, 1_000).expect("front");
        assert_eq!(
            held.phrase,
            PublicPhrase::Presence {
                zone: Some(zone("FRONT DOOR"))
            }
        );
        assert_eq!(held.motion, Motion::Breathing, "presence is still healthy");

        state.tick(timings.presence_dwell_ms + 1, &timings);
        let decayed =
            resolve_front(&state, &view, &opts, timings.presence_dwell_ms + 1).expect("front");
        assert_eq!(decayed.phrase, PublicPhrase::Calm);
    }

    // ---- the precedence ladder -------------------------------------------

    /// The lever quiets cards. It does not quiet the honesty about whether
    /// the witness system is working.
    #[test]
    fn the_quiet_window_suppresses_presence_but_never_safety() {
        let timings = Timings::default();
        let opts = ClassOptions::default();
        let mut state = SurfaceState::new();
        state.begin_quiet(0, timings.quiet_window_ms);
        state.note_presence("porch", Some(zone("FRONT DOOR")), 0, &timings);

        let quiet = live_view(vec![device("porch", vec![])]);
        assert!(
            resolve_front(&state, &quiet, &opts, 1_000).is_none(),
            "a presence card must be suppressed by the quiet window"
        );

        let advisory = live_view(vec![device(
            "porch",
            vec![card(
                "smoke_alarm_heard",
                PrivacyClass::P0,
                CardValue::Binary(Some(true)),
            )],
        )]);
        let r = resolve_front(&state, &advisory, &opts, 1_000).expect("advisory overrides quiet");
        assert_eq!(r.phrase, PublicPhrase::Advisory(AdvisoryWord::Smoke));

        let degraded = live_view(vec![device(
            "porch",
            vec![card(
                "tamper",
                PrivacyClass::P0,
                CardValue::Binary(Some(true)),
            )],
        )]);
        let r = resolve_front(&state, &degraded, &opts, 1_000).expect("tamper overrides quiet");
        assert_eq!(r.phrase, PublicPhrase::Degraded(DegradedWord::Tamper));
    }

    /// Dark blanks the glass. It does not hide a life-safety advisory, and
    /// the rear says out loud that witnessing continues.
    #[test]
    fn dark_blanks_the_glass_but_not_an_advisory_and_says_so() {
        let opts = ClassOptions::default();
        let mut state = SurfaceState::new();
        state.set_mode(SurfaceMode::Dark);

        let calm = live_view(vec![device("porch", vec![])]);
        assert!(resolve_front(&state, &calm, &opts, 0).is_none());

        let rear = resolve_rear(&state, &calm, TransportPath::Usb, 0, 1_700_000_000);
        assert!(
            rear.lines
                .iter()
                .any(|l| l.contains("witnessing continues")),
            "Dark must say that witnessing continues: {:?}",
            rear.lines
        );

        let advisory = live_view(vec![device(
            "porch",
            vec![card(
                "co_alarm_heard",
                PrivacyClass::P0,
                CardValue::Binary(Some(true)),
            )],
        )]);
        let r = resolve_front(&state, &advisory, &opts, 0).expect("advisory pierces Dark");
        assert_eq!(r.phrase, PublicPhrase::Advisory(AdvisoryWord::CoAlarm));
    }

    /// An advisory is visually distinct from everything else without a
    /// flashing state: it is the only render that lights the notification LED.
    #[test]
    fn only_an_advisory_lights_the_notification_led() {
        let opts = ClassOptions::default();
        let state = SurfaceState::new();
        let tamper = live_view(vec![device(
            "porch",
            vec![card(
                "tamper",
                PrivacyClass::P0,
                CardValue::Binary(Some(true)),
            )],
        )]);
        assert!(resolve_front(&state, &tamper, &opts, 0)
            .expect("front")
            .led
            .is_none());

        let smoke = live_view(vec![device(
            "porch",
            vec![card(
                "smoke_alarm_heard",
                PrivacyClass::P0,
                CardValue::Binary(Some(true)),
            )],
        )]);
        assert!(resolve_front(&state, &smoke, &opts, 0)
            .expect("front")
            .led
            .is_some());
    }

    /// Stillness is the alert. Nothing that is wrong may keep breathing, and
    /// nothing that is well may stop.
    #[test]
    fn motion_means_well_and_stillness_means_trouble() {
        let opts = ClassOptions::default();
        let state = SurfaceState::new();

        let well = resolve_front(&state, &live_view(vec![device("porch", vec![])]), &opts, 0)
            .expect("front");
        assert_eq!(well.motion, Motion::Breathing);

        for bad in [
            card("tamper", PrivacyClass::P0, CardValue::Binary(Some(true))),
            card(
                "chain",
                PrivacyClass::P0,
                CardValue::Trust {
                    chain: 9,
                    badge: Badge::Failed,
                },
            ),
            card(
                "smoke_alarm_heard",
                PrivacyClass::P0,
                CardValue::Binary(Some(true)),
            ),
        ] {
            let id = bad.id.clone();
            let v = live_view(vec![device("porch", vec![bad])]);
            let r = resolve_front(&state, &v, &opts, 0).expect("front");
            assert_eq!(r.motion, Motion::Still, "{id} kept breathing");
        }
    }

    /// The breath is a real cycle, not a constant, and it never goes fully
    /// dark: a pulse that reaches zero alpha is indistinguishable from a
    /// display that has expired.
    #[test]
    fn the_breath_cycles_and_never_reaches_zero() {
        let steps = 4u8;
        let seen: Vec<u8> = (0..8).map(|i| breath_alpha(i, steps)).collect();
        assert!(seen.iter().all(|&a| a > 0), "the breath must not black out");
        assert!(
            seen.iter().any(|&a| a != seen[0]),
            "the breath must actually move"
        );
        assert_eq!(
            breath_alpha(0, steps),
            breath_alpha(u32::from(steps), steps),
            "one cycle should repeat"
        );
    }

    // ---- the rear ---------------------------------------------------------

    #[test]
    fn the_rear_names_the_transport_so_the_trust_boundary_is_legible() {
        let state = SurfaceState::new();
        let view = live_view(vec![device("porch", vec![])]);
        let rear = resolve_rear(&state, &view, TransportPath::Usb, 0, 1_700_000_000);
        assert!(rear.lines[0].contains("usb"), "{:?}", rear.lines);
        let rear = resolve_rear(&state, &view, TransportPath::Lan, 0, 1_700_000_000);
        assert!(rear.lines[0].contains("lan"), "{:?}", rear.lines);
    }

    /// The cloud path has a name so the boundary is nameable, and the config
    /// loader refuses it so the name can never be printed.
    #[test]
    fn the_cloud_path_is_classified_and_then_refused() {
        assert_eq!(
            TransportPath::classify("https://api.busy.app"),
            TransportPath::Cloud
        );
        assert!(super::super::device::validate_url("https://api.busy.app").is_err());
        assert!(super::super::device::validate_url("http://10.0.4.20").is_ok());
        assert_eq!(
            TransportPath::classify("http://10.0.4.20"),
            TransportPath::Usb
        );
    }

    /// The rear renders a wire-supplied name, so it must be bounded and
    /// printable — the device's text element accepts printable ASCII only.
    #[test]
    fn a_wire_supplied_name_is_bounded_and_printable_on_the_rear() {
        let mut dev = device("porch", vec![]);
        dev.name = Some("porch\u{202e}\u{0007} ".repeat(20));
        let state = SurfaceState::new();
        let view = live_view(vec![dev]);
        let rear = resolve_rear(&state, &view, TransportPath::Usb, 0, 1_700_000_000);
        assert!(rear.lines.len() <= MAX_REAR_LINES);
        for line in &rear.lines {
            assert!(
                line.chars().all(|c| c.is_ascii_graphic() || c == ' '),
                "rear line is not printable ASCII: {line:?}"
            );
        }
    }

    #[test]
    fn the_rear_is_bounded_however_many_canaries_report() {
        let devices: Vec<DeviceCards> = (0..MAX_TRACKED_DEVICES + 20)
            .map(|i| device(&format!("canary{i}"), vec![]))
            .collect();
        let state = SurfaceState::new();
        let view = live_view(devices);
        let rear = resolve_rear(&state, &view, TransportPath::Lan, 0, 1_700_000_000);
        assert!(rear.lines.len() <= MAX_REAR_LINES);
    }

    // ---- the scrubber -----------------------------------------------------

    #[test]
    fn the_scrubber_steps_one_verified_event_per_detent() {
        let entries: Vec<TimelineEntry> = (0..3)
            .map(|i| TimelineEntry {
                phrase: PublicPhrase::Presence {
                    zone: Some(zone("FRONT DOOR")),
                },
                bucket_start_epoch_s: 1_700_000_000 + i * 600,
                bucket_size_s: 600,
                attestation: Some("adapter".to_string()),
                bundle_badge: Badge::Signed,
            })
            .collect();
        let view = WitnessView {
            timeline: entries,
            ..live_view(vec![device("porch", vec![])])
        };
        let mut state = SurfaceState::new();
        state.set_mode(SurfaceMode::Timeline);

        // The first detent leaves live and lands on the NEWEST event, which
        // is what a person reaching for the dial is asking to see.
        state.scrub_back(1, view.timeline.len());
        let newest = scrubbed(&state, &view).expect("entry");
        assert_eq!(newest.bucket_start_epoch_s, 1_700_001_200);

        state.scrub_back(1, view.timeline.len());
        let one_back = scrubbed(&state, &view).expect("entry");
        assert_eq!(one_back.bucket_start_epoch_s, 1_700_000_600);

        state.scrub_back(1, view.timeline.len());
        let two_back = scrubbed(&state, &view).expect("entry");
        assert_eq!(two_back.bucket_start_epoch_s, 1_700_000_000);

        // Past the oldest, the cursor stops rather than wrapping.
        state.scrub_back(10, view.timeline.len());
        assert_eq!(state.scrub_index(), Some(view.timeline.len() - 1));

        // Forward past the newest returns to live.
        state.scrub_forward(50);
        assert_eq!(state.scrub_index(), None);
    }

    /// The scrubber's rear line says what was actually verified. The kernel's
    /// per-event records carry no signature of their own, so the badge is the
    /// bundle's and the wording must not imply otherwise.
    #[test]
    fn the_scrubber_reports_the_bundle_verdict_not_a_per_event_one() {
        let view = WitnessView {
            timeline: vec![TimelineEntry {
                phrase: PublicPhrase::Presence { zone: None },
                bucket_start_epoch_s: 1_700_000_000,
                bucket_size_s: 600,
                attestation: Some("ha-bridged".to_string()),
                bundle_badge: Badge::Signed,
            }],
            ..live_view(vec![device("porch", vec![])])
        };
        let mut state = SurfaceState::new();
        state.set_mode(SurfaceMode::Timeline);
        state.scrub_back(0, view.timeline.len());
        let rear = resolve_rear(&state, &view, TransportPath::Usb, 0, 1_700_000_000);
        let joined = rear.lines.join(" | ");
        assert!(joined.contains("bundle signed"), "{joined}");
        assert!(joined.contains("ha-bridged"), "{joined}");
        assert!(
            !joined.contains("bundle verified"),
            "a signature nobody checked here must not read as verified: {joined}"
        );
    }

    /// The timeline is bucketed, and the scrubber must not un-bucket it.
    #[test]
    fn the_scrubber_shows_buckets_never_a_precise_time() {
        let view = WitnessView {
            timeline: vec![TimelineEntry {
                phrase: PublicPhrase::Presence { zone: None },
                bucket_start_epoch_s: 1_700_000_000,
                bucket_size_s: 600,
                attestation: None,
                bundle_badge: Badge::Signed,
            }],
            ..live_view(vec![device("porch", vec![])])
        };
        let mut state = SurfaceState::new();
        state.set_mode(SurfaceMode::Timeline);
        state.scrub_back(0, view.timeline.len());
        let entry = scrubbed(&state, &view).expect("entry");
        assert!(
            entry.bucket_size_s >= 300,
            "buckets are at least five minutes"
        );
    }
}
