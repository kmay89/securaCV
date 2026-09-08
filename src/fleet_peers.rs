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
//! advance this bridge did not itself see, never an indefinitely "online"
//! powered-off Canary. The same peer can invent device ids and sign for them
//! with its own key (the pin is TOFU, so such a row is indistinguishable from
//! a real Canary), and can hold a real id in the sticky `degraded` verdict by
//! announcing a second key and signing under it. [`FLEET_PEER_MAX`] bounds the
//! table and eviction bounds a flood's effect to its duration; none of it is
//! a liveness proof, and `tvos/discovery/DISCOVERY.md` says so in the same
//! words. A liveness challenge would use the firmware's `whoami` canonical,
//! which nothing here drives yet.
//!
//! The pin is TOFU, not pairing: "chain: ok" means the last chain publish
//! verified against the first key this bridge ever saw for that device id,
//! which is weaker than a key pinned at pairing and is described that way in
//! `tvos/discovery/DISCOVERY.md`. A second key merely ANNOUNCED for a pinned
//! id (an unsigned `health`, which anyone on the broker can publish) is
//! remembered and changes no verdict; a second key that actually SIGNS the
//! id's chain is a sticky conflict ("degraded") that only deleting the summary
//! file clears — never a silent re-pin.
//!
//! The summary file holds the per-room wellbeing words beside the pins, so it
//! is written `0600` (unix), fsynced before and after the rename, and read
//! back only up to [`FLEET_PEERS_MAX_FILE_BYTES`]; [`fleet_rows`] re-cleans
//! every field it serves and caps the row count, so a damaged or hand-edited
//! file cannot widen the wire.

use std::collections::BTreeMap;
use std::path::Path;

use anyhow::{anyhow, Context, Result};
use ed25519_dalek::{Signature, Verifier, VerifyingKey};
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
/// anything not heard for [`FLEET_PEER_FORGET_SECS`], then the least recently
/// heard id that never produced a verified signature, then the least recently
/// heard id of all — so a broker flood can crowd the roll-call only while it
/// lasts, never lock real Canaries out until someone deletes the file. An
/// evicted id loses its pin and is re-pinned on its next `health`.
pub const FLEET_PEER_MAX: usize = 64;

/// How long an id stays in the table without being heard at all before it is
/// forgotten (pin included): 30 days, pruned on every flush and on rehydrate.
pub const FLEET_PEER_FORGET_SECS: u64 = 30 * 24 * 60 * 60;

/// The most the kernel will read of a summary file. [`FLEET_PEER_MAX`] fully
/// populated records pretty-print to a few tens of kilobytes; anything past
/// this is a damaged or foreign file, and is ignored rather than served.
pub const FLEET_PEERS_MAX_FILE_BYTES: u64 = 256 * 1024;

/// The topic filters the bridge subscribes to when peer tracking is on. The
/// fleet roll-call surfaces only: `events`, `sensing`, `counts` and `tamper`
/// are deliberately absent — the bridge is the kernel's egress and must not
/// become a consumer of witness traffic. What it does consume is the coarse
/// roll-call: `state` for the contract's wellbeing WORDS (never the readings
/// beside them), `health` for the device's public key (stored locally, never
/// projected), and `chain`, the one signed publish the display verifies too,
/// so presence is proven the same way on both.
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
    /// Coarse room words from a LIVE `state` publish: `"clear"`/`"present"`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub presence: Option<String>,
    /// `"0"` / `"1"` / `"2+"`, same source and freshness as `presence`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub occupants: Option<String>,
    /// `breathing_locked` from the same publish.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub breathing: Option<bool>,
    /// When the wellbeing words above were last heard live; they are served
    /// only while this is within [`FLEET_PEER_RECENT_SECS`].
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wellbeing_epoch_s: Option<u64>,
    /// A `chain` publish that arrived before the pin (retained-delivery order
    /// is arbitrary), held so the pin can evaluate it when it lands. Not
    /// persisted: after a restart the pin is already on disk.
    #[serde(skip)]
    pending_chain: Option<PendingChain>,
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
            pending_chain: None,
        }
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

/// The file the bridge writes and the kernel reads.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PeerSummaryFile {
    /// Always [`FLEET_PEERS_SCHEMA`].
    pub schema: String,
    /// When the bridge wrote it (Unix seconds).
    pub written_at_epoch_s: u64,
    /// Sorted by `device_id`.
    pub peers: Vec<PeerRecord>,
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
        let file = match std::fs::File::open(path) {
            Ok(file) => file,
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            Err(err) => return Err(err).with_context(|| format!("opening {}", path.display())),
        };
        let declared = file
            .metadata()
            .with_context(|| format!("sizing {}", path.display()))?
            .len();
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

/// Create-or-truncate `path` with mode `0600`, refuse to follow a symlink in
/// its place, write, and fsync. `.mode()` applies only when the file is
/// created, so a temp file left by a crash mid-write is re-narrowed
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
        .custom_flags(libc::O_NOFOLLOW)
        .open(path)?;
    file.set_permissions(std::fs::Permissions::from_mode(0o600))?;
    file.write_all(bytes)?;
    file.sync_all()
}

#[cfg(not(unix))]
fn write_private_file(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    use std::io::Write;
    let mut file = std::fs::File::create(path)?;
    file.write_all(bytes)?;
    file.sync_all()
}

/// The bridge's in-memory table, keyed by device id.
#[derive(Debug, Default)]
pub struct PeerTable {
    peers: BTreeMap<String, PeerRecord>,
    /// Ids displaced at the cap since this table was built (the bridge logs
    /// each increase — an eviction drops that id's pin).
    evictions: u64,
}

impl PeerTable {
    /// Rehydrate from a previously written summary so pins survive a bridge
    /// restart ("a reboot must not re-TOFU", as the display's pin store puts
    /// it). Records with an invalid id are dropped, never trusted.
    pub fn from_summary(summary: PeerSummaryFile) -> Self {
        let peers = summary
            .peers
            .into_iter()
            .filter(|p| device_id_ok(&p.device_id))
            .map(|p| (p.device_id.clone(), p))
            .collect();
        Self {
            peers,
            evictions: 0,
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

    /// Forget every id not heard within [`FLEET_PEER_FORGET_SECS`], pin
    /// included. Returns how many were dropped. A stamp from the future is a
    /// damaged record, not a recent one, and is kept only until it ages out
    /// like any other.
    pub fn prune(&mut self, now: u64) -> usize {
        let before = self.peers.len();
        self.peers
            .retain(|_, p| now.saturating_sub(p.last_seen_epoch_s) <= FLEET_PEER_FORGET_SECS);
        before - self.peers.len()
    }

    /// The id to displace for a newcomer at the cap: never proven before
    /// proven, then the least recently heard.
    fn eviction_candidate(&self) -> Option<String> {
        self.peers
            .values()
            .min_by_key(|p| (p.last_signed_epoch_s.is_some(), p.last_seen_epoch_s))
            .map(|p| p.device_id.clone())
    }

    /// Feed one received publish. `retained` is the wire flag rumqttc reports
    /// (set by the broker only for a message replayed to a new subscription).
    /// Returns `true` when the table changed and should be flushed; `false`
    /// for topics this module does not handle or malformed ids. A new id at
    /// the cap displaces one (see [`FLEET_PEER_MAX`]) rather than being
    /// refused, so a flood cannot lock real Canaries out for good.
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
        if !self.peers.contains_key(device_id)
            && self.peers.len() >= FLEET_PEER_MAX
            && self.prune(now) == 0
        {
            if let Some(victim) = self.eviction_candidate() {
                self.peers.remove(&victim);
                self.evictions += 1;
            }
        }
        let rec = self
            .peers
            .entry(device_id.to_string())
            .or_insert_with(|| PeerRecord::new(device_id, now));
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
                if retained {
                    // A replayed room claim is history, not a reading.
                    return true;
                }
                let presence = doc
                    .get("presence_state")
                    .and_then(|v| v.as_str())
                    .and_then(presence_word);
                let occupants = doc
                    .get("occupants")
                    .and_then(|v| v.as_str())
                    .and_then(occupants_word);
                let breathing = doc.get("breathing_locked").and_then(|v| v.as_bool());
                if presence.is_some() || occupants.is_some() || breathing.is_some() {
                    rec.presence = presence;
                    rec.occupants = occupants;
                    rec.breathing = breathing;
                    rec.wellbeing_epoch_s = Some(now);
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
            let wellbeing = online && peer.wellbeing_fresh(now);
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
    let Ok(key) = VerifyingKey::from_bytes(pinned) else {
        return false;
    };
    key.verify(canonical.as_bytes(), &Signature::from_bytes(&sig))
        .is_ok()
}

fn chain_canonical(device_id: &str, length: u64, latest_hash_hex: &str) -> String {
    format!("{SIG_PREFIX}|v{SCHEMA_V}|chain|{device_id}|{length}|{latest_hash_hex}")
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

    fn signer() -> SigningKey {
        SigningKey::from_bytes(&[7u8; 32])
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
        let other = SigningKey::from_bytes(&[9u8; 32])
            .verifying_key()
            .to_bytes();
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
        let impostor = SigningKey::from_bytes(&[3u8; 32]);
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
    fn wellbeing_words_ride_only_on_a_proven_row_and_only_while_fresh() {
        let key = signer();
        let mut table = PeerTable::default();
        table.observe("securacv/bedroom/health", &health(&key), false, NOW);
        let state = br#"{"device_type":"canary-sense","presence_state":"present","occupants":"1","breathing_locked":true,"range":"near","lux":138.0,"bpm":14}"#;
        // Retained: history, so no reading is taken.
        table.observe("securacv/bedroom/state", state, true, NOW + 1);
        // Live, but the row is not proven online: heard, not served.
        table.observe("securacv/bedroom/state", state, false, NOW + 2);
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
        for finer in ["range", "lux", "bpm", "rssi", "public_key", "pinned", "fp"] {
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
        let mut table = PeerTable::default();
        let state =
            br#"{"presence_state":"levitating","occupants":"many","breathing_locked":"yes"}"#;
        table.observe("securacv/k/state", state, false, NOW);
        let rec = &table.summary(NOW).peers[0];
        assert!(rec.presence.is_none() && rec.occupants.is_none() && rec.breathing.is_none());
        assert!(rec.wellbeing_epoch_s.is_none());
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
        // A file full of stale ids is pruned on rehydrate, so a lockout
        // cannot outlive the flood by more than FLEET_PEER_FORGET_SECS.
        let mut rehydrated = PeerTable::from_summary(summary);
        let much_later = NOW + 600 + FLEET_PEER_FORGET_SECS + 1;
        assert_eq!(rehydrated.prune(much_later), FLEET_PEER_MAX);
        assert!(rehydrated.is_empty());
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
        let impostor = SigningKey::from_bytes(&[3u8; 32]);
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
        let garbage = SigningKey::from_bytes(&[5u8; 32]);
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
        // A symlink planted at the temp path is refused, not followed.
        let elsewhere = dir.path().join("elsewhere");
        std::os::unix::fs::symlink(&elsewhere, &tmp)?;
        assert!(PeerTable::default()
            .summary(NOW)
            .write_atomic(&path)
            .is_err());
        assert!(!elsewhere.exists());
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
        let impostor = SigningKey::from_bytes(&[3u8; 32]);
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
