//! Fleet peers — the Canaries this hub has actually heard, carried from the
//! MQTT bridge to the kernel's `GET /api/fleet` through one local file.
//!
//! The kernel never speaks MQTT itself: the broker relationship lives in
//! `event_mqtt_bridge` (a loopback API client with its own LWT and Home
//! Assistant discovery), and `witnessd` is a loopback-only daemon. So the
//! bridge is the process that hears the fleet's `securacv/<device_id>/…`
//! topics, and the kernel is the process that answers the Witness Wall. This
//! module is the seam between the two:
//!
//! * the bridge keeps a [`PeerTable`], fed by [`PeerTable::observe`] with each
//!   publish it receives on [`FLEET_PEER_TOPIC_FILTERS`], and writes it out as
//!   a [`PeerSummaryFile`] (schema [`FLEET_PEERS_SCHEMA`]) with an atomic
//!   rename ([`PeerSummaryFile::write_atomic`]);
//! * the kernel reads that file on every `/api/fleet` request
//!   ([`load_rows`]) and projects it through [`fleet_rows`] — an ALLOWLIST
//!   projection, so nothing the bridge stores (the pinned keys included)
//!   reaches the open surface unless this module names it.
//!
//! **Why a file, not a kernel endpoint.** The event API's request parser is
//! header-only (8 KB, no bodies — `POST /verify` refuses a `Content-Length`
//! on purpose), so a JSON-carrying inbound route would widen a surface that
//! was deliberately narrowed. The two processes already exchange the
//! capability token through a path on the same host (`WITNESS_API_TOKEN_PATH`,
//! which the bridge re-reads as it rotates), so a path in the other direction
//! is the established shape. A file also fails safe: if the bridge dies, the
//! timestamps in its last summary age out and every peer honestly reads
//! `online: false` with no wellbeing words.
//!
//! **What "online" means here** is stricter than on the display's glass, and
//! no stronger than this: a peer is claimed present only when a LIVE (not
//! broker-retained) `chain` publish carried an Ed25519 signature that verified
//! against the key pinned for that device — trust-on-first-use from its first
//! `health` publish, the same pin store the display keeps in NVS — AND that
//! publish advanced the chain length past the last one this bridge verified,
//! within [`FLEET_PEER_RECENT_SECS`], with no LWT `offline` since. Unsigned
//! heartbeats (`status`, an `availability` of `online`) never prove presence:
//! anyone on the broker can publish them. A retained publish proves only that
//! the device once said it, so it can pin a key, set the chain verdict and
//! record its length as seen, but it never advances the signed-presence clock.
//! A Canary that has sealed no record within the window therefore reads
//! `online: false`, which the contract defines as "not claimed present", never
//! as "claimed absent".
//!
//! **The broker is the trust boundary, and the signature does not move it.**
//! The chain canonical binds no nonce or timestamp, so a peer with publish
//! rights on the broker can replay a captured signed publish; the
//! length-must-advance rule means a replay buys at most one window per chain
//! advance this bridge did not itself see — for as long as the bridge
//! remembers the length it verified, which the displaced ledger below
//! bounds by a count of displacements, not forever. The same peer can
//! invent device ids and sign for them with its own key (the pin is TOFU,
//! so such a row is indistinguishable from a real Canary), and can hold a
//! real id in the sticky `degraded` verdict by announcing a second key and
//! signing under it (two publishes, no flood needed). [`FLEET_PEER_MAX`]
//! bounds the table; the clock never expires an id that holds a pin or a
//! verified length (only ids that hold neither age out, and the pass that
//! straddles a clock jump is skipped); none of it is a liveness proof, and
//! `tvos/discovery/DISCOVERY.md` says so in the same words. A liveness
//! challenge would use the firmware's `whoami` canonical, which nothing here
//! drives yet.
//!
//! **Displacement keeps the pin, up to a bound.** At the cap a newcomer
//! displaces an id rather than being refused (see [`FLEET_PEER_MAX`]), so a
//! flood can crowd the roll-call while it lasts but never keeps a new Canary
//! off it afterwards. A displaced id that held a pin or a verified length
//! leaves its pin, verified chain length, verified `state` mark and sticky
//! conflict in a ledger of [`FLEET_PEER_DISPLACED_MAX`] ids, carried in the
//! summary file and never projected; when the id is heard again they are
//! restored before its publish is evaluated, so its next `health` under
//! another key is an announcement (changing no verdict on its own), not a
//! first sight, and a replayed chain at or below the remembered length
//! still proves nothing. The ledger drops its oldest displacement first,
//! so a displaced id's pin is let go only after
//! [`FLEET_PEER_DISPLACED_MAX`] further ids that held a pin or a verified
//! length were displaced after it and not heard since, whoever displaced
//! them. Once every resident holds a pin, each of those costs a broker peer
//! at most one fresh `health` publish — fewer where the fleet itself
//! displaces an id meanwhile (a Canary joining at the cap, or one displaced
//! before it coming back; one displaced after it that comes back takes its
//! own entry with it and saves the peer nothing). Nor need the peer pay
//! twice: publishes it made retained are delivered again by the broker to
//! every new subscription, and the bridge subscribes with a clean start and
//! re-subscribes on each reconnect, so a restart, a dropped link or another
//! client on the broker login taking the bridge's session replays the same
//! churn with no new publish. The same re-subscribe delivers the quiet
//! Canary's own retained copies too: its will (on `availability` or
//! `status`, both subscribed before `health`) brings it back from the
//! ledger before the churn lands, and its retained `health` does the same
//! or pins its real key afresh. So the replay re-opens first sight only
//! once the peer has cleared those copies, one zero-length retained publish
//! each (two where the Canary keeps its `health` retained, as every
//! product under `firmware/projects/` that talks to the broker does; one
//! for the flagship, which keeps only its will), made once and before the
//! flood, since a clear heard while the Canary is displaced is itself a
//! return. That is the real bound, and it is a cost, not a wall: a broker
//! peer that displaces a quiet Canary and then pays those publishes (or
//! has the broker replay them) gets a first sight again, so it can pin its
//! own key for the id (and serve a row under the real id that reads `ok`,
//! online and with words), or replay that Canary's captured `health` and
//! chain as fresh — and by doing it again every window, hold a powered-off
//! Canary "online" from one capture. A Canary that keeps publishing leaves
//! the ledger each time it is heard, and re-enters it at the newest end if
//! it is displaced again.
//! Never displacing an id that holds a pin and a verified length would close
//! that, at the price of letting a flood of proven ids shut new Canaries out
//! until the file is deleted; this table does not make that trade.
//!
//! **The wellbeing words are signed too, or they are not words.** The
//! `state` topic is as broker-writable as the heartbeats, so a peer with
//! publish rights could once put a room's `presence`/`occupants`/`breathing`
//! on a real Canary's row while the real Canary kept signing its chain. A
//! canary-sense now signs those three words under its device key over the
//! `state` canonical (`securacv-canary-sig|v1|state|<id>|<chain_length>|
//! <seq>|<presence>|<occupants>|<breathing>`, built by
//! `firmware/common/identity/device_signature.cpp`), and
//! [`PeerTable::observe`] takes them only from a LIVE `state` publish whose
//! signature verifies under the pinned key AND whose freshness pair — the
//! device's chain length when it spoke, then its per-boot publish counter —
//! is strictly past the last pair this bridge verified for the id
//! ([`PeerRecord::last_verified_state`]). An unsigned `state`, one under
//! another key, a replay, or an older publish leaves the words as they were
//! (so they age out of [`fleet_rows`] on the usual clock); it never sets
//! them, and never clears what the real device said. The same bound as the
//! chain applies: a captured publish buys at most one window per pair the
//! bridge itself missed, for as long as the bridge holds the pair it
//! verified — a displaced id keeps it in the ledger, up to the bound above.
//!
//! The pin is TOFU, not pairing: "chain: ok" means the last chain publish
//! verified against the first key this bridge ever saw for that device id,
//! which is weaker than a key pinned at pairing and is described that way in
//! `tvos/discovery/DISCOVERY.md`. A second key merely ANNOUNCED for a pinned
//! id (an unsigned `health`, which anyone on the broker can publish) is
//! remembered and changes no verdict; a second key that actually SIGNS the
//! id's chain is a sticky conflict ("degraded") that only deleting the summary
//! file clears — never a silent re-pin. Displacement clears neither: the pin
//! and the conflict travel through the displaced ledger, and only the bound
//! above lets them go.
//!
//! The summary file holds the per-room wellbeing words beside the pins, so it
//! is written `0600` (unix), fsynced before and after the rename, and read
//! back only up to [`FLEET_PEERS_MAX_FILE_BYTES`]; [`fleet_rows`] re-cleans
//! every field it serves and caps the row count, so a damaged or hand-edited
//! file cannot widen the wire.

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::path::Path;

use anyhow::{anyhow, Context, Result};
use ed25519_dalek::Signature;
use serde::{Deserialize, Serialize};

/// The summary file's schema tag. A file with any other tag is ignored by the
/// kernel (an older or newer bridge is treated as "no peers", never guessed).
pub const FLEET_PEERS_SCHEMA: &str = "securacv/fleet_peers/v1";

/// How recent a verified signed publish must be for a peer to read
/// `online: true`, in seconds. Taken from the display's own fleet model
/// (`fleet_model.h`, `stale_after_ms = 180000`): after three minutes of
/// silence the glass paints a device stale, so the kernel stops claiming it
/// present at the same moment — with the difference that the kernel's clock
/// only counts SIGNED publishes, where the glass counts any activity.
pub const FLEET_PEER_RECENT_SECS: u64 = 180;

/// Cap on distinct device ids the table keeps. Above the display's 24-pin
/// store with headroom for a hub. At the cap a new id displaces one — first
/// any expirable id unheard for [`FLEET_PEER_FORGET_SECS`], then the least
/// recently heard id that never produced a verified signature
/// ([`PeerRecord::proven`]), then the least recently heard id of all — so a
/// broker flood can crowd the roll-call only while it lasts: a newcomer is
/// never refused, so no flood keeps a new Canary off the roll-call after it
/// ends. Eviction is driven by broker activity (a new id arriving), never by
/// the clock alone. A displaced id that held a pin or a verified length
/// leaves them in the displaced ledger ([`FLEET_PEER_DISPLACED_MAX`]) and
/// gets them back when it is next heard; it is re-pinned from scratch only
/// once that ledger has let it go.
pub const FLEET_PEER_MAX: usize = 64;

/// How many displaced ids the table remembers the trust of: four tables'
/// worth. An id displaced at the cap while it held a pinned key or a
/// verified chain length leaves a [`DisplacedPin`] here — its pin, its
/// verified chain length, its verified `state` mark and its sticky pin
/// conflict, nothing else — and gets all four back, before its publish is
/// evaluated, the next time it is heard. The ledger keeps the most recent
/// displacements: when it is full the OLDEST displacement is dropped (and
/// counted, [`PeerTable::pins_released`]). The bound is therefore a count,
/// not a time: a displaced id's pin is let go only after this many other
/// ids that held a pin or a verified length have been displaced after it
/// and not heard since, whoever displaced them (an id heard again leaves
/// the ledger, and its next displacement puts it back at the newest end).
/// Once every resident holds a pin, each displacement costs a broker peer
/// at most one `health` publish for a fresh id, so re-opening
/// trust-on-first-use for one quiet, displaced id costs that peer at most
/// this many publishes after the flood that displaced it — exactly this
/// many when nothing else on the broker moves, and one fewer for each
/// displacement the fleet makes meanwhile that leaves an entry behind (a
/// Canary joining at the cap, or one displaced before it coming back).
/// Publishes the peer made retained cost it nothing the next time: the
/// broker delivers them again whenever the bridge re-subscribes, once the
/// peer has cleared the quiet Canary's own retained will and `health`,
/// which the same re-subscribe also delivers and which bring it back or
/// re-pin its real key (one or two publishes, made once). The tests pin
/// all of these numbers. The ledger
/// rides in the summary file and is never projected by [`fleet_rows`].
pub const FLEET_PEER_DISPLACED_MAX: usize = 4 * FLEET_PEER_MAX;

/// How long an id that holds NOTHING WORTH KEEPING — no pinned key and no
/// verified chain length; an id heard only on `status`, `availability` or
/// `meta` — stays in the table without being heard before it is forgotten:
/// 30 days, checked on every flush. An id that holds a pin or a verified
/// length is never expired by the clock: it leaves only by displacement at
/// the cap or by the operator deleting the file, so no clock, however wrong,
/// can hand a pinned id back to whoever publishes `health` first. The expiry
/// pass that straddles an implausible clock jump is additionally skipped
/// (see [`FLEET_PEER_CLOCK_JUMP_SECS`]); passes after it run against
/// whatever clock the host then has, and can forget only unpinned ghosts.
pub const FLEET_PEER_FORGET_SECS: u64 = 30 * 24 * 60 * 60;

/// The largest step the table's clock may take between two things it did
/// (an observation, a flush, the file's own write stamp) before expiry is
/// skipped for that call: one day. Nothing the bridge does legitimately goes
/// a day without touching the table while the broker delivers, so a larger
/// step is a corrected RTC, a bad NTP answer or a long outage — in every case
/// a moment to keep what is known, not to forget it.
pub const FLEET_PEER_CLOCK_JUMP_SECS: u64 = 24 * 60 * 60;

/// The most the kernel will read of a summary file. [`FLEET_PEER_MAX`] fully
/// populated records beside a full displaced ledger
/// ([`FLEET_PEER_DISPLACED_MAX`] entries), every field at its widest,
/// pretty-print to well under this (a test builds that file and checks);
/// anything past this is a damaged or foreign file, and is ignored rather
/// than served.
pub const FLEET_PEERS_MAX_FILE_BYTES: u64 = 256 * 1024;

/// The topic filters the bridge subscribes to when peer tracking is on. The
/// fleet roll-call surfaces only: `events`, `sensing`, `counts` and `tamper`
/// are deliberately absent — the bridge is the kernel's egress and must not
/// become a consumer of witness traffic. What it does consume is the coarse
/// roll-call: `state` for the contract's wellbeing WORDS (never the readings
/// beside them, and only under the device's signature), `health` for the
/// device's public key (stored locally, never projected), and `chain`, the
/// signed publish the display verifies too, so presence is proven the same
/// way on both.
///
/// The order is the order the bridge subscribes in on every ConnAck, and it
/// matters: `availability` and `status`, where every Canary keeps its will
/// retained, come before `health`, so on a re-subscribe a displaced Canary's
/// own retained will brings it back from the displaced ledger before a
/// retained churn replayed on `health` can push it out (see
/// [`FLEET_PEER_DISPLACED_MAX`]; a test fails if `health` moves first).
pub const FLEET_PEER_TOPIC_FILTERS: &[&str] = &[
    "securacv/+/availability",
    "securacv/+/status",
    "securacv/+/health",
    "securacv/+/chain",
    "securacv/+/state",
    "securacv/+/meta",
];

/// The signing canonical's prefix and schema version — locked in lockstep
/// with `firmware/common/identity/device_signature.cpp`,
/// `custom_components/securacv/signature.py` and the display's `trust.cpp`.
const SIG_PREFIX: &str = "securacv-canary-sig";
const SCHEMA_V: u32 = 1;

const CHAIN_OK: &str = "ok";
const CHAIN_DEGRADED: &str = "degraded";
const CHAIN_UNKNOWN: &str = "unknown";

/// One peer as the bridge remembers it. Every field here is the bridge's
/// private bookkeeping; only what [`fleet_rows`] copies out is ever served.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PeerRecord {
    /// The `<device_id>` segment of the topic — the firmware's configured id
    /// (its `DEVICE_ID` build constant or the owner's override), the same
    /// string a Canary uses as its own `name` in its self-report.
    pub device_id: String,
    /// The owner-authored name from the retained `meta` topic, if any.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    /// The `device_type` the device announces on `status`/`state`, already
    /// reduced to the product-token alphabet by `clean_product`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub device_type: Option<String>,
    /// Last delivery of any handled topic, retained or live. "Heard at all".
    pub last_seen_epoch_s: u64,
    /// Last LIVE `chain` publish whose signature verified against the pin
    /// AND advanced the chain length past `last_verified_length`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_signed_epoch_s: Option<u64>,
    /// The highest chain `length` whose publish verified against the pin,
    /// retained or live. A publish at or below it is a replay — of the
    /// broker's retained head, or of a capture — and proves nothing new.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_verified_length: Option<u64>,
    /// Last time the broker or the device itself said `offline` (LWT on the
    /// availability topic, or `"status":"offline"`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub offline_epoch_s: Option<u64>,
    /// `"ok"` (the last signed chain publish verified against the pin),
    /// `"degraded"` (the last one did not, or a second key has signed for
    /// this id — see `pin_conflict`) or `"unknown"` (nothing signed has been
    /// checkable yet).
    pub chain: String,
    /// The TOFU-pinned Ed25519 verifying key, lowercase hex. Local only.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pinned_key_hex: Option<String>,
    /// Sticky: a chain publish for this pinned id verified under a DIFFERENT
    /// key that a `health` had announced. An announcement alone (unsigned,
    /// anyone can publish one) never sets this.
    #[serde(default)]
    pub pin_conflict: bool,
    /// A different key announced on `health` while a pin exists, held only
    /// until a chain publish shows which key is really signing. Not
    /// persisted: `health` is retained and arrives again after a restart.
    #[serde(skip)]
    announced_key_hex: Option<String>,
    /// Coarse room words from a LIVE `state` publish that verified under
    /// the pin with a fresh [`StateMark`]: `"clear"`/`"present"`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub presence: Option<String>,
    /// `"0"` / `"1"` / `"2+"`, same source and freshness as `presence`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub occupants: Option<String>,
    /// `breathing_locked` from the same publish.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub breathing: Option<bool>,
    /// When the wellbeing words above were last heard live and verified;
    /// they are served only while this is within [`FLEET_PEER_RECENT_SECS`].
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wellbeing_epoch_s: Option<u64>,
    /// The freshness pair of the last `state` publish that verified against
    /// the pin, retained or live. A publish at or below it is a replay — or
    /// an older capture — and sets no words.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_verified_state: Option<StateMark>,
    /// A `chain` publish that arrived before the pin (retained-delivery order
    /// is arbitrary), held so the pin can evaluate it when it lands. Not
    /// persisted: after a restart the pin is already on disk.
    #[serde(skip)]
    pending_chain: Option<PendingChain>,
}

/// The freshness pair a signed `state` publish carries, in the order it is
/// compared: the device's witness-chain length when it spoke (NVS-persisted
/// on the device, so it survives a reboot) and its per-boot count of state
/// publishes. Lexicographic order, which `Ord` derives from the field order,
/// is the contract: a reboot restarts `seq` but never lowers `chain_length`,
/// and a chain advance carries the pair past anything heard before it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
pub struct StateMark {
    /// The device's chain length at the publish.
    pub chain_length: u64,
    /// The device's per-boot state publish counter.
    pub seq: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct PendingChain {
    length: u64,
    latest_hash_hex: String,
    sig_b64url: String,
    retained: bool,
    heard_epoch_s: u64,
}

impl PeerRecord {
    fn new(device_id: &str, now: u64) -> Self {
        Self {
            device_id: device_id.to_string(),
            name: None,
            device_type: None,
            last_seen_epoch_s: now,
            last_signed_epoch_s: None,
            last_verified_length: None,
            offline_epoch_s: None,
            chain: CHAIN_UNKNOWN.to_string(),
            pinned_key_hex: None,
            pin_conflict: false,
            announced_key_hex: None,
            presence: None,
            occupants: None,
            breathing: None,
            wellbeing_epoch_s: None,
            last_verified_state: None,
            pending_chain: None,
        }
    }

    /// Whether a chain publish has EVER verified against this id's pin,
    /// retained or live. The one definition of "proven" the table uses — for
    /// eviction order and for what the clock may forget — so the two paths
    /// cannot disagree about which ids are worth keeping.
    pub fn proven(&self) -> bool {
        self.last_verified_length.is_some()
    }

    /// The contract's `online`: proven by a live signed publish inside the
    /// window, not contradicted by a later `offline`, never while a second
    /// key is signing for the id (`pin_conflict` — this bridge cannot tell
    /// the re-keyed device from the impostor, so it claims neither), and
    /// never by a timestamp from the future (the bridge and the kernel share
    /// a host clock, so a future stamp is a damaged file, not skew).
    pub fn proven_online(&self, now: u64) -> bool {
        if self.pin_conflict {
            return false;
        }
        match self.last_signed_epoch_s {
            Some(signed) => {
                signed <= now
                    && now - signed <= FLEET_PEER_RECENT_SECS
                    && self.offline_epoch_s.is_none_or(|off| signed > off)
            }
            None => false,
        }
    }

    /// Take the wellbeing words from a `state` publish, or leave them: only
    /// a signature that verifies under the pin over the words as published,
    /// with a [`StateMark`] strictly past the last one verified, sets
    /// anything. The words themselves are then held to the contract's
    /// vocabulary; a signed word outside it is dropped, not republished.
    /// A `retained` publish advances the mark (so the broker's retained
    /// head cannot be replayed live later) but is history, not a reading.
    /// Under `pin_conflict` nothing is claimed for the id.
    fn evaluate_state(&mut self, publish: &StatePublish<'_>, retained: bool, now: u64) {
        if self.pin_conflict {
            return;
        }
        let Some(pinned) = self.pinned_key_hex.as_deref().and_then(decode_key_hex) else {
            return;
        };
        if !verify_state_signature(&self.device_id, publish, &pinned) {
            return;
        }
        if !self
            .last_verified_state
            .is_none_or(|seen| publish.mark > seen)
        {
            return;
        }
        self.last_verified_state = Some(publish.mark);
        if retained {
            return;
        }
        self.presence = presence_word(publish.presence);
        self.occupants = occupants_word(publish.occupants);
        self.breathing = match publish.breathing {
            "true" => Some(true),
            "false" => Some(false),
            _ => None,
        };
        self.wellbeing_epoch_s = Some(now);
    }

    fn wellbeing_fresh(&self, now: u64) -> bool {
        self.wellbeing_epoch_s
            .is_some_and(|heard| heard <= now && now - heard <= FLEET_PEER_RECENT_SECS)
    }

    fn note_pubkey(&mut self, pubkey_hex: &str) {
        let key = pubkey_hex.trim().to_ascii_lowercase();
        if key.len() != 64 || !key.bytes().all(|b| b.is_ascii_hexdigit()) {
            return;
        }
        match &self.pinned_key_hex {
            None => self.pinned_key_hex = Some(key),
            Some(pinned) if *pinned != key => {
                // A different key for a pinned identity: never re-pin, and
                // never let an UNSIGNED announcement change the verdict on
                // its own — remember it, and let the next chain publish show
                // which key is actually signing.
                self.announced_key_hex = Some(key);
            }
            Some(_) => self.announced_key_hex = None,
        }
        if let Some(pending) = self.pending_chain.take() {
            self.evaluate_chain(
                pending.length,
                &pending.latest_hash_hex,
                Some(&pending.sig_b64url),
                pending.retained,
                pending.heard_epoch_s,
            );
        }
    }

    fn evaluate_chain(
        &mut self,
        length: u64,
        latest_hash_hex: &str,
        sig_b64url: Option<&str>,
        retained: bool,
        now: u64,
    ) {
        if self.pin_conflict {
            self.chain = CHAIN_DEGRADED.to_string();
            return;
        }
        let Some(sig) = sig_b64url.filter(|s| !s.is_empty()) else {
            // The device stopped signing: no claim, not a failure.
            self.chain = CHAIN_UNKNOWN.to_string();
            return;
        };
        let Some(pinned_hex) = self.pinned_key_hex.as_deref() else {
            // Chain before health on a fresh subscribe: hold it for the pin.
            self.pending_chain = Some(PendingChain {
                length,
                latest_hash_hex: latest_hash_hex.to_string(),
                sig_b64url: sig.to_string(),
                retained,
                heard_epoch_s: now,
            });
            return;
        };
        let Some(pinned) = decode_key_hex(pinned_hex) else {
            self.chain = CHAIN_DEGRADED.to_string();
            return;
        };
        if verify_chain_signature(&self.device_id, length, latest_hash_hex, sig, &pinned) {
            self.chain = CHAIN_OK.to_string();
            // The pinned key is still the one signing: a second key that was
            // only announced was noise.
            self.announced_key_hex = None;
            // Presence advances only with the chain. The canonical carries no
            // nonce, so a publish at or below the last verified length is a
            // replay — the broker's retained head delivered again, or a
            // capture from anyone with publish rights — and a device that
            // sealed nothing new has proven nothing new.
            if self.last_verified_length.is_none_or(|seen| length > seen) {
                self.last_verified_length = Some(length);
                if !retained {
                    self.last_signed_epoch_s = Some(now);
                }
            }
            return;
        }
        let signed_by_announced = self
            .announced_key_hex
            .as_deref()
            .and_then(decode_key_hex)
            .is_some_and(|other| {
                verify_chain_signature(&self.device_id, length, latest_hash_hex, sig, &other)
            });
        if signed_by_announced {
            // A second key is really signing this id's chain — a re-keyed
            // device or an impersonation; this bridge cannot tell which, so
            // it stops claiming either. Sticky until the file is deleted.
            self.pin_conflict = true;
        }
        self.chain = CHAIN_DEGRADED.to_string();
    }
}

/// What the table keeps of an id it displaced at the cap: exactly the four
/// facts that make the id's return safe — the pin (so its next `health`
/// under another key is an announcement, not a first sight), the verified
/// chain length and `state` mark (so a capture at or below them is still a
/// replay) and the sticky conflict (so displacement never clears one). The
/// name, product, clocks and words are not kept: a returning id reads
/// `online: false` with no words until a fresh signed publish says
/// otherwise. Bridge bookkeeping only; [`fleet_rows`] never reads it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DisplacedPin {
    /// The displaced id.
    pub device_id: String,
    /// [`PeerRecord::pinned_key_hex`] when it was displaced.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pinned_key_hex: Option<String>,
    /// [`PeerRecord::last_verified_length`] when it was displaced.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_verified_length: Option<u64>,
    /// [`PeerRecord::last_verified_state`] when it was displaced.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_verified_state: Option<StateMark>,
    /// [`PeerRecord::pin_conflict`] when it was displaced.
    #[serde(default)]
    pub pin_conflict: bool,
}

impl DisplacedPin {
    /// The ledger entry for a record being displaced, or `None` when it
    /// holds nothing worth keeping (no pin and no verified length — the same
    /// test [`PeerTable::prune`] uses for what the clock may forget).
    fn of(rec: &PeerRecord) -> Option<Self> {
        (rec.pinned_key_hex.is_some() || rec.proven()).then(|| Self {
            device_id: rec.device_id.clone(),
            pinned_key_hex: rec.pinned_key_hex.clone(),
            last_verified_length: rec.last_verified_length,
            last_verified_state: rec.last_verified_state,
            pin_conflict: rec.pin_conflict,
        })
    }

    fn holds_trust(&self) -> bool {
        self.pinned_key_hex.is_some() || self.last_verified_length.is_some()
    }

    /// The record a returning id is re-admitted with: new in every respect
    /// but the four facts the ledger kept. A conflict reads `degraded` at
    /// once, as it did before the id was displaced.
    fn restore(self, now: u64) -> PeerRecord {
        let mut rec = PeerRecord::new(&self.device_id, now);
        rec.pinned_key_hex = self.pinned_key_hex;
        rec.last_verified_length = self.last_verified_length;
        rec.last_verified_state = self.last_verified_state;
        rec.pin_conflict = self.pin_conflict;
        if rec.pin_conflict {
            rec.chain = CHAIN_DEGRADED.to_string();
        }
        rec
    }
}

/// The file the bridge writes and the kernel reads.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PeerSummaryFile {
    /// Always [`FLEET_PEERS_SCHEMA`].
    pub schema: String,
    /// When the bridge wrote it (Unix seconds).
    pub written_at_epoch_s: u64,
    /// Sorted by `device_id`.
    pub peers: Vec<PeerRecord>,
    /// The displaced ledger, oldest displacement first, at most
    /// [`FLEET_PEER_DISPLACED_MAX`] entries. Absent from the file while it
    /// is empty, so a file from a table that never displaced a pinned id is
    /// byte-for-byte what a bridge without the ledger wrote; a file without
    /// it (an older bridge's) loads as an empty ledger, and a reader without
    /// the field (an older kernel or bridge) ignores it — the schema tag does
    /// not change, so a downgraded bridge reads the file and drops the
    /// ledger on its next write, as if nothing had been displaced. Never
    /// projected by [`fleet_rows`].
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub displaced: Vec<DisplacedPin>,
}

impl PeerSummaryFile {
    /// Parse and schema-check a summary.
    pub fn from_json(bytes: &[u8]) -> Result<Self> {
        let file: Self =
            serde_json::from_slice(bytes).context("fleet peers summary is not JSON")?;
        if file.schema != FLEET_PEERS_SCHEMA {
            return Err(anyhow!(
                "fleet peers summary schema {:?} is not {FLEET_PEERS_SCHEMA:?}",
                file.schema
            ));
        }
        Ok(file)
    }

    /// Read a summary from disk. A missing file is `Ok(None)` — the bridge
    /// has not run (or peer tracking is off), which is not an error. A file
    /// over [`FLEET_PEERS_MAX_FILE_BYTES`] is an error, never read whole: the
    /// kernel serves this per request, so the file's size is the response's.
    pub fn read(path: &Path) -> Result<Option<Self>> {
        use std::io::Read;
        let file = match open_for_read(path) {
            Ok(file) => file,
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            Err(err) => return Err(err).with_context(|| format!("opening {}", path.display())),
        };
        // A FIFO, a device or a directory at the path is a damaged or hostile
        // setup, never a summary: refuse before reading a byte (the open
        // above is non-blocking, so a FIFO with no writer cannot wedge the
        // kernel's accept loop either).
        let meta = file
            .metadata()
            .with_context(|| format!("sizing {}", path.display()))?;
        if !meta.file_type().is_file() {
            return Err(anyhow!(
                "fleet peers summary {} is not a regular file",
                path.display()
            ));
        }
        let declared = meta.len();
        if declared > FLEET_PEERS_MAX_FILE_BYTES {
            return Err(anyhow!(
                "fleet peers summary {} is {declared} bytes, over the \
                 {FLEET_PEERS_MAX_FILE_BYTES}-byte bound",
                path.display()
            ));
        }
        let mut bytes = Vec::with_capacity(declared as usize);
        file.take(FLEET_PEERS_MAX_FILE_BYTES + 1)
            .read_to_end(&mut bytes)
            .with_context(|| format!("reading {}", path.display()))?;
        if bytes.len() as u64 > FLEET_PEERS_MAX_FILE_BYTES {
            return Err(anyhow!(
                "fleet peers summary {} grew past the {FLEET_PEERS_MAX_FILE_BYTES}-byte bound",
                path.display()
            ));
        }
        Self::from_json(&bytes).map(Some)
    }

    /// Write via a sibling temp file and rename, so the kernel never reads a
    /// half-written summary. The temp file is created `0600` (this file
    /// carries per-room wellbeing words and the pins, like every other state
    /// file the kernel owns) and fsynced before the rename; the directory is
    /// fsynced after it, so a power loss cannot leave the name pointing at an
    /// empty file — which the bridge would recover from by re-pinning every
    /// device, the one thing the pin store exists to avoid.
    pub fn write_atomic(&self, path: &Path) -> Result<()> {
        let json = serde_json::to_string_pretty(self)? + "\n";
        let mut tmp = path.as_os_str().to_owned();
        tmp.push(".tmp");
        let tmp = std::path::PathBuf::from(tmp);
        // The temp name is ours: whatever is squatting there (a symlink, a
        // FIFO, a stale file from a crash) is removed rather than followed,
        // written through, or left to fail every later write.
        if let Ok(meta) = std::fs::symlink_metadata(&tmp) {
            if !meta.file_type().is_file() {
                std::fs::remove_file(&tmp)
                    .with_context(|| format!("removing a non-file at {}", tmp.display()))?;
            }
        }
        write_private_file(&tmp, json.as_bytes())
            .with_context(|| format!("writing {}", tmp.display()))?;
        std::fs::rename(&tmp, path)
            .with_context(|| format!("renaming {} to {}", tmp.display(), path.display()))?;
        let dir = match path.parent() {
            Some(dir) if !dir.as_os_str().is_empty() => dir,
            _ => Path::new("."),
        };
        // Best effort: not every filesystem lets a directory be synced, and a
        // summary that is merely less durable is still a correct summary.
        if let Err(err) = std::fs::File::open(dir).and_then(|d| d.sync_all()) {
            log::debug!(
                "fleet peers: directory sync of {} skipped: {err}",
                dir.display()
            );
        }
        Ok(())
    }
}

/// Open `path` for reading without blocking: a FIFO with no writer opens
/// at once (and reads as empty) instead of parking the caller, which on the
/// kernel side is the API's single accept loop. `O_NONBLOCK` is a no-op for
/// a regular file.
#[cfg(unix)]
fn open_for_read(path: &Path) -> std::io::Result<std::fs::File> {
    use std::os::unix::fs::OpenOptionsExt;
    std::fs::OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(path)
}

#[cfg(not(unix))]
fn open_for_read(path: &Path) -> std::io::Result<std::fs::File> {
    std::fs::File::open(path)
}

/// Create-or-truncate `path` with mode `0600`, refuse to follow a symlink in
/// its place, refuse anything that is not a regular file, write, and fsync.
/// `O_NONBLOCK` turns a writer-side open of a reader-less FIFO into an
/// immediate `ENXIO` instead of a hang. `.mode()` applies only when the file
/// is created, so a temp file left by a crash mid-write is re-narrowed
/// explicitly (the pattern `break_glass::cli::write_secret_file` documents).
#[cfg(unix)]
fn write_private_file(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    use std::io::Write;
    use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
        .open(path)?;
    if !file.metadata()?.file_type().is_file() {
        return Err(std::io::Error::other("temp path is not a regular file"));
    }
    file.set_permissions(std::fs::Permissions::from_mode(0o600))?;
    file.write_all(bytes)?;
    file.sync_all()
}

#[cfg(not(unix))]
fn write_private_file(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    use std::io::Write;
    let mut file = std::fs::File::create(path)?;
    if !file.metadata()?.file_type().is_file() {
        return Err(std::io::Error::other("temp path is not a regular file"));
    }
    file.write_all(bytes)?;
    file.sync_all()
}

/// The bridge's in-memory table, keyed by device id.
#[derive(Debug, Default)]
pub struct PeerTable {
    peers: BTreeMap<String, PeerRecord>,
    /// What displaced ids held, oldest displacement first, at most
    /// [`FLEET_PEER_DISPLACED_MAX`]; an id is never both here and in
    /// `peers`. A queue rather than a map because the order of displacement
    /// is the drop order; the lookup is a scan of at most a few hundred ids,
    /// made only when an id that is not resident is heard.
    displaced: VecDeque<DisplacedPin>,
    /// Ids displaced at the cap since this table was built (the bridge logs
    /// the count as it grows).
    evictions: u64,
    /// Ledger entries dropped to keep it at [`FLEET_PEER_DISPLACED_MAX`]
    /// since this table was built: each is an id whose pin is gone, so its
    /// next `health` pins afresh.
    pins_released: u64,
    /// The latest moment this table has evidence of: the summary's write
    /// stamp on rehydrate, then every observation and flush. [`Self::prune`]
    /// compares its `now` against this, not against the peers, to notice a
    /// clock that jumped.
    clock_hint: u64,
}

impl PeerTable {
    /// Rehydrate from a previously written summary so pins survive a bridge
    /// restart ("a reboot must not re-TOFU", as the display's pin store puts
    /// it). Records with an invalid id are dropped, never trusted. Wellbeing
    /// words that carry no verified [`StateMark`] were taken by a bridge
    /// older than the signature requirement, from a `state` nobody signed:
    /// they are forgotten here, so an upgrade never serves what the old
    /// bridge could not vouch for, not even for one window.
    ///
    /// The displaced ledger comes back in its order, so a restart between
    /// two floods neither forgets a displaced pin nor moves it toward the
    /// drop end. A file that is damaged or hand-edited is held to what the
    /// bridge itself writes: entries with an invalid id, entries that hold
    /// neither a pin nor a verified length, and entries for an id that is
    /// also resident (the resident record wins) are dropped; for an id
    /// listed twice the later (newer) displacement is kept; and only the
    /// newest [`FLEET_PEER_DISPLACED_MAX`] survive.
    pub fn from_summary(summary: PeerSummaryFile) -> Self {
        let peers: BTreeMap<String, PeerRecord> = summary
            .peers
            .into_iter()
            .filter(|p| device_id_ok(&p.device_id))
            .map(|mut p| {
                if p.last_verified_state.is_none() {
                    p.presence = None;
                    p.occupants = None;
                    p.breathing = None;
                    p.wellbeing_epoch_s = None;
                }
                (p.device_id.clone(), p)
            })
            .collect();
        let mut seen = BTreeSet::new();
        let mut newest_first: Vec<DisplacedPin> = summary
            .displaced
            .into_iter()
            .rev()
            .filter(|d| {
                device_id_ok(&d.device_id)
                    && d.holds_trust()
                    && !peers.contains_key(&d.device_id)
                    && seen.insert(d.device_id.clone())
            })
            .take(FLEET_PEER_DISPLACED_MAX)
            .collect();
        newest_first.reverse();
        Self {
            peers,
            displaced: newest_first.into(),
            evictions: 0,
            pins_released: 0,
            clock_hint: summary.written_at_epoch_s,
        }
    }

    /// Number of peers tracked.
    pub fn len(&self) -> usize {
        self.peers.len()
    }

    /// True when no peer has been heard.
    pub fn is_empty(&self) -> bool {
        self.peers.is_empty()
    }

    /// Ids displaced at the cap so far.
    pub fn evictions(&self) -> u64 {
        self.evictions
    }

    /// Displaced ids whose pin and verified length the table still holds.
    pub fn displaced_len(&self) -> usize {
        self.displaced.len()
    }

    /// Displaced ids whose pin the ledger has let go so far (the oldest
    /// displacement, dropped to keep [`FLEET_PEER_DISPLACED_MAX`]): each is
    /// re-pinned from scratch on its next `health`.
    pub fn pins_released(&self) -> u64 {
        self.pins_released
    }

    /// Take a returning id's ledger entry, if it has one.
    fn take_displaced(&mut self, device_id: &str) -> Option<DisplacedPin> {
        let at = self
            .displaced
            .iter()
            .position(|d| d.device_id == device_id)?;
        self.displaced.remove(at)
    }

    /// Keep what a displaced record held, at the newest end of the ledger,
    /// and drop the oldest displacement past the bound.
    fn remember_displaced(&mut self, rec: &PeerRecord) {
        let Some(entry) = DisplacedPin::of(rec) else {
            return;
        };
        self.displaced.retain(|d| d.device_id != entry.device_id);
        self.displaced.push_back(entry);
        while self.displaced.len() > FLEET_PEER_DISPLACED_MAX {
            self.displaced.pop_front();
            self.pins_released += 1;
        }
    }

    /// Forget every id that holds neither a pinned key nor a verified chain
    /// length and has not been heard within [`FLEET_PEER_FORGET_SECS`].
    /// Returns how many were dropped. An id with a pin or a verified length
    /// is never expired here, whatever the clock says: the pin is the one
    /// thing the file exists to keep, and the cap's eviction is what bounds
    /// a table of such ids. The pass is skipped — and the table's clock is
    /// NOT advanced — when `now` is more than [`FLEET_PEER_CLOCK_JUMP_SECS`]
    /// past the last moment this table knew of, so a poll tick under a
    /// jumped clock never teaches the table that clock by itself; only a
    /// publish arriving under it does ([`PeerTable::observe`]), and the
    /// passes that follow can forget only unpinned ghosts. A stamp from the
    /// future is a damaged record, not a recent one; it is kept and ages out
    /// like any other once the clock passes it.
    pub fn prune(&mut self, now: u64) -> usize {
        if self.clock_jumped_since(now) {
            return 0;
        }
        self.clock_hint = self.clock_hint.max(now);
        let before = self.peers.len();
        self.peers.retain(|_, p| {
            p.pinned_key_hex.is_some()
                || p.proven()
                || now.saturating_sub(p.last_seen_epoch_s) <= FLEET_PEER_FORGET_SECS
        });
        before - self.peers.len()
    }

    /// Whether the last [`Self::prune`] declined to expire anything because
    /// `now` had jumped — the bridge logs that once, since a fleet that
    /// suddenly reads stale may be a clock, not a fleet.
    pub fn clock_jumped_since(&self, now: u64) -> bool {
        now > self.clock_hint.saturating_add(FLEET_PEER_CLOCK_JUMP_SECS)
    }

    /// The id to displace for a newcomer at the cap: never proven before
    /// proven ([`PeerRecord::proven`], the same word `prune` uses), then the
    /// least recently heard.
    fn eviction_candidate(&self) -> Option<String> {
        self.peers
            .values()
            .min_by_key(|p| (p.proven(), p.last_seen_epoch_s))
            .map(|p| p.device_id.clone())
    }

    /// Feed one received publish. `retained` is the wire flag rumqttc reports
    /// (set by the broker only for a message replayed to a new subscription).
    /// Returns `true` when the table changed and should be flushed; `false`
    /// for topics this module does not handle or malformed ids. A new id at
    /// the cap displaces one (see [`FLEET_PEER_MAX`]) rather than being
    /// refused, so a flood cannot keep new Canaries out once it ends; the
    /// displaced id's pin, verified length, `state` mark and conflict go to
    /// the ledger ([`FLEET_PEER_DISPLACED_MAX`]), and an id the ledger holds
    /// gets them back here, before the publish that brought it back is
    /// evaluated.
    pub fn observe(&mut self, topic: &str, payload: &[u8], retained: bool, now: u64) -> bool {
        let Some((device_id, suffix)) = split_topic(topic) else {
            return false;
        };
        if !device_id_ok(device_id) {
            return false;
        }
        if !matches!(
            suffix,
            "availability" | "status" | "health" | "chain" | "state" | "meta"
        ) {
            return false;
        }
        let returning = if self.peers.contains_key(device_id) {
            None
        } else {
            // Taken before the displacement below is recorded, so the bound
            // can never drop the very id that is coming back.
            let returning = self.take_displaced(device_id);
            if self.peers.len() >= FLEET_PEER_MAX && self.prune(now) == 0 {
                if let Some(victim) = self.eviction_candidate() {
                    if let Some(displaced) = self.peers.remove(&victim) {
                        self.evictions += 1;
                        self.remember_displaced(&displaced);
                    }
                }
            }
            returning
        };
        self.clock_hint = self.clock_hint.max(now);
        let rec = self
            .peers
            .entry(device_id.to_string())
            .or_insert_with(|| match returning {
                Some(kept) => kept.restore(now),
                None => PeerRecord::new(device_id, now),
            });
        rec.last_seen_epoch_s = rec.last_seen_epoch_s.max(now);

        if suffix == "availability" {
            // A bare string, not JSON. `online` is unsigned and proves
            // nothing; `offline` is the broker's own word (LWT) and counts.
            if payload == b"offline" {
                rec.offline_epoch_s = Some(now);
            }
            return true;
        }

        let Ok(doc) = serde_json::from_slice::<serde_json::Value>(payload) else {
            // Malformed JSON still proves the id is active on the wire —
            // which is worth exactly `last_seen`, already stamped.
            return true;
        };
        match suffix {
            "status" => {
                if let Some(dt) = doc.get("device_type").and_then(|v| v.as_str()) {
                    rec.device_type = clean_product(dt);
                }
                // Two LWT shapes: canary-sense/-vision publish
                // `{"status":"offline",…}`; the canary-wap's will is
                // `{"online":false}` on this same topic.
                if doc.get("status").and_then(|v| v.as_str()) == Some("offline")
                    || doc.get("online").and_then(|v| v.as_bool()) == Some(false)
                {
                    rec.offline_epoch_s = Some(now);
                }
            }
            "health" => {
                if let Some(pk) = doc.get("public_key").and_then(|v| v.as_str()) {
                    rec.note_pubkey(pk);
                }
            }
            "chain" => {
                let length = doc.get("length").and_then(|v| v.as_u64()).unwrap_or(0);
                let latest_hash = doc
                    .get("latest_hash")
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                if latest_hash.is_empty() {
                    return true;
                }
                let sig = doc.get("sig").and_then(|v| v.as_str());
                rec.evaluate_chain(length, latest_hash, sig, retained, now);
            }
            "state" => {
                if let Some(dt) = doc.get("device_type").and_then(|v| v.as_str()) {
                    rec.device_type = clean_product(dt);
                }
                // The words are read exactly as the device signed them —
                // the canonical is rebuilt from these strings — and only
                // then held to the contract's vocabulary. An unsigned
                // `state` (older firmware, or anyone on the broker) sets
                // nothing: `state_publish` is `None` without a signature.
                if let Some(publish) = state_publish(&doc) {
                    rec.evaluate_state(&publish, retained, now);
                }
            }
            "meta" => {
                if let Some(name) = doc.get("name").and_then(|v| v.as_str()) {
                    rec.name = clean_name(name);
                }
            }
            _ => {}
        }
        true
    }

    /// Snapshot for the file.
    pub fn summary(&self, now: u64) -> PeerSummaryFile {
        PeerSummaryFile {
            schema: FLEET_PEERS_SCHEMA.to_string(),
            written_at_epoch_s: now,
            peers: self.peers.values().cloned().collect(),
            displaced: self.displaced.iter().cloned().collect(),
        }
    }
}

/// One `/api/fleet` device row: the contract's words (DISCOVERY.md) and
/// nothing else, in the contract's order.
///
/// A struct rather than a `serde_json::Map` on purpose: the kernel's served
/// bytes are pinned byte-for-byte against the shared fleet vector, and a
/// map's key order depends on whether `serde_json/preserve_order` is compiled
/// in — which an optional feature (`c2pa-export` pulls it in) can flip
/// without any code here changing. Struct fields serialize in declaration
/// order under every feature set.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FleetRow {
    pub name: String,
    pub online: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub chain: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub product: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub presence: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub occupants: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub breathing: Option<bool>,
}

/// Project a summary into `/api/fleet` device rows — the contract's words and
/// nothing else. Rows are ordered by device id and capped at
/// [`FLEET_PEER_MAX`]. Fields are copied by name, so a field added to
/// [`PeerRecord`] stays private until it is added here too — and every value
/// is cleaned again on the way out (`clean_name`, `clean_product`, the
/// wellbeing vocabulary), so this is a value allowlist as well as a field
/// one: the file is the bridge's, but the wire is the kernel's.
pub fn fleet_rows(summary: &PeerSummaryFile, now: u64) -> Vec<FleetRow> {
    let mut peers: Vec<&PeerRecord> = summary
        .peers
        .iter()
        .filter(|p| device_id_ok(&p.device_id))
        .collect();
    peers.sort_by(|a, b| a.device_id.cmp(&b.device_id));
    peers
        .into_iter()
        .take(FLEET_PEER_MAX)
        .map(|peer| {
            let online = peer.proven_online(now);
            let name = peer
                .name
                .as_deref()
                .and_then(clean_name)
                .unwrap_or_else(|| peer.device_id.clone());
            let chain = (peer.chain == CHAIN_OK || peer.chain == CHAIN_DEGRADED)
                .then(|| peer.chain.clone());
            let product = peer.device_type.as_deref().and_then(clean_product);
            // Words are served only beside the mark that proves a signed
            // `state` set them: a file written by a bridge older than the
            // signature requirement holds words no signature vouched for.
            let wellbeing =
                online && peer.wellbeing_fresh(now) && peer.last_verified_state.is_some();
            FleetRow {
                name,
                online,
                chain,
                product,
                presence: wellbeing
                    .then(|| peer.presence.as_deref().and_then(presence_word))
                    .flatten(),
                occupants: wellbeing
                    .then(|| peer.occupants.as_deref().and_then(occupants_word))
                    .flatten(),
                breathing: wellbeing.then_some(peer.breathing).flatten(),
            }
        })
        .collect()
}

/// What the kernel calls per request: the projected rows, or an empty list
/// when there is no file, an unreadable one, or one with a foreign schema.
/// Never an error — the open surface must keep answering with the kernel's
/// own row whatever the bridge is doing.
pub fn load_rows(path: &Path, now: u64) -> Vec<FleetRow> {
    match PeerSummaryFile::read(path) {
        Ok(Some(summary)) => fleet_rows(&summary, now),
        Ok(None) => Vec::new(),
        Err(err) => {
            log::debug!("fleet peers summary ignored: {err:#}");
            Vec::new()
        }
    }
}

/// Verify a Canary's `chain` publish against a pinned key: Ed25519 over the
/// locked canonical `securacv-canary-sig|v1|chain|<device_id>|<length>|<hash>`
/// with the signature base64url-encoded without padding — the same bytes
/// `signature.py` and the display's `trust.cpp` rebuild.
pub fn verify_chain_signature(
    device_id: &str,
    length: u64,
    latest_hash_hex: &str,
    sig_b64url: &str,
    pinned: &[u8; 32],
) -> bool {
    let canonical = chain_canonical(device_id, length, latest_hash_hex);
    let Some(sig) = b64url_decode_nopad(sig_b64url) else {
        return false;
    };
    let Ok(sig): Result<[u8; 64], _> = sig.try_into() else {
        return false;
    };
    let Ok(key) = crate::crypto::signatures::verifying_key_strict(pinned) else {
        return false;
    };
    key.verify_strict(canonical.as_bytes(), &Signature::from_bytes(&sig))
        .is_ok()
}

fn chain_canonical(device_id: &str, length: u64, latest_hash_hex: &str) -> String {
    format!("{SIG_PREFIX}|v{SCHEMA_V}|chain|{device_id}|{length}|{latest_hash_hex}")
}

/// A `state` publish's signed part, as the device published it: the three
/// words verbatim (the canonical is rebuilt from them, so they are not
/// cleaned first), the freshness pair, and the signature.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StatePublish<'a> {
    /// The freshness pair (`chain_length`, `seq`).
    pub mark: StateMark,
    /// `presence_state` as published: `unknown`/`clear`/`present`.
    pub presence: &'a str,
    /// `occupants` as published: `0`/`1`/`2+`.
    pub occupants: &'a str,
    /// `breathing_locked` as a canonical word: `true`/`false`, or `unknown`
    /// when the key is absent (a build with no breathing channel).
    pub breathing: &'a str,
    /// The base64url signature, unpadded.
    pub sig_b64url: &'a str,
}

/// The signed part of a parsed `state` document, or `None` when it carries
/// no signature or no freshness pair — an unsigned publish is never a
/// reading, whatever words it holds. A word that could not have been
/// signed (longer than the vocabulary, or carrying the canonical's own
/// separator) is refused here rather than handed to the verifier.
fn state_publish(doc: &serde_json::Value) -> Option<StatePublish<'_>> {
    let sig_b64url = doc.get("sig")?.as_str()?;
    let chain_length = doc.get("chain_length")?.as_u64()?;
    let seq = doc.get("seq")?.as_u64()?;
    let word = |key: &str| -> Option<&str> {
        let w = doc.get(key)?.as_str()?;
        (w.len() <= 16 && w.bytes().all(|b| b.is_ascii_graphic() && b != b'|')).then_some(w)
    };
    let presence = word("presence_state")?;
    let occupants = word("occupants")?;
    let breathing = match doc.get("breathing_locked") {
        None | Some(serde_json::Value::Null) => "unknown",
        Some(serde_json::Value::Bool(true)) => "true",
        Some(serde_json::Value::Bool(false)) => "false",
        Some(_) => return None,
    };
    Some(StatePublish {
        mark: StateMark { chain_length, seq },
        presence,
        occupants,
        breathing,
        sig_b64url,
    })
}

/// Verify a Canary's `state` publish against a pinned key: Ed25519 over the
/// locked canonical `securacv-canary-sig|v1|state|<device_id>|<chain_length>|
/// <seq>|<presence>|<occupants>|<breathing>` — the bytes
/// `device_signature::build_state_canonical` signs on the device and
/// `signature.py` rebuilds in Home Assistant.
pub fn verify_state_signature(
    device_id: &str,
    publish: &StatePublish<'_>,
    pinned: &[u8; 32],
) -> bool {
    let canonical = state_canonical(
        device_id,
        publish.mark,
        publish.presence,
        publish.occupants,
        publish.breathing,
    );
    let Some(sig) = b64url_decode_nopad(publish.sig_b64url) else {
        return false;
    };
    let Ok(sig): Result<[u8; 64], _> = sig.try_into() else {
        return false;
    };
    let Ok(key) = crate::crypto::signatures::verifying_key_strict(pinned) else {
        return false;
    };
    key.verify_strict(canonical.as_bytes(), &Signature::from_bytes(&sig))
        .is_ok()
}

fn state_canonical(
    device_id: &str,
    mark: StateMark,
    presence: &str,
    occupants: &str,
    breathing: &str,
) -> String {
    let StateMark { chain_length, seq } = mark;
    format!(
        "{SIG_PREFIX}|v{SCHEMA_V}|state|{device_id}|{chain_length}|{seq}|{presence}|{occupants}|{breathing}"
    )
}

/// `securacv/<device_id>/<suffix…>` → `(device_id, suffix)`. The fleet-wide
/// `securacv/fleet/…` topics have no device and are skipped.
fn split_topic(topic: &str) -> Option<(&str, &str)> {
    let rest = topic.strip_prefix("securacv/")?;
    let (device_id, suffix) = rest.split_once('/')?;
    if device_id.is_empty() || device_id.eq_ignore_ascii_case("fleet") || suffix.is_empty() {
        return None;
    }
    Some((device_id, suffix))
}

/// The id alphabet a topic segment must fit before it becomes a table key:
/// at most 48 of `[A-Za-z0-9_.-]`, with at least one letter or digit, so
/// `..`, `.` and `-` are not ids (they are never used as paths — only as
/// map keys and the row-name fallback — but they are not names either).
fn device_id_ok(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 48
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-' | b'.'))
        && id.bytes().any(|b| b.is_ascii_alphanumeric())
}

/// Characters that render as nothing or re-order what follows: zero-width
/// space through right-to-left mark, the bidi embedding/override block, the
/// bidi isolates, the line/paragraph separators and the byte-order mark.
/// `char::is_control` is Cc only and lets all of these through.
fn is_invisible_or_bidi(c: char) -> bool {
    matches!(
        c,
        '\u{200b}'..='\u{200f}'
            | '\u{2028}'..='\u{202e}'
            | '\u{2066}'..='\u{2069}'
            | '\u{feff}'
    )
}

/// The byte bound on a served name. The 48-character bound alone lets an
/// all-emoji name reach 192 bytes.
const NAME_MAX_BYTES: usize = 96;

/// An owner-typed name, kept to visible characters, 48 of them and
/// [`NAME_MAX_BYTES`]. `None` when nothing visible is left.
fn clean_name(raw: &str) -> Option<String> {
    let mut cleaned = String::new();
    for c in raw
        .chars()
        .filter(|c| !c.is_control() && !is_invisible_or_bidi(*c))
        .take(48)
    {
        if cleaned.len() + c.len_utf8() > NAME_MAX_BYTES {
            break;
        }
        cleaned.push(c);
    }
    let cleaned = cleaned.trim().to_string();
    (!cleaned.is_empty()).then_some(cleaned)
}

/// The contract's presence vocabulary; anything else is not a word.
fn presence_word(raw: &str) -> Option<String> {
    matches!(raw, "clear" | "present").then(|| raw.to_string())
}

/// The contract's occupancy vocabulary.
fn occupants_word(raw: &str) -> Option<String> {
    matches!(raw, "0" | "1" | "2+").then(|| raw.to_string())
}

/// A product token: the OTA product alphabet (`canary-wap`, `canary-sense`).
fn clean_product(raw: &str) -> Option<String> {
    let cleaned: String = raw
        .trim()
        .chars()
        .map(|c| c.to_ascii_lowercase())
        .filter(|c| c.is_ascii_alphanumeric() || matches!(c, '-' | '_'))
        .take(32)
        .collect();
    (!cleaned.is_empty()).then_some(cleaned)
}

fn decode_key_hex(hex_str: &str) -> Option<[u8; 32]> {
    hex::decode(hex_str).ok()?.try_into().ok()
}

/// Inverse of the firmware's `b64url_encode_nopad`. Trailing `=` padding is
/// tolerated (the Python verifier re-adds it); anything outside the URL-safe
/// alphabet is a decode failure, never a partial result.
fn b64url_decode_nopad(input: &str) -> Option<Vec<u8>> {
    let input = input.trim_end_matches('=');
    let mut out = Vec::with_capacity(input.len() * 3 / 4);
    let mut acc: u32 = 0;
    let mut bits = 0u32;
    for b in input.bytes() {
        let v = match b {
            b'A'..=b'Z' => b - b'A',
            b'a'..=b'z' => b - b'a' + 26,
            b'0'..=b'9' => b - b'0' + 52,
            b'-' => 62,
            b'_' => 63,
            _ => return None,
        } as u32;
        acc = (acc << 6) | v;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push(((acc >> bits) & 0xFF) as u8);
        }
    }
    // A dangling sextet that cannot form a byte is a malformed length.
    if bits >= 6 {
        return None;
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signer, SigningKey};

    const NOW: u64 = 1_700_000_000;
    const HASH: &str = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    fn b64url_encode_nopad(bytes: &[u8]) -> String {
        const ALPHABET: &[u8; 64] =
            b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        let mut out = String::new();
        for chunk in bytes.chunks(3) {
            let mut acc = 0u32;
            for (i, b) in chunk.iter().enumerate() {
                acc |= (*b as u32) << (16 - 8 * i);
            }
            let n = chunk.len() * 8 / 6 + usize::from(chunk.len() * 8 % 6 != 0);
            for i in 0..n {
                out.push(ALPHABET[((acc >> (18 - 6 * i)) & 0x3F) as usize] as char);
            }
        }
        out
    }

    /// The tests' device key: minted once per process from the OS RNG (a
    /// literal seed reads as a hard-coded key to a scanner, and the tests
    /// only need one stable key, not a particular one).
    fn signer() -> SigningKey {
        static SEED: std::sync::OnceLock<[u8; 32]> = std::sync::OnceLock::new();
        SigningKey::from_bytes(SEED.get_or_init(rand::random::<[u8; 32]>))
    }

    /// A fresh key that is not the signer's: another device, an impostor,
    /// a stranger. Distinct from `signer()` with overwhelming probability.
    fn other_key() -> SigningKey {
        SigningKey::from_bytes(&rand::random::<[u8; 32]>())
    }

    fn pubkey_hex(key: &SigningKey) -> String {
        hex::encode(key.verifying_key().to_bytes())
    }

    fn signed_chain(key: &SigningKey, device_id: &str, length: u64) -> Vec<u8> {
        let sig = key.sign(chain_canonical(device_id, length, HASH).as_bytes());
        serde_json::json!({
            "length": length,
            "latest_hash": HASH,
            "v": 1,
            "alg": "ed25519",
            "sig": b64url_encode_nopad(&sig.to_bytes()),
        })
        .to_string()
        .into_bytes()
    }

    fn health(key: &SigningKey) -> Vec<u8> {
        serde_json::json!({ "public_key": pubkey_hex(key), "battery": 90 })
            .to_string()
            .into_bytes()
    }

    /// A canary-sense `state` body as the firmware publishes it: the
    /// readings beside the words, and the signed envelope over the words
    /// and the freshness pair. `breathing` is `None` for a build with no
    /// breathing channel (the key is absent, the canonical says `unknown`).
    fn signed_state(
        key: &SigningKey,
        device_id: &str,
        mark: (u64, u64),
        presence: &str,
        occupants: &str,
        breathing: Option<bool>,
    ) -> Vec<u8> {
        let mark = StateMark {
            chain_length: mark.0,
            seq: mark.1,
        };
        let word = match breathing {
            Some(true) => "true",
            Some(false) => "false",
            None => "unknown",
        };
        let sig = key.sign(state_canonical(device_id, mark, presence, occupants, word).as_bytes());
        let mut doc = serde_json::json!({
            "device_id": device_id,
            "device_type": "canary-sense",
            "presence": presence == "present",
            "presence_state": presence,
            "occupants": occupants,
            "range": "near",
            "radar_ok": true,
            "frame_errors": 0,
            "lux": 138.0,
            "last_event": "presence_detected",
            "uptime_s": 4200,
            "ts_ms": 4200000,
            "v": 1,
            "chain_length": mark.chain_length,
            "seq": mark.seq,
            "alg": "ed25519",
            "fp": "0123456789abcdef",
            "sig": b64url_encode_nopad(&sig.to_bytes()),
        });
        if let Some(locked) = breathing {
            doc["breathing_locked"] = serde_json::Value::Bool(locked);
            doc["breath_bpm"] = serde_json::json!(14);
            doc["heart_bpm"] = serde_json::json!(61);
        }
        doc.to_string().into_bytes()
    }

    /// The `state` golden vector, shared byte-for-byte with
    /// `firmware/tests_host/test_device_signature_common.cpp` (STATE_GOLDEN)
    /// and `custom_components/securacv/tests/test_signature.py`.
    const STATE_GOLDEN: &str = "securacv-canary-sig|v1|state|sense01|12|3|present|1|true";
    /// Ed25519 is deterministic: the signature over STATE_GOLDEN under the
    /// pytest's fixed key (seed 0x42) is a golden value too, so a verifier
    /// here, in Python and on a device that accepts it rebuilds the same
    /// bytes.
    const STATE_GOLDEN_PUB: &str =
        "2152f8d19b791d24453242e15f2eab6cb7cffa7b6a5ed30097960e069881db12";
    const STATE_GOLDEN_SIG: &str =
        "McxqbRv5TCU4YWBcDlS1EXcDeiQ7UEGZ88O3zySgQVZaEam6AjKJ-eVuG5fvhhnhC954a7DEOimTfQP8uvb8Ag";

    #[test]
    fn b64url_round_trips_and_rejects_junk() {
        for len in 0..70 {
            let bytes: Vec<u8> = (0..len as u8).map(|i| i.wrapping_mul(37)).collect();
            let enc = b64url_encode_nopad(&bytes);
            assert_eq!(
                b64url_decode_nopad(&enc).as_deref(),
                Some(&bytes[..]),
                "len {len}"
            );
            assert_eq!(
                b64url_decode_nopad(&(enc + "==")).as_deref(),
                Some(&bytes[..])
            );
        }
        assert!(
            b64url_decode_nopad("ab+c").is_none(),
            "standard alphabet is not url-safe"
        );
        assert!(
            b64url_decode_nopad("a").is_none(),
            "a lone sextet is not a byte"
        );
    }

    #[test]
    fn chain_signature_verifies_against_the_pin_and_nothing_else() {
        let key = signer();
        let sig = key.sign(chain_canonical("porch", 42, HASH).as_bytes());
        let sig_b64 = b64url_encode_nopad(&sig.to_bytes());
        let pinned = key.verifying_key().to_bytes();
        assert!(verify_chain_signature("porch", 42, HASH, &sig_b64, &pinned));
        // Any field moved → the canonical moves → the signature fails.
        assert!(!verify_chain_signature(
            "porch", 43, HASH, &sig_b64, &pinned
        ));
        assert!(!verify_chain_signature(
            "studio", 42, HASH, &sig_b64, &pinned
        ));
        assert!(!verify_chain_signature(
            "porch", 42, "00", &sig_b64, &pinned
        ));
        // A different key, a truncated signature, a padded-but-wrong one.
        let other = other_key().verifying_key().to_bytes();
        assert!(!verify_chain_signature("porch", 42, HASH, &sig_b64, &other));
        assert!(!verify_chain_signature(
            "porch",
            42,
            HASH,
            &sig_b64[..40],
            &pinned
        ));
        assert!(!verify_chain_signature("porch", 42, HASH, "", &pinned));
    }

    #[test]
    fn a_live_verified_chain_publish_is_the_only_thing_that_proves_presence() {
        let key = signer();
        let mut table = PeerTable::default();
        // Unsigned heartbeats: heard, never online.
        assert!(table.observe("securacv/porch/availability", b"online", true, NOW));
        assert!(table.observe(
            "securacv/porch/status",
            br#"{"status":"online","device_type":"canary-wap","rssi":-61}"#,
            false,
            NOW + 1
        ));
        let rows = fleet_rows(&table.summary(NOW + 1), NOW + 1);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].name, "porch");
        assert!(!rows[0].online);
        assert_eq!(rows[0].product.as_deref(), Some("canary-wap"));
        assert!(rows[0].chain.is_none(), "no signed publish yet: {rows:?}");

        // Pin, then a RETAINED chain replay: verdict yes, presence no.
        assert!(table.observe("securacv/porch/health", &health(&key), true, NOW + 2));
        assert!(table.observe(
            "securacv/porch/chain",
            &signed_chain(&key, "porch", 5),
            true,
            NOW + 3
        ));
        let rows = fleet_rows(&table.summary(NOW + 3), NOW + 3);
        assert_eq!(rows[0].chain.as_deref(), Some("ok"));
        assert!(!rows[0].online, "a retained publish is history");

        // A LIVE signed publish inside the window: online.
        assert!(table.observe(
            "securacv/porch/chain",
            &signed_chain(&key, "porch", 6),
            false,
            NOW + 10
        ));
        let rows = fleet_rows(&table.summary(NOW + 10), NOW + 10);
        assert!(rows[0].online);
        // …until the window lapses.
        let later = NOW + 10 + FLEET_PEER_RECENT_SECS;
        assert!(fleet_rows(&table.summary(later), later)[0].online);
        assert!(!fleet_rows(&table.summary(later + 1), later + 1)[0].online);
        // …or the broker says offline.
        assert!(table.observe("securacv/porch/availability", b"offline", false, NOW + 20));
        assert!(!fleet_rows(&table.summary(NOW + 21), NOW + 21)[0].online);
        // A newer signed publish overrides an older offline.
        assert!(table.observe(
            "securacv/porch/chain",
            &signed_chain(&key, "porch", 7),
            false,
            NOW + 30
        ));
        assert!(fleet_rows(&table.summary(NOW + 31), NOW + 31)[0].online);
    }

    #[test]
    fn a_second_key_for_a_pinned_id_is_a_sticky_conflict() {
        let key = signer();
        let impostor = other_key();
        let mut table = PeerTable::default();
        table.observe("securacv/porch/health", &health(&key), false, NOW);
        table.observe("securacv/porch/health", &health(&impostor), false, NOW + 1);
        // Even a publish the impostor signs correctly with ITS key fails: the
        // pin is the first key, and the conflict is remembered.
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&impostor, "porch", 9),
            false,
            NOW + 2,
        );
        let rows = fleet_rows(&table.summary(NOW + 2), NOW + 2);
        assert_eq!(rows[0].chain.as_deref(), Some("degraded"));
        assert!(!rows[0].online);
        // The real key signing again does not clear it either.
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&key, "porch", 10),
            false,
            NOW + 3,
        );
        let rows = fleet_rows(&table.summary(NOW + 3), NOW + 3);
        assert_eq!(rows[0].chain.as_deref(), Some("degraded"));
        assert!(!rows[0].online);
    }

    #[test]
    fn a_chain_heard_before_its_pin_is_evaluated_when_the_pin_lands() {
        let key = signer();
        let mut table = PeerTable::default();
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&key, "porch", 1),
            false,
            NOW,
        );
        assert!(fleet_rows(&table.summary(NOW), NOW)[0].chain.is_none());
        table.observe("securacv/porch/health", &health(&key), false, NOW + 1);
        let rows = fleet_rows(&table.summary(NOW + 1), NOW + 1);
        assert_eq!(rows[0].chain.as_deref(), Some("ok"));
        assert!(rows[0].online);
    }

    #[test]
    fn state_canonical_is_the_shared_golden_vector_and_verifies_under_its_key() {
        let mark = StateMark {
            chain_length: 12,
            seq: 3,
        };
        assert_eq!(
            state_canonical("sense01", mark, "present", "1", "true"),
            STATE_GOLDEN
        );
        // The golden vector was signed with this exact key, so it is the one
        // literal the tests keep: a test fixture, not a deployed secret.
        // codeql[rust/hard-coded-cryptographic-value]
        let key = SigningKey::from_bytes(&[0x42u8; 32]);
        let pinned = key.verifying_key().to_bytes();
        assert_eq!(hex::encode(pinned), STATE_GOLDEN_PUB);
        assert_eq!(
            b64url_encode_nopad(&key.sign(STATE_GOLDEN.as_bytes()).to_bytes()),
            STATE_GOLDEN_SIG
        );
        let publish = StatePublish {
            mark,
            presence: "present",
            occupants: "1",
            breathing: "true",
            sig_b64url: STATE_GOLDEN_SIG,
        };
        assert!(verify_state_signature("sense01", &publish, &pinned));
        // Any field moved → the canonical moves → the signature fails.
        for (edited, what) in [
            (
                StatePublish {
                    presence: "clear",
                    ..publish.clone()
                },
                "presence",
            ),
            (
                StatePublish {
                    occupants: "2+",
                    ..publish.clone()
                },
                "occupants",
            ),
            (
                StatePublish {
                    breathing: "false",
                    ..publish.clone()
                },
                "breathing",
            ),
            (
                StatePublish {
                    mark: StateMark {
                        chain_length: 12,
                        seq: 4,
                    },
                    ..publish.clone()
                },
                "seq",
            ),
            (
                StatePublish {
                    mark: StateMark {
                        chain_length: 13,
                        seq: 3,
                    },
                    ..publish.clone()
                },
                "chain_length",
            ),
            (
                StatePublish {
                    sig_b64url: &STATE_GOLDEN_SIG[..40],
                    ..publish.clone()
                },
                "a truncated signature",
            ),
        ] {
            assert!(
                !verify_state_signature("sense01", &edited, &pinned),
                "{what} edited after signing still verified"
            );
        }
        assert!(!verify_state_signature("sense02", &publish, &pinned));
        let other = other_key().verifying_key().to_bytes();
        assert!(!verify_state_signature("sense01", &publish, &other));
        // The firmware host test carries the same literal: neither side can
        // move it alone.
        let firmware_test = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/firmware/tests_host/test_device_signature_common.cpp"
        );
        if let Ok(text) = std::fs::read_to_string(firmware_test) {
            assert!(
                text.contains(&format!("\"{STATE_GOLDEN}\"")),
                "the firmware host test no longer pins STATE_GOLDEN"
            );
        }
    }

    #[test]
    fn an_unsigned_or_foreign_state_sets_no_wellbeing_words() {
        let key = signer();
        let impostor = other_key();
        let mut table = PeerTable::default();
        table.observe("securacv/bedroom/health", &health(&key), false, NOW);
        table.observe(
            "securacv/bedroom/chain",
            &signed_chain(&key, "bedroom", 3),
            false,
            NOW + 1,
        );
        assert!(fleet_rows(&table.summary(NOW + 1), NOW + 1)[0].online);
        // The pre-signature body: words, no envelope. Anyone on the broker
        // can publish it under a real id, so it is not a reading.
        let unsigned = br#"{"device_type":"canary-sense","presence_state":"present","occupants":"2+","breathing_locked":true,"range":"near","lux":138.0}"#;
        table.observe("securacv/bedroom/state", unsigned, false, NOW + 2);
        let rows = fleet_rows(&table.summary(NOW + 2), NOW + 2);
        assert!(rows[0].online);
        assert!(
            rows[0].presence.is_none()
                && rows[0].occupants.is_none()
                && rows[0].breathing.is_none(),
            "an unsigned state was served: {rows:?}"
        );
        // Signed, but by a key that is not the pin.
        table.observe(
            "securacv/bedroom/state",
            &signed_state(&impostor, "bedroom", (3, 1), "present", "2+", Some(true)),
            false,
            NOW + 3,
        );
        let rows = fleet_rows(&table.summary(NOW + 3), NOW + 3);
        assert!(
            rows[0].presence.is_none(),
            "a foreign signature was served: {rows:?}"
        );
        assert!(table.summary(NOW + 3).peers[0]
            .last_verified_state
            .is_none());
        // A signature over different words than the body carries (the
        // words edited on the broker after signing).
        let mut edited: serde_json::Value = serde_json::from_slice(&signed_state(
            &key,
            "bedroom",
            (3, 1),
            "clear",
            "0",
            Some(false),
        ))
        .expect("json");
        edited["presence_state"] = "present".into();
        edited["occupants"] = "2+".into();
        table.observe(
            "securacv/bedroom/state",
            edited.to_string().as_bytes(),
            false,
            NOW + 4,
        );
        assert!(fleet_rows(&table.summary(NOW + 4), NOW + 4)[0]
            .presence
            .is_none());
        // The real device's words land; an unsigned publish after them
        // neither changes nor clears them.
        table.observe(
            "securacv/bedroom/state",
            &signed_state(&key, "bedroom", (3, 1), "clear", "0", Some(false)),
            false,
            NOW + 5,
        );
        let rows = fleet_rows(&table.summary(NOW + 5), NOW + 5);
        assert_eq!(rows[0].presence.as_deref(), Some("clear"));
        assert_eq!(rows[0].occupants.as_deref(), Some("0"));
        assert_eq!(rows[0].breathing, Some(false));
        table.observe("securacv/bedroom/state", unsigned, false, NOW + 6);
        let rows = fleet_rows(&table.summary(NOW + 6), NOW + 6);
        assert_eq!(rows[0].presence.as_deref(), Some("clear"));
        assert_eq!(rows[0].occupants.as_deref(), Some("0"));
        // A state signed by the pinned key for an id in sticky conflict
        // claims nothing either.
        table.observe(
            "securacv/bedroom/health",
            &health(&impostor),
            false,
            NOW + 7,
        );
        table.observe(
            "securacv/bedroom/chain",
            &signed_chain(&impostor, "bedroom", 4),
            false,
            NOW + 8,
        );
        assert!(table.summary(NOW + 8).peers[0].pin_conflict);
        table.observe(
            "securacv/bedroom/state",
            &signed_state(&key, "bedroom", (4, 1), "present", "1", Some(true)),
            false,
            NOW + 9,
        );
        let rec = &table.summary(NOW + 9).peers[0];
        assert_eq!(
            rec.last_verified_state,
            Some(StateMark {
                chain_length: 3,
                seq: 1
            })
        );
        assert_eq!(
            rec.presence.as_deref(),
            Some("clear"),
            "no claim under a conflict"
        );
    }

    #[test]
    fn a_replayed_or_older_signed_state_sets_nothing_and_a_reboot_is_not_a_replay() {
        let key = signer();
        let mut table = PeerTable::default();
        table.observe("securacv/bedroom/health", &health(&key), true, NOW);
        // The broker's retained snapshot: it moves the mark (so the same
        // bytes cannot be replayed live later) but is history, not a reading.
        let retained = signed_state(&key, "bedroom", (5, 7), "present", "1", Some(true));
        table.observe("securacv/bedroom/state", &retained, true, NOW + 1);
        let rec = &table.summary(NOW + 1).peers[0];
        assert_eq!(
            rec.last_verified_state,
            Some(StateMark {
                chain_length: 5,
                seq: 7
            })
        );
        assert!(rec.presence.is_none() && rec.wellbeing_epoch_s.is_none());
        table.observe(
            "securacv/bedroom/chain",
            &signed_chain(&key, "bedroom", 5),
            false,
            NOW + 2,
        );
        assert!(fleet_rows(&table.summary(NOW + 2), NOW + 2)[0].online);
        // The retained bytes arriving LIVE — a capture from anyone on the
        // broker — set nothing.
        table.observe("securacv/bedroom/state", &retained, false, NOW + 3);
        assert!(fleet_rows(&table.summary(NOW + 3), NOW + 3)[0]
            .presence
            .is_none());
        // Nor does an older publish (same chain length, lower seq).
        table.observe(
            "securacv/bedroom/state",
            &signed_state(&key, "bedroom", (5, 6), "present", "2+", Some(true)),
            false,
            NOW + 4,
        );
        assert!(fleet_rows(&table.summary(NOW + 4), NOW + 4)[0]
            .presence
            .is_none());
        // The next genuine publish does.
        table.observe(
            "securacv/bedroom/state",
            &signed_state(&key, "bedroom", (5, 8), "present", "1", Some(true)),
            false,
            NOW + 5,
        );
        let rows = fleet_rows(&table.summary(NOW + 5), NOW + 5);
        assert_eq!(rows[0].presence.as_deref(), Some("present"));
        assert_eq!(rows[0].occupants.as_deref(), Some("1"));
        assert_eq!(rows[0].breathing, Some(true));
        // Replaying THAT a week later holds nothing open: the mark does not
        // advance, so the words it carried age out on the usual clock.
        let week = NOW + 7 * 24 * 3600;
        table.observe(
            "securacv/bedroom/state",
            &signed_state(&key, "bedroom", (5, 8), "present", "1", Some(true)),
            false,
            week,
        );
        table.observe(
            "securacv/bedroom/chain",
            &signed_chain(&key, "bedroom", 6),
            false,
            week,
        );
        let rows = fleet_rows(&table.summary(week), week);
        assert!(rows[0].online);
        assert!(
            rows[0].presence.is_none(),
            "a replay is not a reading: {rows:?}"
        );
        // The device rebooted: its chain length is persisted (6 now), its
        // publish counter restarted at 1. The pair (6, 1) is past (5, 8),
        // so the first words after the reboot land.
        table.observe(
            "securacv/bedroom/state",
            &signed_state(&key, "bedroom", (6, 1), "clear", "0", Some(false)),
            false,
            week + 1,
        );
        let rows = fleet_rows(&table.summary(week + 1), week + 1);
        assert_eq!(rows[0].presence.as_deref(), Some("clear"));
        assert_eq!(rows[0].breathing, Some(false));
        // The mark survives the file, so a bridge restart cannot be fed the
        // pre-reboot capture either.
        let mut rehydrated = PeerTable::from_summary(table.summary(week + 1));
        rehydrated.observe(
            "securacv/bedroom/state",
            &signed_state(&key, "bedroom", (5, 8), "present", "1", Some(true)),
            false,
            week + 2,
        );
        assert_eq!(
            rehydrated.summary(week + 2).peers[0].presence.as_deref(),
            Some("clear")
        );
    }

    #[test]
    fn a_build_without_a_breathing_channel_signs_unknown_and_serves_no_breathing() {
        let key = signer();
        let mut table = PeerTable::default();
        table.observe("securacv/hall/health", &health(&key), false, NOW);
        table.observe(
            "securacv/hall/chain",
            &signed_chain(&key, "hall", 2),
            false,
            NOW + 1,
        );
        table.observe(
            "securacv/hall/state",
            &signed_state(&key, "hall", (2, 1), "present", "2+", None),
            false,
            NOW + 2,
        );
        let rows = fleet_rows(&table.summary(NOW + 2), NOW + 2);
        assert_eq!(rows[0].presence.as_deref(), Some("present"));
        assert_eq!(rows[0].occupants.as_deref(), Some("2+"));
        assert!(rows[0].breathing.is_none());
        // Adding a `breathing_locked` to that body on the broker breaks the
        // signature (the canonical said `unknown`).
        let mut doc: serde_json::Value =
            serde_json::from_slice(&signed_state(&key, "hall", (2, 2), "present", "2+", None))
                .expect("json");
        doc["breathing_locked"] = true.into();
        table.observe(
            "securacv/hall/state",
            doc.to_string().as_bytes(),
            false,
            NOW + 3,
        );
        let rows = fleet_rows(&table.summary(NOW + 3), NOW + 3);
        assert!(rows[0].breathing.is_none());
        assert_eq!(
            table.summary(NOW + 3).peers[0].last_verified_state,
            Some(StateMark {
                chain_length: 2,
                seq: 1
            })
        );
    }

    #[test]
    fn wellbeing_words_ride_only_on_a_proven_row_and_only_while_fresh() {
        let key = signer();
        let mut table = PeerTable::default();
        table.observe("securacv/bedroom/health", &health(&key), false, NOW);
        let state = signed_state(&key, "bedroom", (3, 1), "present", "1", Some(true));
        // Retained: history, so no reading is taken.
        table.observe("securacv/bedroom/state", &state, true, NOW + 1);
        // Live, but the row is not proven online: heard, not served.
        table.observe(
            "securacv/bedroom/state",
            &signed_state(&key, "bedroom", (3, 2), "present", "1", Some(true)),
            false,
            NOW + 2,
        );
        let rows = fleet_rows(&table.summary(NOW + 2), NOW + 2);
        assert!(!rows[0].online);
        assert!(rows[0].presence.is_none(), "{rows:?}");
        // Proven online: the coarse words appear — and nothing finer does.
        table.observe(
            "securacv/bedroom/chain",
            &signed_chain(&key, "bedroom", 3),
            false,
            NOW + 3,
        );
        let rows = fleet_rows(&table.summary(NOW + 3), NOW + 3);
        assert!(rows[0].online);
        assert_eq!(rows[0].presence.as_deref(), Some("present"));
        assert_eq!(rows[0].occupants.as_deref(), Some("1"));
        assert_eq!(rows[0].breathing, Some(true));
        let text = serde_json::to_string(&rows[0]).expect("a row serializes");
        for finer in [
            "range",
            "lux",
            "bpm",
            "rssi",
            "public_key",
            "pinned",
            "fp",
            "sig",
            "seq",
            "chain_length",
        ] {
            assert!(!text.contains(finer), "{finer} leaked: {text}");
        }
        // The reading ages out before the presence proof does? No — both use
        // the same window here; push the reading past it with a later proof.
        let later = NOW + 3 + FLEET_PEER_RECENT_SECS - 1;
        table.observe(
            "securacv/bedroom/chain",
            &signed_chain(&key, "bedroom", 4),
            false,
            later,
        );
        let stale = NOW + 2 + FLEET_PEER_RECENT_SECS + 1;
        let rows = fleet_rows(&table.summary(stale), stale);
        assert!(rows[0].online);
        assert!(rows[0].presence.is_none(), "a stale claim omits: {rows:?}");
    }

    #[test]
    fn unknown_words_are_not_republished() {
        let key = signer();
        let mut table = PeerTable::default();
        table.observe("securacv/k/health", &health(&key), false, NOW);
        // Signed by the real key, so they verify — and still are not words.
        table.observe(
            "securacv/k/state",
            &signed_state(&key, "k", (1, 1), "levitating", "many", Some(true)),
            false,
            NOW,
        );
        let rec = &table.summary(NOW).peers[0];
        assert!(rec.presence.is_none() && rec.occupants.is_none());
        assert_eq!(rec.breathing, Some(true));
        assert_eq!(
            rec.last_verified_state,
            Some(StateMark {
                chain_length: 1,
                seq: 1
            })
        );
        // A `breathing_locked` that is not a boolean is not a signable body.
        let state = br#"{"presence_state":"present","occupants":"1","breathing_locked":"yes","chain_length":1,"seq":2,"sig":"AA"}"#;
        table.observe("securacv/k/state", state, false, NOW + 1);
        assert_eq!(
            table.summary(NOW + 1).peers[0].last_verified_state,
            Some(StateMark {
                chain_length: 1,
                seq: 1
            })
        );
        // A word the canonical could not carry (its own separator) is refused
        // before any verifier sees it; an unknown word that verifies is
        // dropped at projection too.
        let state =
            br#"{"presence_state":"pre|sent","occupants":"1","chain_length":1,"seq":3,"sig":"AA"}"#;
        assert!(state_publish(&serde_json::from_slice(state).unwrap()).is_none());
    }

    #[test]
    fn names_products_and_ids_are_cleaned_and_bounded() {
        let mut table = PeerTable::default();
        assert!(!table.observe("securacv/fleet/escalation", b"{}", false, NOW));
        assert!(!table.observe("securacv/FLEET/escalation", b"{}", false, NOW));
        assert!(!table.observe("securacv/../status", b"{}", false, NOW));
        assert!(!table.observe("securacv/./status", b"{}", false, NOW));
        assert!(!table.observe("securacv/-/status", b"{}", false, NOW));
        assert!(!table.observe("securacv/has space/status", b"{}", false, NOW));
        assert!(!table.observe("witness/chain_problem", b"ON", false, NOW));
        assert!(
            !table.observe("securacv/porch/events", b"{}", false, NOW),
            "events are not consumed"
        );
        assert!(!table.observe("securacv/porch/sensing", b"{}", false, NOW));
        // A control character arrives JSON-escaped (a raw one is not JSON);
        // a bidi override and a zero-width space are not controls, and go
        // the same way.
        assert!(table.observe(
            "securacv/porch/meta",
            br#"{"name":"  Front\u0007 \u202eDoor\u200b ","room":"hall"}"#,
            true,
            NOW
        ));
        table.observe(
            "securacv/porch/status",
            br#"{"device_type":"Canary-WAP <script>"}"#,
            false,
            NOW,
        );
        let rows = fleet_rows(&table.summary(NOW), NOW);
        assert_eq!(rows[0].name, "Front Door");
        assert_eq!(rows[0].product.as_deref(), Some("canary-wapscript"));
        assert!(!table.is_empty() && table.len() == 1);
        // An all-emoji name is bounded by bytes as well as characters.
        let emoji = "🦜".repeat(48);
        table.observe(
            "securacv/porch/meta",
            serde_json::json!({ "name": emoji }).to_string().as_bytes(),
            false,
            NOW,
        );
        let name = &fleet_rows(&table.summary(NOW), NOW)[0].name;
        assert!(name.len() <= NAME_MAX_BYTES, "{} bytes", name.len());
        assert!(name.chars().count() == NAME_MAX_BYTES / 4);
    }

    // ---- Displacement keeps the pin (the displaced ledger) -----------------
    //
    // These first tests drive only what the table had before the ledger
    // (observe, summary, from_summary, fleet_rows, evictions) plus the bound
    // constant, so they run unchanged against a table without it.

    fn record(table: &PeerTable, id: &str, now: u64) -> Option<PeerRecord> {
        table
            .summary(now)
            .peers
            .into_iter()
            .find(|p| p.device_id == id)
    }

    fn row_named(table: &PeerTable, name: &str, now: u64) -> Option<FleetRow> {
        fleet_rows(&table.summary(now), now)
            .into_iter()
            .find(|r| r.name == name)
    }

    /// `count` ids proven under keys of their own (`health`, then a signed
    /// `chain`), heard from `t` on, one a second. Returns the next second.
    fn flood_proven(table: &mut PeerTable, prefix: &str, count: usize, mut t: u64) -> u64 {
        for i in 0..count {
            let key = other_key();
            let id = format!("{prefix}-{i:04}");
            table.observe(&format!("securacv/{id}/health"), &health(&key), false, t);
            table.observe(
                &format!("securacv/{id}/chain"),
                &signed_chain(&key, &id, 1),
                false,
                t,
            );
            t += 1;
        }
        t
    }

    /// porch: pinned under `key`, proven at chain length 5, its words signed
    /// at (5, 1) — all live, at `now`.
    fn proven_porch(table: &mut PeerTable, key: &SigningKey, now: u64) {
        table.observe("securacv/porch/health", &health(key), false, now);
        table.observe(
            "securacv/porch/chain",
            &signed_chain(key, "porch", 5),
            false,
            now,
        );
        table.observe(
            "securacv/porch/state",
            &signed_state(key, "porch", (5, 1), "present", "1", Some(true)),
            false,
            now,
        );
    }

    /// A flood of [`FLEET_PEER_MAX`] proven ids heard after porch: the table
    /// fills, and the first newcomer at the cap displaces the least recently
    /// heard proven id — porch.
    fn displace_porch(table: &mut PeerTable, prefix: &str, t: u64) -> u64 {
        let t = flood_proven(table, prefix, FLEET_PEER_MAX, t);
        assert!(
            record(table, "porch", t).is_none(),
            "the flood displaced porch"
        );
        t
    }

    /// Would `attacker`'s `health` for `id` be a first sight right now? Asked
    /// of a copy rebuilt from the summary — a bridge restart — so the probe
    /// leaves the table alone and proves the answer survives the file.
    fn first_sight_open(table: &PeerTable, id: &str, attacker: &SigningKey, t: u64) -> bool {
        let mut probe = PeerTable::from_summary(table.summary(t));
        probe.observe(
            &format!("securacv/{id}/health"),
            &health(attacker),
            false,
            t,
        );
        record(&probe, id, t).and_then(|r| r.pinned_key_hex) == Some(pubkey_hex(attacker))
    }

    /// Displace fresh ids at the cap until `attacker` would get a first
    /// sight of `id`: one fresh `health` per displacement, plus a signed
    /// `chain` for each when `proven`. Every step must displace exactly one
    /// id. A restart (summary round trip) is taken halfway. Returns
    /// (displacements, publishes, clock).
    fn churn_until_open(
        table: &mut PeerTable,
        id: &str,
        attacker: &SigningKey,
        prefix: &str,
        proven: bool,
        mut t: u64,
    ) -> (usize, usize, u64) {
        let (mut displacements, mut publishes) = (0, 0);
        while !first_sight_open(table, id, attacker, t) {
            assert!(
                displacements <= 2 * FLEET_PEER_DISPLACED_MAX,
                "{id}'s pin outlived {displacements} displacements"
            );
            if displacements == FLEET_PEER_DISPLACED_MAX / 2 {
                *table = PeerTable::from_summary(table.summary(t));
            }
            let before = table.evictions();
            let key = other_key();
            let churn = format!("{prefix}-{displacements:04}");
            table.observe(&format!("securacv/{churn}/health"), &health(&key), false, t);
            publishes += 1;
            assert_eq!(table.evictions(), before + 1, "each step displaces one id");
            if proven {
                table.observe(
                    &format!("securacv/{churn}/chain"),
                    &signed_chain(&key, &churn, 1),
                    false,
                    t,
                );
                publishes += 1;
            }
            displacements += 1;
            t += 1;
        }
        (displacements, publishes, t)
    }

    #[test]
    fn a_flood_never_hands_a_displaced_pin_to_the_next_health() {
        let real = signer();
        let attacker = other_key();
        let mut table = PeerTable::default();
        proven_porch(&mut table, &real, NOW);
        let t = displace_porch(&mut table, "fake", NOW + 10);
        assert_eq!(table.evictions(), 1, "porch was the one displaced");

        // The attacker's health for porch is an announcement, not a first
        // sight: the pin, the verified length and the state mark came back.
        table.observe("securacv/porch/health", &health(&attacker), false, t);
        let rec = record(&table, "porch", t).expect("porch re-admitted");
        assert_eq!(
            rec.pinned_key_hex.as_deref(),
            Some(pubkey_hex(&real).as_str()),
            "the flood handed porch's pin to the next health"
        );
        assert_eq!(rec.last_verified_length, Some(5));
        assert_eq!(
            rec.last_verified_state,
            Some(StateMark {
                chain_length: 5,
                seq: 1
            })
        );
        assert!(!rec.pin_conflict, "an announcement alone sets no conflict");
        // Its signed words and a name: nothing it signed reads under porch.
        table.observe(
            "securacv/porch/state",
            &signed_state(&attacker, "porch", (1, 1), "clear", "0", Some(false)),
            false,
            t + 1,
        );
        table.observe(
            "securacv/porch/meta",
            br#"{"name":"Front porch"}"#,
            false,
            t + 2,
        );
        let row = row_named(&table, "Front porch", t + 2).expect("porch's row");
        assert_eq!(
            row,
            FleetRow {
                name: "Front porch".to_string(),
                online: false,
                chain: None,
                // `device_type` rides unsigned beside the words, like a
                // `meta` name: an announcement anyone can make.
                product: Some("canary-sense".to_string()),
                presence: None,
                occupants: None,
                breathing: None,
            },
            "the attacker's words read under the real id"
        );
        // A replay of porch's own captured words is still a replay: the mark
        // came back with the pin.
        table.observe(
            "securacv/porch/state",
            &signed_state(&real, "porch", (5, 1), "present", "1", Some(true)),
            false,
            t + 3,
        );
        let rec = record(&table, "porch", t + 3).expect("porch");
        assert!(
            rec.presence.is_none() && rec.wellbeing_epoch_s.is_none(),
            "a replayed state set words after displacement: {rec:?}"
        );
        // The real Canary speaks again: its key is the pin, its chain
        // advances, its words are new — ok, online and its own.
        table.observe("securacv/porch/health", &health(&real), false, t + 30);
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&real, "porch", 6),
            false,
            t + 31,
        );
        table.observe(
            "securacv/porch/state",
            &signed_state(&real, "porch", (6, 1), "present", "2+", Some(true)),
            false,
            t + 32,
        );
        let row = row_named(&table, "Front porch", t + 32).expect("porch's row");
        assert!(row.online, "{row:?}");
        assert_eq!(row.chain.as_deref(), Some("ok"));
        assert_eq!(row.presence.as_deref(), Some("present"));
        assert_eq!(row.occupants.as_deref(), Some("2+"));
        assert!(!record(&table, "porch", t + 32).expect("porch").pin_conflict);
    }

    #[test]
    fn eviction_cycles_never_replay_one_capture_into_standing_online() {
        // porch sealed once (length 5) and was powered off for good; a peer
        // on the broker holds a capture of its health and that chain.
        let real = signer();
        let captured_health = health(&real);
        let captured_chain = signed_chain(&real, "porch", 5);
        let mut table = PeerTable::default();
        table.observe("securacv/porch/health", &captured_health, false, NOW);
        table.observe("securacv/porch/chain", &captured_chain, false, NOW);
        let mut t = displace_porch(&mut table, "fake", NOW + 300);
        let mut newcomer = 0u32;
        let mut windows_online = 0;
        for cycle in 0..20 {
            // Every resident heard after porch, then one proven newcomer at
            // the cap: porch is displaced again (about 67 publishes a cycle).
            let residents: Vec<String> = table
                .summary(t)
                .peers
                .iter()
                .map(|p| p.device_id.clone())
                .filter(|id| id != "porch")
                .collect();
            for id in residents {
                table.observe(&format!("securacv/{id}/status"), b"{}", false, t);
            }
            if record(&table, "porch", t).is_some() {
                t += 1;
                let key = other_key();
                let id = format!("new-{newcomer}");
                newcomer += 1;
                table.observe(&format!("securacv/{id}/health"), &health(&key), false, t);
                table.observe(
                    &format!("securacv/{id}/chain"),
                    &signed_chain(&key, &id, 1),
                    false,
                    t,
                );
            }
            assert!(
                record(&table, "porch", t).is_none(),
                "cycle {cycle}: porch was displaced"
            );
            // The SAME capture, replayed into the re-admission.
            table.observe("securacv/porch/health", &captured_health, false, t);
            table.observe("securacv/porch/chain", &captured_chain, false, t);
            let row = row_named(&table, "porch", t + 170).expect("porch's row");
            if row.online {
                windows_online += 1;
            }
            t += 175;
        }
        assert_eq!(
            windows_online, 0,
            "one capture held a powered-off Canary online in {windows_online} of 20 \
             consecutive windows by cycling it through displacement"
        );
        let rec = record(&table, "porch", t).expect("porch");
        assert_eq!(rec.last_verified_length, Some(5));
        assert_eq!(
            rec.pinned_key_hex.as_deref(),
            Some(pubkey_hex(&real).as_str())
        );
    }

    #[test]
    fn a_displaced_pin_is_let_go_only_after_the_stated_number_of_displacements() {
        // The number FLEET_PEER_DISPLACED_MAX's doc states, measured: once
        // the flood has displaced porch and every resident holds a pin, each
        // fresh `health` displaces one id, and it takes exactly that many —
        // across a restart taken halfway — before a `health` under another
        // key pins afresh. Churn ids that are proven (and so displaced with a
        // verified length, not just a pin) push porch out no faster.
        for proven in [false, true] {
            let real = signer();
            let attacker = other_key();
            let mut table = PeerTable::default();
            proven_porch(&mut table, &real, NOW);
            // The cheapest flood that displaces a proven porch from a table
            // that holds nothing else: proven ids heard after it until the
            // table is full of them, then one more `health`.
            let (mut t, mut flood_publishes, mut i) = (NOW + 10, 0, 0);
            loop {
                let key = other_key();
                let id = format!("fake-{i:04}");
                table.observe(&format!("securacv/{id}/health"), &health(&key), false, t);
                flood_publishes += 1;
                if record(&table, "porch", t).is_none() {
                    break;
                }
                table.observe(
                    &format!("securacv/{id}/chain"),
                    &signed_chain(&key, &id, 1),
                    false,
                    t,
                );
                flood_publishes += 1;
                i += 1;
                t += 1;
            }
            assert_eq!(flood_publishes, 2 * (FLEET_PEER_MAX - 1) + 1);
            assert!(
                !first_sight_open(&table, "porch", &attacker, t),
                "proven={proven}: the flood alone re-opened porch's first sight"
            );
            let (displacements, publishes, t) =
                churn_until_open(&mut table, "porch", &attacker, "churn", proven, t);
            assert_eq!(
                displacements, FLEET_PEER_DISPLACED_MAX,
                "proven={proven}: porch's pin was let go after {displacements} further \
                 displacements, not the {FLEET_PEER_DISPLACED_MAX} the docs state"
            );
            assert_eq!(
                publishes,
                FLEET_PEER_DISPLACED_MAX * if proven { 2 } else { 1 }
            );
            // What the bound does not stop, pinned so the docs stay honest:
            // once the pin is let go, porch's old capture is a first sight
            // again, and reads online for a window.
            let mut replay = PeerTable::from_summary(table.summary(t));
            replay.observe("securacv/porch/health", &health(&real), false, t);
            replay.observe(
                "securacv/porch/chain",
                &signed_chain(&real, "porch", 5),
                false,
                t,
            );
            assert!(row_named(&replay, "porch", t + 1).expect("row").online);
        }
    }

    #[test]
    fn a_canary_heard_again_starts_its_count_over() {
        let real = signer();
        let attacker = other_key();
        let mut table = PeerTable::default();
        proven_porch(&mut table, &real, NOW);
        let mut t = displace_porch(&mut table, "fake", NOW + 10);
        // One displacement short of letting porch go...
        for i in 0..FLEET_PEER_DISPLACED_MAX - 1 {
            table.observe(
                &format!("securacv/churn-{i:04}/health"),
                &health(&other_key()),
                false,
                t,
            );
            t += 1;
        }
        assert!(!first_sight_open(&table, "porch", &attacker, t));
        // ...the real Canary is heard: it is resident again, under its pin.
        table.observe("securacv/porch/health", &health(&real), false, t);
        let rec = record(&table, "porch", t).expect("porch is back");
        assert_eq!(
            rec.pinned_key_hex.as_deref(),
            Some(pubkey_hex(&real).as_str())
        );
        // A second flood displaces it again; the count starts from there.
        let t = displace_porch(&mut table, "again", t + 1);
        let (displacements, _, _) =
            churn_until_open(&mut table, "porch", &attacker, "more", false, t);
        assert_eq!(
            displacements, FLEET_PEER_DISPLACED_MAX,
            "porch's return did not move it back to the newest end of the ledger"
        );
    }

    #[test]
    fn the_fleet_moving_during_the_churn_saves_the_peer_only_the_entries_it_adds() {
        // The bound counts ids displaced after porch and not heard since,
        // whoever displaced them, so the fleet's own traffic mixed into the
        // churn is measured here against the docs' account of it: a Canary
        // displaced BEFORE porch coming back displaces one more id (an
        // entry the peer did not pay for); a new Canary joining at the cap
        // does the same; that new Canary, displaced by the churn after
        // porch and then coming back, takes its own entry with it as its
        // return adds one (no change). A restart sits between the flood and
        // the churn. The peer's own `health` publishes come to the stated
        // number less exactly the two entries the fleet added.
        let real = signer();
        let attacker = other_key();
        let early_key = other_key();
        let newbie_key = other_key();
        let mut table = PeerTable::default();
        // early: a real Canary that has announced its key and sealed nothing
        // yet (pinned, never proven), so a flood displaces it before porch.
        table.observe("securacv/early/health", &health(&early_key), false, NOW);
        proven_porch(&mut table, &real, NOW + 1);
        let (mut t, mut i, mut early_gone_first) = (NOW + 10, 0, false);
        loop {
            let key = other_key();
            let id = format!("fake-{i:04}");
            table.observe(&format!("securacv/{id}/health"), &health(&key), false, t);
            if record(&table, "porch", t).is_none() {
                break;
            }
            early_gone_first |= record(&table, "early", t).is_none();
            table.observe(
                &format!("securacv/{id}/chain"),
                &signed_chain(&key, &id, 1),
                false,
                t,
            );
            i += 1;
            t += 1;
        }
        assert!(early_gone_first, "early was displaced before porch");
        let mut table = PeerTable::from_summary(table.summary(t));
        assert!(
            !first_sight_open(&table, "porch", &attacker, t),
            "the flood alone re-opened porch's first sight"
        );
        let displaced_at_porch = table.evictions();
        let (mut step, mut peer_publishes) = (0usize, 0usize);
        while !first_sight_open(&table, "porch", &attacker, t) {
            assert!(
                step <= 2 * FLEET_PEER_DISPLACED_MAX,
                "porch's pin outlived {step} churn steps"
            );
            let before = table.evictions();
            match step {
                40 => {
                    table.observe("securacv/early/health", &health(&early_key), false, t);
                    assert_eq!(
                        record(&table, "early", t).and_then(|r| r.pinned_key_hex),
                        Some(pubkey_hex(&early_key)),
                        "early came back under its own pin"
                    );
                }
                80 | 120 => {
                    table.observe("securacv/newbie/health", &health(&newbie_key), false, t);
                    assert_eq!(
                        record(&table, "newbie", t).and_then(|r| r.pinned_key_hex),
                        Some(pubkey_hex(&newbie_key))
                    );
                }
                _ => {}
            }
            let fleet_moved = table.evictions() - before;
            assert_eq!(
                fleet_moved,
                u64::from(matches!(step, 40 | 80 | 120)),
                "step {step}: each fleet event displaces one id"
            );
            table.observe(
                &format!("securacv/churn-{step:04}/health"),
                &health(&other_key()),
                false,
                t,
            );
            peer_publishes += 1;
            step += 1;
            t += 1;
        }
        assert_eq!(
            peer_publishes,
            FLEET_PEER_DISPLACED_MAX - 2,
            "the peer paid {peer_publishes} publishes; the docs say at most \
             {FLEET_PEER_DISPLACED_MAX}, less one per entry the fleet added (two)"
        );
        // Every displacement after porch is counted once, and the one id
        // that came back from after porch (newbie, at step 120) is the only
        // one the count does not hold.
        assert_eq!(
            table.evictions() - displaced_at_porch,
            (FLEET_PEER_DISPLACED_MAX + 1) as u64
        );
    }

    /// The broker as far as the roll-call sees it: a retained publish
    /// replaces its topic's copy (a zero-length one clears it), every
    /// publish reaches the bridge's standing subscription live (retain flag
    /// clear), and a new subscription gets every retained copy again,
    /// filter by filter in the order the bridge subscribes them
    /// ([`FLEET_PEER_TOPIC_FILTERS`], one SUBSCRIBE each on every ConnAck).
    #[derive(Default)]
    struct RetainedBroker {
        retained: BTreeMap<String, Vec<u8>>,
    }

    impl RetainedBroker {
        fn publish(
            &mut self,
            table: &mut PeerTable,
            topic: &str,
            payload: &[u8],
            retain: bool,
            t: u64,
        ) {
            if retain && payload.is_empty() {
                self.retained.remove(topic);
            } else if retain {
                self.retained.insert(topic.to_string(), payload.to_vec());
            }
            table.observe(topic, payload, false, t);
        }

        /// A clean-start re-subscribe. Within one filter the order is the
        /// broker's own, so `id` is delivered either before or after every
        /// other id there.
        fn resubscribe(&self, table: &mut PeerTable, id: &str, id_first: bool, t: u64) {
            for filter in FLEET_PEER_TOPIC_FILTERS {
                let suffix = filter.rsplit('/').next().unwrap_or_default();
                let mut phase: Vec<(&String, &Vec<u8>)> = self
                    .retained
                    .iter()
                    .filter(|(topic, _)| split_topic(topic).is_some_and(|(_, s)| s == suffix))
                    .collect();
                phase.sort_by_key(|(topic, _)| {
                    split_topic(topic).is_some_and(|(of, _)| of == id) != id_first
                });
                for (topic, payload) in phase {
                    table.observe(topic, payload, true, t);
                }
            }
        }
    }

    #[test]
    fn a_retained_churn_replays_on_resubscribe_once_the_canarys_own_copies_are_cleared() {
        // What the bound does not stop, measured so the docs stay honest. A
        // churn published retained is handed again to every new
        // subscription, and the bridge subscribes with a clean start and
        // re-subscribes on every reconnect, so a restart, a dropped link or
        // another client taking its session replays it with no new publish.
        // The same replay carries the displaced Canary's own retained
        // copies: its will (on `availability` or `status`, both subscribed
        // before `health`) brings it back from the ledger before the churn
        // lands, and its retained `health` either does the same or, after
        // the churn, pins its real key afresh. So the replay re-opens first
        // sight only once the peer has cleared those copies — one
        // zero-length retained publish each, made before the flood (a clear
        // heard while the Canary is displaced is itself a return) — and
        // then whichever order the broker delivers one filter's copies in.
        let sense_will =
            br#"{"device_id":"porch","device_type":"canary-sense","status":"offline","ts_ms":0}"#;
        // A canary-sense keeps its will on `status` and its health, chain
        // and state retained (every product under `firmware/projects/`
        // that talks to the broker keeps its will and health so); the
        // flagship keeps only its will, on `availability`, retained.
        for keeps_health in [true, false] {
            let (will_topic, will): (&str, &[u8]) = if keeps_health {
                ("securacv/porch/status", sense_will)
            } else {
                ("securacv/porch/availability", b"offline")
            };
            for (clear_will, clear_health) in
                [(false, false), (true, false), (false, true), (true, true)]
            {
                if clear_health && !keeps_health {
                    continue; // no retained `health` of porch's to clear
                }
                for porch_first in [true, false] {
                    let case = format!(
                        "keeps_health={keeps_health} clear_will={clear_will} \
                         clear_health={clear_health} porch_first={porch_first}"
                    );
                    let real = signer();
                    let attacker = other_key();
                    let mut broker = RetainedBroker::default();
                    let mut table = PeerTable::default();
                    // porch proven as `proven_porch` does, under its own
                    // retain flags; then it goes quiet and the broker
                    // publishes its will.
                    let porch = [
                        ("securacv/porch/health", health(&real)),
                        ("securacv/porch/chain", signed_chain(&real, "porch", 5)),
                        (
                            "securacv/porch/state",
                            signed_state(&real, "porch", (5, 1), "present", "1", Some(true)),
                        ),
                    ];
                    for (topic, body) in &porch {
                        broker.publish(&mut table, topic, body, keeps_health, NOW);
                    }
                    broker.publish(&mut table, will_topic, will, true, NOW + 1);
                    // The peer pays once: its clears, then one retained
                    // `health` per fresh id, the stated number of them.
                    // Unproven, they displace one another, never porch.
                    let mut clears = 0;
                    for (clear, topic) in [
                        (clear_will, will_topic),
                        (clear_health, "securacv/porch/health"),
                    ] {
                        if clear {
                            broker.publish(&mut table, topic, b"", true, NOW + 2);
                            clears += 1;
                        }
                    }
                    let mut t = NOW + 10;
                    for i in 0..FLEET_PEER_DISPLACED_MAX {
                        let topic = format!("securacv/seed-{i:04}/health");
                        broker.publish(&mut table, &topic, &health(&other_key()), true, t);
                        t += 1;
                    }
                    assert!(record(&table, "porch", t).is_some(), "{case}");
                    let t = displace_porch(&mut table, "fake", t);
                    assert!(
                        !first_sight_open(&table, "porch", &attacker, t),
                        "{case}: the flood alone re-opened porch's first sight"
                    );
                    // The bridge restarts (or its session is taken) and
                    // subscribes again: nothing new is published.
                    let mut table = PeerTable::from_summary(table.summary(t));
                    broker.resubscribe(&mut table, "porch", porch_first, t);
                    let cleared = clear_will && (clear_health || !keeps_health);
                    assert_eq!(
                        first_sight_open(&table, "porch", &attacker, t),
                        cleared,
                        "{case}: the replay re-opened porch's first sight only if \
                         its own retained copies were cleared"
                    );
                    if cleared {
                        assert_eq!(clears, 1 + usize::from(keeps_health), "{case}");
                    } else {
                        let summary = table.summary(t);
                        let held = summary
                            .peers
                            .iter()
                            .find(|p| p.device_id == "porch")
                            .and_then(|p| p.pinned_key_hex.clone())
                            .or_else(|| {
                                summary
                                    .displaced
                                    .iter()
                                    .find(|d| d.device_id == "porch")
                                    .and_then(|d| d.pinned_key_hex.clone())
                            });
                        assert_eq!(held, Some(pubkey_hex(&real)), "{case}");
                    }
                }
            }
        }
    }

    #[test]
    fn a_returning_id_is_never_dropped_by_its_own_return() {
        // porch is the oldest entry of a full ledger when it comes back at
        // the cap: its return displaces one id, and that must not push porch
        // itself out of the ledger on the way in.
        let real = signer();
        let attacker = other_key();
        let mut table = PeerTable::default();
        proven_porch(&mut table, &real, NOW);
        let mut t = displace_porch(&mut table, "fake", NOW + 10);
        for i in 0..FLEET_PEER_DISPLACED_MAX - 1 {
            table.observe(
                &format!("securacv/churn-{i:04}/health"),
                &health(&other_key()),
                false,
                t,
            );
            t += 1;
        }
        let before = table.evictions();
        table.observe("securacv/porch/health", &health(&attacker), false, t);
        assert_eq!(
            table.evictions(),
            before + 1,
            "porch's return displaced one id"
        );
        let rec = record(&table, "porch", t).expect("porch");
        assert_eq!(
            rec.pinned_key_hex.as_deref(),
            Some(pubkey_hex(&real).as_str()),
            "porch's own return let its pin go"
        );
    }

    #[test]
    fn calibration_a_signed_second_key_degrades_with_no_flood_and_displacement_keeps_it() {
        // The documented two-publish sticky `degraded` (a second key
        // announced, then signing) needs no flood, and is not new.
        let real = signer();
        let attacker = other_key();
        let mut table = PeerTable::default();
        table.observe("securacv/porch/health", &health(&real), false, NOW);
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&real, "porch", 5),
            false,
            NOW,
        );
        table.observe("securacv/porch/health", &health(&attacker), false, NOW + 1);
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&attacker, "porch", 1),
            false,
            NOW + 2,
        );
        assert!(
            record(&table, "porch", NOW + 2)
                .expect("porch")
                .pin_conflict
        );
        let row = row_named(&table, "porch", NOW + 2).expect("row");
        assert!(!row.online && row.chain.as_deref() == Some("degraded"));
        // Displacement and a restart do not clear it: the conflict is
        // sticky until the operator deletes the file, as documented.
        let t = displace_porch(&mut table, "fake", NOW + 10);
        let mut table = PeerTable::from_summary(table.summary(t));
        table.observe("securacv/porch/health", &health(&real), false, t + 1);
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&real, "porch", 6),
            false,
            t + 2,
        );
        let rec = record(&table, "porch", t + 2).expect("porch");
        assert!(rec.pin_conflict, "displacement cleared a sticky conflict");
        let row = row_named(&table, "porch", t + 2).expect("row");
        assert!(!row.online, "{row:?}");
        assert_eq!(row.chain.as_deref(), Some("degraded"));
    }

    #[test]
    fn calibration_after_a_benign_flood_the_real_canary_reads_ok_and_a_replay_does_not() {
        let real = signer();
        let mut table = PeerTable::default();
        proven_porch(&mut table, &real, NOW);
        let t = displace_porch(&mut table, "fake", NOW + 10);
        // Nobody races the return: the real Canary's next health and chain.
        let mut back = PeerTable::from_summary(table.summary(t));
        back.observe("securacv/porch/health", &health(&real), false, t + 60);
        back.observe(
            "securacv/porch/chain",
            &signed_chain(&real, "porch", 6),
            false,
            t + 61,
        );
        let row = row_named(&back, "porch", t + 62).expect("row");
        assert!(row.online && row.chain.as_deref() == Some("ok"), "{row:?}");
        // The same moment, but the publish is a replay of the length porch
        // had already proven: verified, and no proof of presence.
        table.observe("securacv/porch/health", &health(&real), false, t + 60);
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&real, "porch", 5),
            false,
            t + 61,
        );
        let row = row_named(&table, "porch", t + 62).expect("row");
        assert!(
            !row.online,
            "a replay after displacement read online: {row:?}"
        );
        assert_eq!(row.chain.as_deref(), Some("ok"));
    }

    // These read the ledger itself.

    #[test]
    fn the_ledger_rides_in_the_file_through_a_restart_and_an_older_file_loads() -> Result<()> {
        let real = signer();
        let studio_key = other_key();
        let attacker = other_key();
        let mut table = PeerTable::default();
        proven_porch(&mut table, &real, NOW);
        // studio holds the documented sticky conflict when it is displaced.
        table.observe(
            "securacv/studio/health",
            &health(&studio_key),
            false,
            NOW + 1,
        );
        table.observe(
            "securacv/studio/chain",
            &signed_chain(&studio_key, "studio", 2),
            false,
            NOW + 1,
        );
        table.observe("securacv/studio/health", &health(&attacker), false, NOW + 1);
        table.observe(
            "securacv/studio/chain",
            &signed_chain(&attacker, "studio", 1),
            false,
            NOW + 1,
        );
        let t = flood_proven(&mut table, "fake", FLEET_PEER_MAX, NOW + 10);
        assert_eq!(table.evictions(), 2);
        let summary = table.summary(t);
        assert_eq!(
            summary.displaced,
            vec![
                DisplacedPin {
                    device_id: "porch".to_string(),
                    pinned_key_hex: Some(pubkey_hex(&real)),
                    last_verified_length: Some(5),
                    last_verified_state: Some(StateMark {
                        chain_length: 5,
                        seq: 1
                    }),
                    pin_conflict: false,
                },
                DisplacedPin {
                    device_id: "studio".to_string(),
                    pinned_key_hex: Some(pubkey_hex(&studio_key)),
                    last_verified_length: Some(2),
                    last_verified_state: None,
                    pin_conflict: true,
                },
            ],
            "oldest displacement first, the four facts and nothing else"
        );
        // Through the file and a restart, in order.
        let dir = tempfile::tempdir()?;
        let path = dir.path().join("fleet_peers.json");
        summary.write_atomic(&path)?;
        let read = PeerSummaryFile::read(&path)?.expect("written");
        assert_eq!(read.displaced, summary.displaced);
        let mut table = PeerTable::from_summary(read);
        assert_eq!(table.displaced_len(), 2);
        // studio comes back on an unsigned status: its conflict reads
        // degraded at once, as it did before, survives the next restart, and
        // its real key signing again does not clear it.
        table.observe("securacv/studio/status", b"{}", false, t + 1);
        let row = row_named(&table, "studio", t + 1).expect("row");
        assert_eq!(row.chain.as_deref(), Some("degraded"));
        assert!(!row.online);
        let mut table = PeerTable::from_summary(table.summary(t + 2));
        table.observe(
            "securacv/studio/chain",
            &signed_chain(&studio_key, "studio", 3),
            false,
            t + 3,
        );
        assert!(
            record(&table, "studio", t + 3)
                .expect("studio")
                .pin_conflict
        );
        let held: Vec<&str> = table
            .displaced
            .iter()
            .map(|d| d.device_id.as_str())
            .collect();
        assert!(held.contains(&"porch"), "{held:?}");
        assert!(
            !held.contains(&"studio"),
            "a returning id is never both resident and held"
        );

        // A file without the field — every file a bridge without the ledger
        // wrote — loads as an empty ledger, its peers untouched.
        let older = serde_json::json!({
            "schema": FLEET_PEERS_SCHEMA,
            "written_at_epoch_s": NOW,
            "peers": [{
                "device_id": "porch",
                "last_seen_epoch_s": NOW,
                "last_verified_length": 5,
                "chain": "ok",
                "pinned_key_hex": pubkey_hex(&real),
                "pin_conflict": false
            }]
        });
        let older = PeerSummaryFile::from_json(older.to_string().as_bytes())?;
        assert!(older.displaced.is_empty());
        let reopened = PeerTable::from_summary(older);
        assert_eq!((reopened.len(), reopened.displaced_len()), (1, 0));
        Ok(())
    }

    /// The bytes the bridge before the ledger wrote for one proven peer
    /// (serde_json's pretty printer over that struct, verbatim): the ledger
    /// is absent while empty, so the writer still produces exactly these.
    const PRE_LEDGER_FILE: &str = r#"{
  "schema": "securacv/fleet_peers/v1",
  "written_at_epoch_s": 1700000010,
  "peers": [
    {
      "device_id": "porch",
      "device_type": "canary-sense",
      "last_seen_epoch_s": 1700000005,
      "last_signed_epoch_s": 1700000005,
      "last_verified_length": 5,
      "chain": "ok",
      "pinned_key_hex": "2152f8d19b791d24453242e15f2eab6cb7cffa7b6a5ed30097960e069881db12",
      "pin_conflict": false,
      "presence": "present",
      "occupants": "1",
      "breathing": true,
      "wellbeing_epoch_s": 1700000005,
      "last_verified_state": {
        "chain_length": 5,
        "seq": 1
      }
    }
  ]
}
"#;

    #[test]
    fn a_table_that_displaced_no_pin_writes_the_pre_ledger_bytes() -> Result<()> {
        let summary = PeerSummaryFile::from_json(PRE_LEDGER_FILE.as_bytes())?;
        let table = PeerTable::from_summary(summary);
        let dir = tempfile::tempdir()?;
        let path = dir.path().join("fleet_peers.json");
        table.summary(NOW + 10).write_atomic(&path)?;
        assert_eq!(std::fs::read_to_string(&path)?, PRE_LEDGER_FILE);
        // Ids displaced while holding nothing (heard only on `status`) are
        // not remembered either, so a ghost flood leaves the same shape.
        let mut table =
            PeerTable::from_summary(PeerSummaryFile::from_json(PRE_LEDGER_FILE.as_bytes())?);
        for i in 0..(2 * FLEET_PEER_MAX) {
            table.observe(
                &format!("securacv/ghost-{i:04}/status"),
                b"{}",
                false,
                NOW + 20 + i as u64,
            );
        }
        assert!(table.evictions() > 0);
        assert_eq!(table.displaced_len(), 0);
        assert!(!serde_json::to_string(&table.summary(NOW + 500))?.contains("displaced"));
        Ok(())
    }

    #[test]
    fn a_reader_ignores_fields_it_does_not_know() -> Result<()> {
        // The downgrade path: a bridge or kernel without the ledger read a
        // file with one because none of these structs denies unknown fields.
        // Held here for whatever field comes next, at every level the file
        // has — and an entry this reader cannot use is dropped, not trusted.
        let plain = PeerSummaryFile::from_json(PRE_LEDGER_FILE.as_bytes())?;
        let mut doc: serde_json::Value = serde_json::from_str(PRE_LEDGER_FILE)?;
        doc["a_later_field"] = serde_json::json!({ "anything": [1, 2] });
        doc["peers"][0]["a_later_field"] = serde_json::json!(true);
        doc["peers"][0]["last_verified_state"]["a_later_field"] = serde_json::json!(0);
        doc["displaced"] = serde_json::json!([{
            "device_id": "studio",
            "pinned_key_hex": hex::encode([0x5au8; 32]),
            "last_verified_length": 2,
            "a_later_field": "x"
        }]);
        let newer = PeerSummaryFile::from_json(doc.to_string().as_bytes())?;
        assert_eq!(newer.peers, plain.peers);
        assert_eq!(newer.displaced.len(), 1);
        assert_eq!(newer.displaced[0].last_verified_length, Some(2));
        let table = PeerTable::from_summary(newer);
        assert_eq!((table.len(), table.displaced_len()), (1, 1));
        Ok(())
    }

    #[test]
    fn a_damaged_ledger_loads_bounded_and_a_resident_record_wins() {
        let pin = |b: u8| hex::encode([b; 32]);
        let entry = |id: &str, key: Option<String>, length: Option<u64>| DisplacedPin {
            device_id: id.to_string(),
            pinned_key_hex: key,
            last_verified_length: length,
            last_verified_state: None,
            pin_conflict: false,
        };
        let mut resident = PeerRecord::new("porch", NOW);
        resident.pinned_key_hex = Some(pin(0xa1));
        let mut displaced: Vec<DisplacedPin> = (0..(2 * FLEET_PEER_DISPLACED_MAX))
            .map(|i| entry(&format!("x{i:04}"), Some(pin(0x11)), Some(1)))
            .collect();
        // At the newest end, where the bound alone would keep them: each is
        // dropped by its own rule, and none takes a slot from the bound.
        displaced.push(entry("twice", Some(pin(0xd4)), Some(1)));
        displaced.push(entry("porch", Some(pin(0xb2)), Some(9)));
        displaced.push(entry("..", Some(pin(0xc3)), None));
        displaced.push(entry("ghost", None, None));
        displaced.push(entry("twice", Some(pin(0xe5)), Some(2)));
        let table = PeerTable::from_summary(PeerSummaryFile {
            schema: FLEET_PEERS_SCHEMA.to_string(),
            written_at_epoch_s: NOW,
            peers: vec![resident],
            displaced,
        });
        assert_eq!(table.displaced_len(), FLEET_PEER_DISPLACED_MAX);
        let kept: Vec<&str> = table
            .displaced
            .iter()
            .map(|d| d.device_id.as_str())
            .collect();
        assert_eq!(kept.last(), Some(&"twice"));
        assert_eq!(
            table.displaced.back().and_then(|d| d.last_verified_length),
            Some(2)
        );
        assert_eq!(
            kept.first(),
            Some(&format!("x{:04}", FLEET_PEER_DISPLACED_MAX + 1).as_str()),
            "the newest entries are kept, oldest first"
        );
        assert_eq!(kept.iter().filter(|id| **id == "twice").count(), 1);
        for gone in ["porch", "..", "ghost"] {
            assert!(!kept.contains(&gone), "{gone} survived the load");
        }
        let porch = record(&table, "porch", NOW).expect("porch");
        assert_eq!(
            porch.pinned_key_hex,
            Some(pin(0xa1)),
            "the resident record wins"
        );
    }

    #[test]
    fn a_sustained_flood_keeps_the_table_the_ledger_and_the_file_bounded() -> Result<()> {
        // Every flood id is as wide as the alphabet allows, proven, and
        // carries signed words, the widest name and the widest product.
        let widest_name = format!("{}{}", "\"".repeat(32), "\u{1F600}".repeat(16));
        assert_eq!(
            clean_name(&widest_name).as_deref(),
            Some(widest_name.as_str())
        );
        let meta = serde_json::json!({ "name": widest_name }).to_string();
        let status = serde_json::json!({ "device_type": "p".repeat(32) }).to_string();
        let extra = 50;
        let total = FLEET_PEER_MAX + FLEET_PEER_DISPLACED_MAX + extra;
        let mut table = PeerTable::default();
        for i in 0..total {
            let key = other_key();
            let id = format!("x{i:047}");
            let t = NOW + i as u64;
            table.observe(&format!("securacv/{id}/health"), &health(&key), false, t);
            table.observe(
                &format!("securacv/{id}/chain"),
                &signed_chain(&key, &id, u64::MAX),
                false,
                t,
            );
            table.observe(
                &format!("securacv/{id}/state"),
                &signed_state(&key, &id, (u64::MAX, u64::MAX), "present", "2+", Some(true)),
                false,
                t,
            );
            table.observe(&format!("securacv/{id}/meta"), meta.as_bytes(), false, t);
            table.observe(
                &format!("securacv/{id}/status"),
                status.as_bytes(),
                false,
                t,
            );
        }
        assert_eq!(table.len(), FLEET_PEER_MAX);
        assert_eq!(table.displaced_len(), FLEET_PEER_DISPLACED_MAX);
        assert_eq!(table.evictions(), (total - FLEET_PEER_MAX) as u64);
        assert_eq!(table.pins_released(), extra as u64);
        let dir = tempfile::tempdir()?;
        let path = dir.path().join("fleet_peers.json");
        let now = NOW + total as u64;
        table.summary(now).write_atomic(&path)?;
        let flooded = std::fs::metadata(&path)?.len();
        // The same file with every remaining field at its widest too.
        let mut widest = table.summary(now);
        for peer in &mut widest.peers {
            peer.offline_epoch_s = Some(u64::MAX);
            peer.last_seen_epoch_s = u64::MAX;
            peer.last_signed_epoch_s = Some(u64::MAX);
            peer.wellbeing_epoch_s = Some(u64::MAX);
            peer.chain = CHAIN_DEGRADED.to_string();
            peer.pin_conflict = true;
        }
        for entry in &mut widest.displaced {
            entry.last_verified_state = Some(StateMark {
                chain_length: u64::MAX,
                seq: u64::MAX,
            });
            entry.pin_conflict = true;
        }
        widest.written_at_epoch_s = u64::MAX;
        let widest_path = dir.path().join("widest.json");
        widest.write_atomic(&widest_path)?;
        let widest_len = std::fs::metadata(&widest_path)?.len();
        eprintln!("flooded summary {flooded} bytes, widest {widest_len} bytes");
        assert!(
            widest_len < FLEET_PEERS_MAX_FILE_BYTES,
            "a full table and ledger ({widest_len} bytes) would be refused by the kernel"
        );
        // The kernel still serves the flooded file, capped as ever.
        assert_eq!(load_rows(&path, now).len(), FLEET_PEER_MAX);
        Ok(())
    }

    #[test]
    fn the_projection_never_reads_the_ledger() {
        let real = signer();
        let mut table = PeerTable::default();
        proven_porch(&mut table, &real, NOW);
        let t = displace_porch(&mut table, "fake", NOW + 10);
        let with = table.summary(t);
        assert_eq!(with.displaced.len(), 1);
        let without = PeerSummaryFile {
            displaced: Vec::new(),
            ..with.clone()
        };
        let rows = fleet_rows(&with, t);
        assert_eq!(rows, fleet_rows(&without, t));
        let wire = serde_json::to_string(&rows).expect("rows serialize");
        assert!(
            !wire.contains("porch"),
            "a displaced id reached the wire: {wire}"
        );
        assert!(!wire.contains(&pubkey_hex(&real)[..16]));
        // The kernel's own read path, from the file the bridge writes.
        let dir = tempfile::tempdir().expect("tempdir");
        let path = dir.path().join("fleet_peers.json");
        with.write_atomic(&path).expect("written");
        assert!(std::fs::read_to_string(&path)
            .expect("read")
            .contains("\"displaced\""));
        assert_eq!(load_rows(&path, t), rows);
    }

    #[test]
    fn at_the_cap_a_newcomer_displaces_the_least_useful_id_never_a_real_lockout() {
        let key = signer();
        let mut table = PeerTable::default();
        // One proven Canary, heard early.
        table.observe("securacv/porch/health", &health(&key), false, NOW);
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&key, "porch", 1),
            false,
            NOW,
        );
        // A flood of never-proven ids fills the rest of the table.
        for i in 0..(FLEET_PEER_MAX - 1) {
            table.observe(
                &format!("securacv/ghost-{i:03}/status"),
                b"{}",
                false,
                NOW + 1 + i as u64,
            );
        }
        assert_eq!(table.len(), FLEET_PEER_MAX);
        assert_eq!(table.evictions(), 0);
        // Five more: each displaces the oldest UNPROVEN id; porch, older than
        // all of them but proven, survives, and the table never grows.
        for i in 0..5 {
            assert!(table.observe(
                &format!("securacv/late-{i}/status"),
                b"{}",
                false,
                NOW + 500 + i
            ));
        }
        assert_eq!(table.len(), FLEET_PEER_MAX);
        assert_eq!(table.evictions(), 5);
        let summary = table.summary(NOW + 600);
        assert!(summary.peers.iter().any(|p| p.device_id == "porch"));
        for i in 0..5 {
            assert!(
                !summary
                    .peers
                    .iter()
                    .any(|p| p.device_id == format!("ghost-{i:03}")),
                "ghost-{i:03} should have been the one displaced"
            );
            assert!(summary
                .peers
                .iter()
                .any(|p| p.device_id == format!("late-{i}")));
        }
        // Rehydrate the file (written at NOW + 600) and jump the clock past
        // the forget window in one step: that is a clock, not a fleet, so
        // NOTHING is expired — every pin survives.
        let mut rehydrated = PeerTable::from_summary(summary);
        let much_later = NOW + 600 + FLEET_PEER_FORGET_SECS + 1;
        assert!(rehydrated.clock_jumped_since(much_later));
        assert_eq!(rehydrated.prune(much_later), 0);
        assert_eq!(rehydrated.len(), FLEET_PEER_MAX);
        // Time flowing normally (an observation every 12 h) past the window:
        // the never-proven ghosts age out, the proven porch is kept even
        // though it has been silent just as long.
        let mut t = NOW + 600;
        while t < NOW + 600 + FLEET_PEER_FORGET_SECS {
            t += 12 * 3600;
            rehydrated.observe("securacv/keeper/status", b"{}", false, t);
        }
        // keeper's first observation arrived at the cap and displaced one
        // never-proven ghost, so 62 stale ghosts remain to age out — never
        // porch, and not keeper (heard within the window).
        let dropped = rehydrated.prune(t + 1);
        assert_eq!(
            dropped,
            FLEET_PEER_MAX - 2,
            "the stale never-proven ghosts, not porch"
        );
        let left = rehydrated.summary(t + 1);
        assert!(left.peers.iter().any(|p| p.device_id == "porch"));
        assert!(left.peers.iter().any(|p| p.device_id == "keeper"));
        assert_eq!(left.peers.len(), 2);
    }

    #[test]
    fn a_clock_jump_never_erases_pins_and_a_proven_pin_never_expires() {
        let key = signer();
        let mut table = PeerTable::default();
        table.observe("securacv/porch/health", &health(&key), false, NOW);
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&key, "porch", 1),
            false,
            NOW + 1,
        );
        table.observe("securacv/ghost/status", b"{}", false, NOW + 1);
        // A device whose health pinned a key but whose chain never verified:
        // pinned, never proven. Its pin must survive any clock.
        let fresh = other_key();
        table.observe("securacv/fresh/health", &health(&fresh), false, NOW + 1);
        // The bridge restarts with the host clock 40 days ahead (a bad RTC or
        // NTP answer) and flushes: the pass straddling the jump is skipped,
        // and the table's clock is not taught the jump by that pass.
        let mut reopened = PeerTable::from_summary(table.summary(NOW + 2));
        let ahead = NOW + 2 + 40 * 86_400;
        assert_eq!(reopened.prune(ahead), 0);
        assert_eq!(reopened.len(), 3);
        assert_eq!(
            reopened.prune(ahead + 30),
            0,
            "a second poll tick learns nothing either"
        );
        // A publish arrives under the jumped clock; the next pass runs against
        // it — and can forget only the unpinned ghost. Both pins survive.
        reopened.observe("securacv/porch/status", b"{}", false, ahead + 31);
        assert_eq!(reopened.prune(ahead + 32), 1);
        let after = reopened.summary(ahead + 32);
        assert!(!after.peers.iter().any(|p| p.device_id == "ghost"));
        assert!(after
            .peers
            .iter()
            .any(|p| p.device_id == "porch" && p.pinned_key_hex.is_some()));
        assert!(after
            .peers
            .iter()
            .any(|p| p.device_id == "fresh" && p.pinned_key_hex.is_some() && !p.proven()));
        // With the clock flowing normally for 31 days of silence, the proven
        // pin is still not the clock's to expire; the ghost is.
        let mut t = NOW + 2;
        while t < NOW + 2 + FLEET_PEER_FORGET_SECS + 3600 {
            t += 6 * 3600;
            table.observe("securacv/other/status", b"{}", false, t);
        }
        assert_eq!(table.prune(t), 1);
        let left = table.summary(t);
        assert!(left.peers.iter().any(|p| p.device_id == "porch"));
        assert!(!left.peers.iter().any(|p| p.device_id == "ghost"));
    }

    #[test]
    fn a_replayed_signed_publish_proves_nothing_new() {
        let key = signer();
        let mut table = PeerTable::default();
        table.observe("securacv/porch/health", &health(&key), true, NOW);
        // The broker's retained head: verdict, and the length is now "seen".
        let head = signed_chain(&key, "porch", 5);
        table.observe("securacv/porch/chain", &head, true, NOW + 1);
        assert!(!fleet_rows(&table.summary(NOW + 1), NOW + 1)[0].online);
        // The identical bytes arriving LIVE — a capture from anyone on the
        // broker, or the head published again — advance nothing.
        table.observe("securacv/porch/chain", &head, false, NOW + 2);
        assert!(!fleet_rows(&table.summary(NOW + 2), NOW + 2)[0].online);
        // Nor does an older publish.
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&key, "porch", 4),
            false,
            NOW + 3,
        );
        assert!(!fleet_rows(&table.summary(NOW + 3), NOW + 3)[0].online);
        // A genuine advance does.
        let six = signed_chain(&key, "porch", 6);
        table.observe("securacv/porch/chain", &six, false, NOW + 4);
        assert!(fleet_rows(&table.summary(NOW + 4), NOW + 4)[0].online);
        // Replaying THAT a week later holds nothing open.
        let week = NOW + 7 * 24 * 3600;
        table.observe("securacv/porch/chain", &six, false, week);
        let rows = fleet_rows(&table.summary(week), week);
        assert!(!rows[0].online, "a replay is not presence: {rows:?}");
        assert_eq!(rows[0].chain.as_deref(), Some("ok"));
        assert_eq!(table.summary(week).peers[0].last_verified_length, Some(6));
    }

    #[test]
    fn an_unsigned_health_with_another_key_changes_no_verdict_by_itself() {
        let key = signer();
        let impostor = other_key();
        let mut table = PeerTable::default();
        table.observe("securacv/porch/health", &health(&key), false, NOW);
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&key, "porch", 1),
            false,
            NOW + 1,
        );
        // Anyone can publish a health with a different key. Alone it is noise.
        table.observe("securacv/porch/health", &health(&impostor), false, NOW + 2);
        let rows = fleet_rows(&table.summary(NOW + 2), NOW + 2);
        assert_eq!(rows[0].chain.as_deref(), Some("ok"));
        assert!(rows[0].online);
        assert!(!table.summary(NOW + 2).peers[0].pin_conflict);
        // The real device keeps signing under the pin: still ok, and the
        // announcement is forgotten.
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&key, "porch", 2),
            false,
            NOW + 3,
        );
        assert_eq!(
            fleet_rows(&table.summary(NOW + 3), NOW + 3)[0]
                .chain
                .as_deref(),
            Some("ok")
        );
        // A publish signed by neither key is not evidence of a second key
        // either: the verdict says the last publish failed, nothing sticks.
        let garbage = other_key();
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&garbage, "porch", 3),
            false,
            NOW + 4,
        );
        assert_eq!(
            fleet_rows(&table.summary(NOW + 4), NOW + 4)[0]
                .chain
                .as_deref(),
            Some("degraded")
        );
        assert!(!table.summary(NOW + 4).peers[0].pin_conflict);
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&key, "porch", 3),
            false,
            NOW + 5,
        );
        assert_eq!(
            fleet_rows(&table.summary(NOW + 5), NOW + 5)[0]
                .chain
                .as_deref(),
            Some("ok")
        );
    }

    #[test]
    fn the_wap_lwt_shape_counts_as_offline() {
        let key = signer();
        let mut table = PeerTable::default();
        table.observe("securacv/porch/health", &health(&key), false, NOW);
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&key, "porch", 1),
            false,
            NOW + 1,
        );
        assert!(fleet_rows(&table.summary(NOW + 1), NOW + 1)[0].online);
        // The canary-wap's will is `{"online":false}` on the status topic.
        table.observe(
            "securacv/porch/status",
            br#"{"online":false}"#,
            false,
            NOW + 2,
        );
        assert!(!fleet_rows(&table.summary(NOW + 2), NOW + 2)[0].online);
        // …and `{"online":true}` is an unsigned heartbeat, so still not online.
        table.observe(
            "securacv/porch/status",
            br#"{"online":true}"#,
            false,
            NOW + 3,
        );
        assert!(!fleet_rows(&table.summary(NOW + 3), NOW + 3)[0].online);
    }

    #[test]
    fn the_projection_recleans_and_caps_whatever_the_file_says() {
        let mut dirty = PeerRecord::new("z", NOW);
        dirty.name = Some("line1\nline2\u{202e}\u{7}".to_string());
        dirty.device_type = Some("<script>alert(1)</script>".to_string());
        dirty.presence = Some("levitating".to_string());
        dirty.occupants = Some("many".to_string());
        dirty.breathing = Some(true);
        dirty.wellbeing_epoch_s = Some(NOW);
        dirty.last_verified_state = Some(StateMark {
            chain_length: 1,
            seq: 1,
        });
        dirty.last_signed_epoch_s = Some(NOW);
        dirty.chain = CHAIN_OK.to_string();
        let mut peers = vec![dirty];
        for i in 0..(FLEET_PEER_MAX * 3) {
            peers.push(PeerRecord::new(&format!("a{i:04}"), NOW));
        }
        let summary = PeerSummaryFile {
            schema: FLEET_PEERS_SCHEMA.to_string(),
            written_at_epoch_s: NOW,
            peers,
            displaced: Vec::new(),
        };
        let rows = fleet_rows(&summary, NOW);
        assert_eq!(
            rows.len(),
            FLEET_PEER_MAX,
            "the row count is capped on read"
        );
        // Sorted by id, so "z" is past the cap; project it alone to inspect.
        let summary_z = PeerSummaryFile {
            peers: vec![summary.peers[0].clone()],
            ..summary
        };
        let row = &fleet_rows(&summary_z, NOW)[0];
        assert_eq!(row.name, "line1line2");
        assert_eq!(row.product.as_deref(), Some("scriptalert1script"));
        assert!(row.online);
        assert!(row.presence.is_none() && row.occupants.is_none());
        assert_eq!(row.breathing, Some(true));
    }

    #[test]
    fn words_a_pre_signature_bridge_stored_are_served_by_nobody_after_the_upgrade() {
        // A summary written before the `state` signature requirement: fresh
        // words beside a proven-online row, and no verified mark — because
        // the bridge that wrote them never checked one.
        let mut old = PeerRecord::new("bedroom", NOW);
        old.presence = Some("present".to_string());
        old.occupants = Some("2+".to_string());
        old.breathing = Some(true);
        old.wellbeing_epoch_s = Some(NOW);
        old.last_signed_epoch_s = Some(NOW);
        old.last_verified_length = Some(3);
        old.chain = CHAIN_OK.to_string();
        let summary = PeerSummaryFile {
            schema: FLEET_PEERS_SCHEMA.to_string(),
            written_at_epoch_s: NOW,
            peers: vec![old],
            displaced: Vec::new(),
        };
        // The kernel, reading that file as it stands: online, no words.
        let rows = fleet_rows(&summary, NOW + 1);
        assert!(rows[0].online);
        assert!(
            rows[0].presence.is_none()
                && rows[0].occupants.is_none()
                && rows[0].breathing.is_none(),
            "unvouched words served: {rows:?}"
        );
        // The upgraded bridge, rehydrating it: the words are forgotten with
        // the file's next write, the pin and the proof are kept.
        let table = PeerTable::from_summary(summary);
        let rec = &table.summary(NOW + 2).peers[0];
        assert!(rec.presence.is_none() && rec.wellbeing_epoch_s.is_none());
        assert_eq!(rec.last_verified_length, Some(3));
        assert!(rec.proven());
    }

    #[test]
    fn an_oversize_summary_file_is_ignored_not_served() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let path = dir.path().join("fleet_peers.json");
        let mut peers = Vec::new();
        for i in 0..20_000 {
            peers.push(PeerRecord::new(&format!("p{i}"), NOW));
        }
        let big = PeerSummaryFile {
            schema: FLEET_PEERS_SCHEMA.to_string(),
            written_at_epoch_s: NOW,
            peers,
            displaced: Vec::new(),
        };
        big.write_atomic(&path)?;
        assert!(std::fs::metadata(&path)?.len() > FLEET_PEERS_MAX_FILE_BYTES);
        let err = PeerSummaryFile::read(&path).unwrap_err();
        assert!(format!("{err:#}").contains("bound"), "{err:#}");
        assert!(load_rows(&path, NOW).is_empty());
        Ok(())
    }

    #[cfg(unix)]
    #[test]
    fn the_summary_file_is_written_private() -> Result<()> {
        use std::os::unix::fs::PermissionsExt;
        let dir = tempfile::tempdir()?;
        let path = dir.path().join("fleet_peers.json");
        // A pre-existing, world-readable temp file (a crash mid-write) is
        // re-narrowed, not inherited.
        let tmp = dir.path().join("fleet_peers.json.tmp");
        std::fs::write(&tmp, b"stale")?;
        std::fs::set_permissions(&tmp, std::fs::Permissions::from_mode(0o644))?;
        PeerTable::default().summary(NOW).write_atomic(&path)?;
        let mode = std::fs::metadata(&path)?.permissions().mode() & 0o777;
        assert_eq!(mode, 0o600, "the summary holds room words and pins");
        assert!(!tmp.exists());
        // A symlink planted at the temp path is removed, never followed: the
        // write succeeds, the link's target is untouched, and the next write
        // is not doomed by a squatter.
        let elsewhere = dir.path().join("elsewhere");
        std::os::unix::fs::symlink(&elsewhere, &tmp)?;
        PeerTable::default().summary(NOW).write_atomic(&path)?;
        assert!(!elsewhere.exists());
        assert!(!tmp.exists());
        Ok(())
    }

    #[cfg(unix)]
    #[test]
    fn a_fifo_at_either_path_is_refused_without_blocking() -> Result<()> {
        use std::ffi::CString;
        use std::os::unix::ffi::OsStrExt;
        let dir = tempfile::tempdir()?;
        let mkfifo = |p: &Path| {
            let c = CString::new(p.as_os_str().as_bytes()).expect("path");
            // SAFETY: a valid NUL-terminated path; mkfifo has no other
            // preconditions.
            assert_eq!(unsafe { libc::mkfifo(c.as_ptr(), 0o600) }, 0, "mkfifo");
        };
        // A FIFO squatting at the temp path: removed, then the write lands.
        let path = dir.path().join("fleet_peers.json");
        mkfifo(&dir.path().join("fleet_peers.json.tmp"));
        PeerTable::default().summary(NOW).write_atomic(&path)?;
        assert!(PeerSummaryFile::read(&path)?.is_some());
        // A FIFO at the summary path itself: the kernel refuses it at once
        // instead of parking its accept loop on a reader-less open.
        let fifo = dir.path().join("summary.fifo");
        mkfifo(&fifo);
        let err = PeerSummaryFile::read(&fifo).unwrap_err();
        assert!(format!("{err:#}").contains("regular file"), "{err:#}");
        assert!(load_rows(&fifo, NOW).is_empty());
        // Writing to a FIFO at the SUMMARY path replaces it: rename is atomic
        // and never opens the target.
        PeerTable::default().summary(NOW).write_atomic(&fifo)?;
        assert!(std::fs::metadata(&fifo)?.file_type().is_file());
        Ok(())
    }

    #[test]
    fn the_file_round_trips_and_a_foreign_schema_yields_no_rows() -> Result<()> {
        let key = signer();
        let mut table = PeerTable::default();
        table.observe("securacv/porch/health", &health(&key), false, NOW);
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&key, "porch", 1),
            false,
            NOW + 1,
        );
        let dir = tempfile::tempdir()?;
        let path = dir.path().join("fleet_peers.json");
        assert!(load_rows(&path, NOW).is_empty(), "no file yet is no peers");
        table.summary(NOW + 1).write_atomic(&path)?;
        assert!(!path.with_extension("json.tmp").exists());

        let rows = load_rows(&path, NOW + 2);
        assert_eq!(rows.len(), 1);
        assert!(rows[0].online);
        assert_eq!(rows[0].chain.as_deref(), Some("ok"));

        // Rehydrating keeps the pin: the impostor is still an impostor.
        let rehydrated = PeerSummaryFile::read(&path)?.expect("file exists");
        let mut table = PeerTable::from_summary(rehydrated);
        let impostor = other_key();
        table.observe("securacv/porch/health", &health(&impostor), false, NOW + 5);
        table.observe(
            "securacv/porch/chain",
            &signed_chain(&impostor, "porch", 2),
            false,
            NOW + 6,
        );
        assert!(table.summary(NOW + 6).peers[0].pin_conflict);
        assert!(!fleet_rows(&table.summary(NOW + 6), NOW + 6)[0].online);

        std::fs::write(
            &path,
            br#"{"schema":"securacv/fleet_peers/v9","written_at_epoch_s":1,"peers":[]}"#,
        )?;
        assert!(load_rows(&path, NOW).is_empty());
        std::fs::write(&path, b"not json")?;
        assert!(load_rows(&path, NOW).is_empty());
        Ok(())
    }
}
