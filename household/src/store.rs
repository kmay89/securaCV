//! The checker's private data directory.
//!
//! ```text
//! <data_dir>/                    0700
//!   household.json               {"household": hex}            (immutable; written last at init)
//!   hub_keys.json                the hub's two PUBLIC keys: posture (witnesses pin it) and roster
//!   hub_keys.sealed              the hub's two signing keys, sealed under the KEK (see crate::secrets)
//!   highwater.json               {"epoch", "digest"}: highest roster ever accepted
//!   seq.json                     {"last_seq"}: last posture statement seq
//!   roster/chain/<epoch>.json    every accepted signed roster, genesis first
//!   pending/<digest>.json        proposals awaiting approval
//!   bearers.json                 SHA-256 of each paired LAN bearer token
//!   cards.sealed                 card keys, sealed under the KEK (see crate::secrets)
//!   household_posture.bin        THE OUTPUT SEAM: the current 112-byte statement
//! ```
//!
//! Every write is atomic (temp file in the same directory, mode 0600, fsync,
//! rename, fsync the directory). Every read is bounded.
//!
//! Nothing here records which credential was used: there are no counters,
//! no tap log and no per-member timestamps. What the directory does say
//! about time, in full:
//!
//! - the output seam holds the current statement only. Its `issued_at` and
//!   `not_after`, and `seq.json`'s `last_seq` (whose value `>> 10` is that
//!   minute), state the minute of the last posture change or `home`
//!   renewal, and both files' modification times state its second. That was
//!   a command (a renewed `home` included), a lapse, a roster change that
//!   ended `home`, a republish (after a failed publish, or once a hub clock
//!   that ran ahead is corrected) or a restart. It is the time of the last
//!   act, not a history;
//! - a proposal's `proposed_at` (an admin act, needed for the 24-hour rule),
//!   deleted when the proposal is applied.
//!
//! The KEK lives outside this directory, so a copy of the directory alone (a
//! backup, a tarball) does not open the sealed files. Keeping the KEK out of
//! every backup that holds the directory is the operator's job: v0 does not,
//! and cannot, do it for you. A restored copy also rolls the roster
//! high-water mark back with it; v0 has no satellite to remember it (see the
//! README's limitations).

use crate::admin::Approval;
use crate::change::{self, HighWater, SignedRoster};
use crate::roster::Roster;
use crate::secrets::{HubKeys, Kek};
use crate::{hex, rng, Error, Result};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::io::Write;
use std::path::{Path, PathBuf};
use subtle::ConstantTimeEq;
use zeroize::Zeroizing;

/// The output seam's file name.
pub const SEAM_FILE: &str = "household_posture.bin";
/// Largest JSON file read from the data directory.
pub const MAX_JSON_BYTES: usize = 256 * 1024;
/// Longest roster chain loaded.
pub const MAX_CHAIN: usize = 100_000;
/// Most paired LAN clients. Far below what `bearers.json` can hold under
/// [`MAX_JSON_BYTES`], so the file can always be read back.
pub const MAX_BEARERS: usize = 32;

/// Whether `name` is a roster chain file name: twenty ASCII digits (the
/// epoch) and `.json`.
pub fn is_chain_file_name(name: &str) -> bool {
    name.len() == 25 && name.ends_with(".json") && name[..20].bytes().all(|b| b.is_ascii_digit())
}

/// Write `bytes` to `path` atomically with mode 0600.
pub fn write_atomic(path: &Path, bytes: &[u8]) -> Result<()> {
    let dir = path.parent().ok_or(Error::Invalid("path has no parent"))?;
    let name = path
        .file_name()
        .ok_or(Error::Invalid("path has no file name"))?;
    let tmp = dir.join(format!(
        ".{}.{}.tmp",
        name.to_string_lossy(),
        hex::encode(&rng::array::<8>()?)
    ));
    let mut opts = std::fs::OpenOptions::new();
    opts.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.mode(0o600);
    }
    let result = (|| {
        let mut f = opts.open(&tmp).map_err(Error::io("create a temp file"))?;
        f.write_all(bytes).map_err(Error::io("write a temp file"))?;
        f.sync_all().map_err(Error::io("sync a temp file"))?;
        drop(f);
        std::fs::rename(&tmp, path).map_err(Error::io("rename into place"))?;
        // The rename is only durable once the directory entry is: a write
        // whose directory sync failed is reported as failed, so a caller never
        // acknowledges a seq, roster or high-water update a power cut can undo.
        #[cfg(unix)]
        {
            let d = std::fs::File::open(dir).map_err(Error::io("open the data directory"))?;
            d.sync_all().map_err(Error::io("sync the data directory"))?;
        }
        Ok(())
    })();
    if result.is_err() {
        let _ = std::fs::remove_file(&tmp);
    }
    result
}

/// Read at most `max` bytes from `path`; larger files are refused.
pub fn read_bounded(path: &Path, max: usize) -> Result<Vec<u8>> {
    use std::io::Read;
    let f = std::fs::File::open(path).map_err(Error::io("open a data file"))?;
    let mut buf = Vec::new();
    f.take(max as u64 + 1)
        .read_to_end(&mut buf)
        .map_err(Error::io("read a data file"))?;
    if buf.len() > max {
        return Err(Error::Invalid("a data file is larger than allowed"));
    }
    Ok(buf)
}

fn read_json(path: &Path) -> Result<Value> {
    let b = read_bounded(path, MAX_JSON_BYTES)?;
    serde_json::from_slice(&b).map_err(|_| Error::Invalid("a data file is not valid JSON"))
}

fn write_json(path: &Path, v: &Value) -> Result<()> {
    let mut b = serde_json::to_vec_pretty(v).map_err(|_| Error::Invalid("JSON encoding failed"))?;
    b.push(b'\n');
    write_atomic(path, &b)
}

/// Create a directory (and its parents) with mode 0700.
pub fn mkdir_private(p: &Path) -> Result<()> {
    let mut b = std::fs::DirBuilder::new();
    b.recursive(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;
        b.mode(0o700);
    }
    b.create(p).map_err(Error::io("create a private directory"))
}

/// A proposal awaiting approval.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Pending {
    /// The proposed roster.
    pub roster: Roster,
    /// Approvals collected so far.
    pub approvals: Vec<Approval>,
    /// A holder revocation proof, if any.
    pub holder_proof: Option<Approval>,
    /// When the hub first recorded the proposal (Unix seconds).
    pub proposed_at: u64,
}

impl Pending {
    fn to_value(&self) -> Value {
        json!({
            "roster": self.roster.to_value(),
            "approvals": self.approvals,
            "holder_proof": self.holder_proof,
            "proposed_at": self.proposed_at,
        })
    }

    fn from_value(v: &Value) -> Result<Pending> {
        let o = v
            .as_object()
            .ok_or(Error::Invalid("pending is not an object"))?;
        if o.len() != 4 {
            return Err(Error::Invalid("pending has unexpected fields"));
        }
        Ok(Pending {
            roster: Roster::from_value(o.get("roster").ok_or(Error::Invalid("missing roster"))?)?,
            approvals: serde_json::from_value(o.get("approvals").cloned().unwrap_or(Value::Null))
                .map_err(|_| Error::Invalid("bad approvals"))?,
            holder_proof: serde_json::from_value(
                o.get("holder_proof").cloned().unwrap_or(Value::Null),
            )
            .map_err(|_| Error::Invalid("bad holder proof"))?,
            proposed_at: o
                .get("proposed_at")
                .and_then(Value::as_u64)
                .ok_or(Error::Invalid("missing proposed_at"))?,
        })
    }
}

/// The data directory.
#[derive(Clone, Debug)]
pub struct DataDir {
    root: PathBuf,
}

impl DataDir {
    /// Create a new data directory: household id and the hub's two keys,
    /// sealed under `kek` (whose file must live outside the directory).
    /// Refuses a directory that already holds a household.
    pub fn create(root: &Path, kek: &Kek) -> Result<DataDir> {
        mkdir_private(root)?;
        let d = DataDir {
            root: root.to_path_buf(),
        };
        if d.path("household.json").exists() {
            return Err(Error::Invalid(
                "this data directory already holds a household",
            ));
        }
        mkdir_private(&d.path("roster/chain"))?;
        mkdir_private(&d.path("pending"))?;
        let household: [u8; 16] = rng::array()?;
        let keys = HubKeys::generate()?;
        write_atomic(&d.path("hub_keys.sealed"), &keys.seal(kek, &household)?)?;
        write_json(
            &d.path("hub_keys.json"),
            &json!({
                "posture_public_key": hex::encode(&keys.posture.verifying_key().to_bytes()),
                "roster_public_key": hex::encode(&keys.roster.verifying_key().to_bytes()),
            }),
        )?;
        write_json(&d.path("seq.json"), &json!({"last_seq": 0}))?;
        write_json(&d.path("bearers.json"), &json!({"tokens": []}))?;
        // Last: a directory without household.json is an unfinished init.
        write_json(
            &d.path("household.json"),
            &json!({"household": hex::encode(&household)}),
        )?;
        Ok(d)
    }

    /// Open an existing data directory. On Unix, refuses one that group or
    /// others can read.
    pub fn open(root: &Path) -> Result<DataDir> {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let m = std::fs::metadata(root).map_err(Error::io("stat the data directory"))?;
            if m.permissions().mode() & 0o077 != 0 {
                return Err(Error::Invalid("the data directory must be private (0700)"));
            }
        }
        let d = DataDir {
            root: root.to_path_buf(),
        };
        d.household()?;
        Ok(d)
    }

    /// The directory itself.
    pub fn root(&self) -> &Path {
        &self.root
    }

    /// A path inside the directory.
    pub fn path(&self, rel: &str) -> PathBuf {
        self.root.join(rel)
    }

    /// The output seam path.
    pub fn seam_path(&self) -> PathBuf {
        self.path(SEAM_FILE)
    }

    /// The household id.
    pub fn household(&self) -> Result<[u8; 16]> {
        let v = read_json(&self.path("household.json"))?;
        hex::decode_array(
            v["household"]
                .as_str()
                .ok_or(Error::Invalid("missing household"))?,
        )
    }

    fn public_key(&self, field: &str) -> Result<ed25519_dalek::VerifyingKey> {
        let v = read_json(&self.path("hub_keys.json"))?;
        let b: [u8; 32] = hex::decode_array(
            v[field]
                .as_str()
                .ok_or(Error::Invalid("hub_keys.json is missing a key"))?,
        )?;
        ed25519_dalek::VerifyingKey::from_bytes(&b)
            .map_err(|_| Error::Invalid("hub_keys.json holds an invalid key"))
    }

    /// The hub posture public key witnesses pin (no KEK needed).
    pub fn posture_public_key(&self) -> Result<ed25519_dalek::VerifyingKey> {
        self.public_key("posture_public_key")
    }

    /// The hub roster public key the chain is checked against (no KEK
    /// needed).
    pub fn roster_public_key(&self) -> Result<ed25519_dalek::VerifyingKey> {
        self.public_key("roster_public_key")
    }

    /// Open the hub's signing keys with `kek`. Refuses a KEK that does not
    /// open them (so this is also the check that a KEK belongs to this
    /// household), and keys whose public halves differ from `hub_keys.json`.
    pub fn hub_keys(&self, kek: &Kek) -> Result<HubKeys> {
        let b = read_bounded(
            &self.path("hub_keys.sealed"),
            crate::secrets::MAX_SEALED_BYTES,
        )?;
        let keys = HubKeys::open(&b, kek, &self.household()?)?;
        if keys.posture.verifying_key() != self.posture_public_key()?
            || keys.roster.verifying_key() != self.roster_public_key()?
        {
            return Err(Error::Rejected(
                "the sealed hub keys do not match hub_keys.json",
            ));
        }
        Ok(keys)
    }

    /// The last posture statement seq.
    pub fn last_seq(&self) -> Result<u64> {
        read_json(&self.path("seq.json"))?["last_seq"]
            .as_u64()
            .ok_or(Error::Invalid("missing last_seq"))
    }

    /// Persist the last posture statement seq.
    pub fn save_seq(&self, seq: u64) -> Result<()> {
        write_json(&self.path("seq.json"), &json!({"last_seq": seq}))
    }

    /// The highest roster ever accepted.
    pub fn high_water(&self) -> Result<Option<HighWater>> {
        let p = self.path("highwater.json");
        if !p.exists() {
            return Ok(None);
        }
        let v = read_json(&p)?;
        Ok(Some(HighWater {
            epoch: v["epoch"].as_u64().ok_or(Error::Invalid("missing epoch"))?,
            digest: hex::decode_array(
                v["digest"]
                    .as_str()
                    .ok_or(Error::Invalid("missing digest"))?,
            )?,
        }))
    }

    fn raise_high_water(&self, r: &Roster) -> Result<()> {
        let cur = self.high_water()?;
        change::check_not_rollback(cur, None, r)?;
        if cur.is_none_or(|h| r.epoch > h.epoch) {
            write_json(
                &self.path("highwater.json"),
                &json!({"epoch": r.epoch, "digest": hex::encode(&r.digest()?)}),
            )?;
        }
        Ok(())
    }

    fn chain_path(&self, epoch: u64) -> PathBuf {
        self.path(&format!("roster/chain/{epoch:020}.json"))
    }

    /// Load the whole chain, genesis first (structure only).
    pub fn load_chain(&self) -> Result<Vec<SignedRoster>> {
        let mut names: Vec<String> = std::fs::read_dir(self.path("roster/chain"))
            .map_err(Error::io("list the roster chain"))?
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| is_chain_file_name(n))
            .collect();
        if names.len() > MAX_CHAIN {
            return Err(Error::Invalid("roster chain is too long"));
        }
        names.sort();
        let mut out = Vec::with_capacity(names.len());
        for (i, n) in names.iter().enumerate() {
            let s =
                SignedRoster::from_value(&read_json(&self.path(&format!("roster/chain/{n}")))?)?;
            if s.roster.epoch != (i as u64) + 1 || n[..20] != format!("{:020}", s.roster.epoch) {
                return Err(Error::Invalid("roster chain has a gap or a misnamed file"));
            }
            out.push(s);
        }
        Ok(out)
    }

    /// Load, re-verify and rollback-check the current roster against the
    /// roster public key in `hub_keys.json`. Returns `None` before genesis.
    pub fn current_roster(&self) -> Result<Option<Roster>> {
        self.current_roster_with(&self.roster_public_key()?)
    }

    /// As [`DataDir::current_roster`], against a roster public key the
    /// caller already holds (the running server's, opened under the KEK at
    /// startup), so a rewritten `hub_keys.json` cannot change what verifies.
    pub fn current_roster_with(&self, hub: &ed25519_dalek::VerifyingKey) -> Result<Option<Roster>> {
        let chain = self.load_chain()?;
        if chain.is_empty() {
            if self.high_water()?.is_some() {
                return Err(Error::Rejected(
                    "the roster chain is gone but a high-water mark exists",
                ));
            }
            return Ok(None);
        }
        let r = change::verify_chain(&chain, hub)?.clone();
        self.raise_high_water(&r)?;
        Ok(Some(r))
    }

    /// Record a newly accepted roster (already checked by the caller): hub
    /// signature, chain file, high-water mark.
    pub fn accept(
        &self,
        keys: &HubKeys,
        roster: Roster,
        approvals: Vec<Approval>,
        holder_proof: Option<Approval>,
    ) -> Result<SignedRoster> {
        let digest = roster.digest()?;
        let hub_sig = crate::admin::hub_sign(&keys.roster, &digest);
        let s = SignedRoster {
            roster,
            approvals,
            holder_proof,
            hub_sig,
        };
        let p = self.chain_path(s.roster.epoch);
        if p.exists() {
            return Err(Error::Rejected("a roster at this epoch already exists"));
        }
        // Check first, then write the chain file, then raise the mark: a
        // crash between the two writes leaves a chain the next load accepts
        // (and raises the mark for), never a mark above the chain.
        change::check_not_rollback(self.high_water()?, None, &s.roster)?;
        write_json(&p, &s.to_value())?;
        self.raise_high_water(&s.roster)?;
        Ok(s)
    }

    fn pending_path(&self, digest_hex: &str) -> Result<PathBuf> {
        hex::decode_array::<32>(digest_hex)?;
        Ok(self.path(&format!("pending/{digest_hex}.json")))
    }

    /// Store a proposal; returns its digest (hex).
    pub fn save_pending(&self, p: &Pending) -> Result<String> {
        let d = hex::encode(&p.roster.digest()?);
        write_json(&self.pending_path(&d)?, &p.to_value())?;
        Ok(d)
    }

    /// Load a proposal by digest (hex).
    pub fn load_pending(&self, digest_hex: &str) -> Result<Pending> {
        let p = Pending::from_value(&read_json(&self.pending_path(digest_hex)?)?)?;
        if hex::encode(&p.roster.digest()?) != digest_hex {
            return Err(Error::Invalid("a pending file does not match its name"));
        }
        Ok(p)
    }

    /// Remove a proposal.
    pub fn remove_pending(&self, digest_hex: &str) -> Result<()> {
        std::fs::remove_file(self.pending_path(digest_hex)?).map_err(Error::io("remove a proposal"))
    }

    /// The digests of every pending proposal.
    pub fn list_pending(&self) -> Result<Vec<String>> {
        let mut v: Vec<String> = std::fs::read_dir(self.path("pending"))
            .map_err(Error::io("list proposals"))?
            .filter_map(|e| e.ok())
            .filter_map(|e| {
                let n = e.file_name().to_string_lossy().into_owned();
                n.strip_suffix(".json").map(str::to_owned)
            })
            .filter(|n| n.len() == 64 && hex::decode(n).is_ok())
            .collect();
        v.sort();
        Ok(v)
    }

    /// Pair a new LAN client: draws a bearer token, stores only its SHA-256,
    /// and returns the token (shown once).
    pub fn add_bearer(&self) -> Result<Zeroizing<String>> {
        use base64::Engine;
        let raw = Zeroizing::new(rng::array::<32>()?);
        let token =
            Zeroizing::new(base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(&raw[..]));
        let mut hashes = self.bearer_hashes()?;
        if hashes.len() >= MAX_BEARERS {
            return Err(Error::Invalid(
                "the paired-client limit is reached; `pair --reset` unpairs every client",
            ));
        }
        hashes.push(Sha256::digest(token.as_bytes()).into());
        let list: Vec<String> = hashes.iter().map(|h| hex::encode(h)).collect();
        write_json(&self.path("bearers.json"), &json!({"tokens": list}))?;
        Ok(token)
    }

    /// Unpair every LAN client; returns how many were removed, or `None` when
    /// `bearers.json` could not be read (it is rewritten empty either way, so
    /// a damaged pairing file is repaired rather than blocking the reset).
    pub fn reset_bearers(&self) -> Result<Option<usize>> {
        let n = self.bearer_hashes().ok().map(|h| h.len());
        write_json(&self.path("bearers.json"), &json!({"tokens": []}))?;
        Ok(n)
    }

    /// The stored bearer hashes.
    pub fn bearer_hashes(&self) -> Result<Vec<[u8; 32]>> {
        let v = read_json(&self.path("bearers.json"))?;
        v["tokens"]
            .as_array()
            .ok_or(Error::Invalid("bearers.json has no tokens"))?
            .iter()
            .map(|t| hex::decode_array::<32>(t.as_str().unwrap_or("")))
            .collect()
    }

    /// The sealed card store path.
    pub fn cards_path(&self) -> PathBuf {
        self.path("cards.sealed")
    }
}

/// Look up a bearer token among stored hashes in constant time per entry.
/// Returns the session id (the token's SHA-256) when it matches.
pub fn match_bearer(hashes: &[[u8; 32]], token: &str) -> Option<[u8; 32]> {
    let h: [u8; 32] = Sha256::digest(token.as_bytes()).into();
    let mut hit = subtle::Choice::from(0u8);
    for s in hashes {
        hit |= s.ct_eq(&h);
    }
    if bool::from(hit) {
        Some(h)
    } else {
        None
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    pub fn tmpdir(tag: &str) -> PathBuf {
        std::env::temp_dir().join(format!(
            "hk-{tag}-{}",
            hex::encode(&rng::array::<8>().unwrap())
        ))
    }

    pub fn kek() -> Kek {
        Kek::generate().unwrap()
    }

    #[test]
    fn create_open_and_keys() {
        let p = tmpdir("store");
        let k = kek();
        let d = DataDir::create(&p, &k).unwrap();
        assert!(DataDir::create(&p, &k).is_err(), "second create accepted");
        let d2 = DataDir::open(&p).unwrap();
        assert_eq!(d.household().unwrap(), d2.household().unwrap());
        let keys = d.hub_keys(&k).unwrap();
        assert_ne!(keys.posture.to_bytes(), keys.roster.to_bytes());
        assert_eq!(
            keys.posture.verifying_key(),
            d.posture_public_key().unwrap()
        );
        // No plaintext secret key anywhere in the directory.
        for e in std::fs::read_dir(&p).unwrap() {
            let path = e.unwrap().path();
            if path.is_file() {
                let b = std::fs::read(&path).unwrap();
                for secret in [keys.posture.as_bytes(), keys.roster.as_bytes()] {
                    assert!(!b.windows(32).any(|w| w == secret), "{path:?}");
                    let hx = hex::encode(secret);
                    assert!(!String::from_utf8_lossy(&b).contains(&hx), "{path:?}");
                }
            }
        }
        // Another KEK does not open them.
        assert!(d.hub_keys(&kek()).is_err());
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            for f in [
                "hub_keys.sealed",
                "hub_keys.json",
                "household.json",
                "seq.json",
            ] {
                let m = std::fs::metadata(d.path(f)).unwrap().permissions().mode();
                assert_eq!(m & 0o777, 0o600, "{f}");
            }
            let m = std::fs::metadata(&p).unwrap().permissions().mode();
            assert_eq!(m & 0o777, 0o700);
        }
        assert_eq!(d.last_seq().unwrap(), 0);
        d.save_seq(77).unwrap();
        assert_eq!(d.last_seq().unwrap(), 77);
        std::fs::remove_dir_all(&p).unwrap();
    }

    #[test]
    fn swapped_public_keys_are_refused() {
        let p = tmpdir("swap");
        let k = kek();
        let d = DataDir::create(&p, &k).unwrap();
        let other = HubKeys::generate().unwrap();
        write_json(
            &d.path("hub_keys.json"),
            &json!({
                "posture_public_key": hex::encode(&other.posture.verifying_key().to_bytes()),
                "roster_public_key": hex::encode(&other.roster.verifying_key().to_bytes()),
            }),
        )
        .unwrap();
        assert!(d.hub_keys(&k).is_err(), "mismatched public record accepted");
        std::fs::remove_dir_all(&p).unwrap();
    }

    #[test]
    fn bearer_list_is_capped_and_resettable() {
        let p = tmpdir("bearer-cap");
        let d = DataDir::create(&p, &kek()).unwrap();
        for _ in 0..MAX_BEARERS {
            d.add_bearer().unwrap();
        }
        assert!(d.add_bearer().is_err(), "the cap refuses, it never evicts");
        assert_eq!(d.bearer_hashes().unwrap().len(), MAX_BEARERS);
        assert_eq!(d.reset_bearers().unwrap(), Some(MAX_BEARERS));
        // A damaged pairing file does not block the reset; it is repaired.
        std::fs::write(d.path("bearers.json"), b"{not json").unwrap();
        assert!(d.bearer_hashes().is_err());
        assert_eq!(d.reset_bearers().unwrap(), None);
        assert!(d.bearer_hashes().unwrap().is_empty());
        assert!(d.bearer_hashes().unwrap().is_empty());
        assert!(d.add_bearer().is_ok());
        std::fs::remove_dir_all(&p).unwrap();
    }

    #[test]
    fn bearer_tokens_are_stored_hashed() {
        let p = tmpdir("bearer");
        let d = DataDir::create(&p, &kek()).unwrap();
        let t = d.add_bearer().unwrap();
        let file = std::fs::read_to_string(d.path("bearers.json")).unwrap();
        assert!(!file.contains(t.as_str()));
        let hashes = d.bearer_hashes().unwrap();
        assert!(match_bearer(&hashes, &t).is_some());
        assert!(match_bearer(&hashes, "nope").is_none());
        std::fs::remove_dir_all(&p).unwrap();
    }

    #[test]
    fn bounded_reads() {
        let p = tmpdir("bounded");
        std::fs::create_dir_all(&p).unwrap();
        let f = p.join("x");
        std::fs::write(&f, vec![b'a'; 100]).unwrap();
        assert!(read_bounded(&f, 99).is_err());
        assert_eq!(read_bounded(&f, 100).unwrap().len(), 100);
        std::fs::remove_dir_all(&p).unwrap();
    }

    #[test]
    fn deleting_the_newest_chain_file_is_a_refused_rollback() {
        use crate::admin::sign_cli;
        use crate::roster::tests::{ed_admin, phone};
        use crate::roster::CredentialClass;
        let p = tmpdir("rollback");
        let k = kek();
        let d = DataDir::create(&p, &k).unwrap();
        let hub = d.hub_keys(&k).unwrap();
        let key = ed25519_dalek::SigningKey::from_bytes(&[1; 32]);
        let g = Roster::genesis(d.household().unwrap(), vec![ed_admin(1, 1)], 1).unwrap();
        let a = vec![sign_cli(&key, &[1; 16], &g.digest().unwrap())];
        d.accept(&hub, g.clone(), a, None).unwrap();
        let mut n = g.successor().unwrap();
        n.members.push(phone(0x10, 0x31, CredentialClass::Intent));
        let a = vec![sign_cli(&key, &[1; 16], &n.digest().unwrap())];
        d.accept(&hub, n.clone(), a, None).unwrap();
        assert_eq!(d.current_roster().unwrap().unwrap(), n);
        assert_eq!(d.high_water().unwrap().unwrap().epoch, 2);
        // Re-accepting epoch 1 is refused.
        let a = vec![sign_cli(&key, &[1; 16], &g.digest().unwrap())];
        assert!(d.accept(&hub, g, a, None).is_err());
        // Restoring an older chain (deleting epoch 2) is refused at load.
        std::fs::remove_file(d.chain_path(2)).unwrap();
        assert!(d.current_roster().is_err(), "rolled-back chain loaded");
        // So is deleting the whole chain.
        std::fs::remove_file(d.chain_path(1)).unwrap();
        assert!(
            d.current_roster().is_err(),
            "empty chain with a high-water mark loaded"
        );
        std::fs::remove_dir_all(&p).unwrap();
    }

    #[test]
    fn a_tampered_chain_file_is_refused() {
        use crate::admin::sign_cli;
        use crate::roster::tests::ed_admin;
        let p = tmpdir("tamper");
        let k = kek();
        let d = DataDir::create(&p, &k).unwrap();
        let hub = d.hub_keys(&k).unwrap();
        let key = ed25519_dalek::SigningKey::from_bytes(&[1; 32]);
        let g = Roster::genesis(d.household().unwrap(), vec![ed_admin(1, 1)], 1).unwrap();
        let a = vec![sign_cli(&key, &[1; 16], &g.digest().unwrap())];
        d.accept(&hub, g, a, None).unwrap();
        let path = d.chain_path(1);
        let text = std::fs::read_to_string(&path).unwrap();
        // Add insignificant whitespace to the stored JSON.
        let edited = text.replacen("\"threshold\": 1", "\"threshold\": 1 ", 1);
        assert_eq!(edited.len(), text.len() + 1, "fixture did not match");
        // Whitespace alone changes nothing (the digest is over canonical bytes)...
        std::fs::write(&path, &edited).unwrap();
        assert!(d.current_roster().is_ok());
        // ...but a different admin key does.
        let other = ed_admin(1, 2);
        let mut v: Value = serde_json::from_str(&text).unwrap();
        v["roster"]["admins"][0]["public_key"] = json!(hex::encode(&other.public_key));
        std::fs::write(&path, serde_json::to_vec(&v).unwrap()).unwrap();
        assert!(d.current_roster().is_err(), "tampered chain loaded");
        std::fs::remove_dir_all(&p).unwrap();
    }

    #[test]
    fn atomic_write_leaves_no_temp_files() {
        let p = tmpdir("atomic");
        std::fs::create_dir_all(&p).unwrap();
        let f = p.join("seam.bin");
        write_atomic(&f, b"one").unwrap();
        write_atomic(&f, b"two").unwrap();
        assert_eq!(std::fs::read(&f).unwrap(), b"two");
        let n = std::fs::read_dir(&p).unwrap().count();
        assert_eq!(n, 1);
        std::fs::remove_dir_all(&p).unwrap();
    }
}
