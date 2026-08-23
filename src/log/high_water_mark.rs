//! Signed external high-water-mark for the sealed log.
//!
//! The interior hash chain proves that no event was edited, reordered, or
//! deleted *mid-log*, but it cannot bind its own **length** or **head**:
//! lopping off the newest events, restoring an older whole-DB snapshot, or
//! wiping the log all leave an internally-consistent (shorter or older) chain
//! that still verifies. This module remembers, in a small device-signed record
//! kept **outside** the database, the furthest `(seq, head)` the log ever
//! reached, and [`crate::verify_runner`] fails closed when the live log is
//! behind it. Closes tail-truncation, whole-file rollback, and the wiped-log
//! case (`docs/security/ENTERPRISE_CUSTODY.md` §2).
//!
//! ## Honest scope
//!
//! The mark is signed by the **device key**, so an actor who holds that key
//! forges both the log and the mark — that is the key / host-compromise case
//! the kernel's threat model deliberately scopes out (`spec/threat_model.md`
//! §2.6). What the mark closes is the *no-key* rollback / truncation / wipe: an
//! actor who restores an old snapshot or truncates the DB but does **not** have
//! the current signing key cannot produce a matching-or-advanced mark, so
//! verify fails closed.
//!
//! One residual: if the mark file is restored *together with* an old DB
//! snapshot (both rolled back in lockstep), a same-directory file rolls back
//! too. The operator points `SECURACV_HWM_PATH` at append-only or external
//! media so the two cannot be rolled back as a unit; the transparency-witness
//! end-state (an RFC 9162 log with witness cosigning, TSA-anchored) is
//! specified in `spec/quorum_unseal_v2.md` §4. The mark's own writer also
//! refuses to move backward, so a regression attempt on the file is itself a
//! signal rather than a silent overwrite.

use anyhow::{anyhow, bail, Context, Result};
use ed25519_dalek::VerifyingKey;
use rusqlite::{Connection, OptionalExtension};
use sha2::{Digest, Sha256};
use std::io::Write;
use std::path::Path;

use crate::crypto::signatures::{
    verify_with_domain, PqPublicKey, SignatureKeys, SignatureMode, SignatureSet,
};

/// Domain-separation tag for the high-water-mark signature. Distinct from the
/// sealed-log-entry and checkpoint domains so a mark signature can never be
/// replayed as an entry or checkpoint signature (and vice versa).
pub const DOMAIN_HIGH_WATER_MARK: &str = "securacv:pwk:sealed-log-high-water:v1";

/// On-disk container magic. The trailing digits are the container version; a
/// bump here is a wire-format change, independent of the struct version below.
const HWM_MAGIC: &[u8; 8] = b"SCVHWM01";
/// Struct version covered by the signature. Lets the signed body evolve
/// without a magic bump.
const HWM_STRUCT_VERSION: u16 = 1;

/// Fixed prefix length: magic(8) + struct_version(2) + seq(8) + head(32)
/// + signed_at_bucket(8) + signer_public_key(32) + ed25519_sig(64).
const FIXED_PREFIX_LEN: usize = 8 + 2 + 8 + 32 + 8 + 32 + 64;

/// A decoded, signature-bearing high-water-mark.
#[derive(Clone, Debug)]
pub struct HighWaterMark {
    /// Sequence marker: the id (AUTOINCREMENT primary key) of the newest
    /// sealed event when the mark was written — equivalently, the total number
    /// of events ever appended. **Monotone under retention pruning**, which
    /// deletes the *oldest* rows and never lowers the newest id, so a drop in
    /// this value signals truncation, rollback, or a wipe rather than normal
    /// pruning.
    pub seq: u64,
    /// `entry_hash` of that newest sealed event — the live chain head at the
    /// mark.
    pub head_hash: [u8; 32],
    /// Coarse 10-minute bucket start at which the mark was signed. Bucketed,
    /// never precise, to honor the invariants' metadata-minimization rule
    /// (the mark can flow to an operator or an external witness).
    pub signed_at_bucket: u64,
    /// The device verifying key that signed this mark. The verifier requires
    /// this to be a genesis-anchored lineage key (mirroring the checkpoint
    /// signer check) *before* trusting the signature.
    pub signer_public_key: [u8; 32],
    signatures: SignatureSet,
}

impl HighWaterMark {
    /// The 32-byte body hash the signature covers: a SHA-256 over every signed
    /// field (magic and versions included, so a container/struct downgrade
    /// cannot reuse a signature).
    fn body_hash(
        seq: u64,
        head_hash: &[u8; 32],
        signed_at_bucket: u64,
        signer_public_key: &[u8; 32],
    ) -> [u8; 32] {
        let mut h = Sha256::new();
        h.update(HWM_MAGIC);
        h.update(HWM_STRUCT_VERSION.to_le_bytes());
        h.update(seq.to_le_bytes());
        h.update(head_hash);
        h.update(signed_at_bucket.to_le_bytes());
        h.update(signer_public_key);
        h.finalize().into()
    }

    /// Sign a new mark with the device key set. Both the ed25519 and (when
    /// configured) the post-quantum signature are produced, so the mark
    /// verifies under `SignatureMode::Strict` on a hybrid device.
    pub fn sign(
        seq: u64,
        head_hash: [u8; 32],
        signed_at_bucket: u64,
        keys: &SignatureKeys<'_>,
    ) -> Result<Self> {
        let signer_public_key = keys.ed25519.verifying_key().to_bytes();
        let body = Self::body_hash(seq, &head_hash, signed_at_bucket, &signer_public_key);
        let signatures = super::sign_entry(keys, &body, DOMAIN_HIGH_WATER_MARK)?;
        Ok(Self {
            seq,
            head_hash,
            signed_at_bucket,
            signer_public_key,
            signatures,
        })
    }

    /// Verify the mark's signature against a trusted verifying key. The caller
    /// is responsible for having established that `trusted_key` is a
    /// genesis-anchored lineage key; this method additionally binds the
    /// signature to the `signer_public_key` embedded in (and hashed into) the
    /// mark, so a mark cannot claim one signer while carrying another's key.
    pub fn verify_signature(
        &self,
        trusted_key: &VerifyingKey,
        mode: SignatureMode,
        pq_public_key: Option<&PqPublicKey>,
    ) -> Result<()> {
        if trusted_key.to_bytes() != self.signer_public_key {
            bail!("high-water-mark signer key does not match the trusted verifying key");
        }
        let body = Self::body_hash(
            self.seq,
            &self.head_hash,
            self.signed_at_bucket,
            &self.signer_public_key,
        );
        verify_with_domain(
            DOMAIN_HIGH_WATER_MARK,
            trusted_key,
            &body,
            &self.signatures,
            mode,
            pq_public_key,
        )
    }

    /// Serialize to the on-disk container form.
    pub fn encode(&self) -> Vec<u8> {
        let ed_sig = &self.signatures.ed25519_signature;
        let pq_scheme = self.signatures.pq_scheme_id().unwrap_or("");
        let pq_sig = self.signatures.pq_signature_bytes().unwrap_or(&[]);

        let mut out = Vec::with_capacity(FIXED_PREFIX_LEN + 6 + pq_scheme.len() + pq_sig.len());
        out.extend_from_slice(HWM_MAGIC);
        out.extend_from_slice(&HWM_STRUCT_VERSION.to_le_bytes());
        out.extend_from_slice(&self.seq.to_le_bytes());
        out.extend_from_slice(&self.head_hash);
        out.extend_from_slice(&self.signed_at_bucket.to_le_bytes());
        out.extend_from_slice(&self.signer_public_key);
        // ed25519 signature is fixed 64 bytes (SignatureSet guarantees length
        // on the sign path; assert on decode).
        out.extend_from_slice(ed_sig);
        out.extend_from_slice(&(pq_scheme.len() as u16).to_le_bytes());
        out.extend_from_slice(pq_scheme.as_bytes());
        out.extend_from_slice(&(pq_sig.len() as u32).to_le_bytes());
        out.extend_from_slice(pq_sig);
        out
    }

    /// Parse a container produced by [`Self::encode`]. Rejects a wrong magic,
    /// an unknown struct version, or a truncated/oversized field before any
    /// cryptographic work — a corrupt mark is a hard error, never a silent
    /// "no mark".
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        if bytes.len() < FIXED_PREFIX_LEN {
            bail!("high-water-mark container too short");
        }
        if &bytes[0..8] != HWM_MAGIC {
            bail!("high-water-mark container: bad magic");
        }
        let mut off = 8;
        let struct_version = u16::from_le_bytes([bytes[off], bytes[off + 1]]);
        off += 2;
        if struct_version != HWM_STRUCT_VERSION {
            bail!("high-water-mark container: unsupported struct version {struct_version}");
        }
        let seq = u64::from_le_bytes(bytes[off..off + 8].try_into().unwrap());
        off += 8;
        let mut head_hash = [0u8; 32];
        head_hash.copy_from_slice(&bytes[off..off + 32]);
        off += 32;
        let signed_at_bucket = u64::from_le_bytes(bytes[off..off + 8].try_into().unwrap());
        off += 8;
        let mut signer_public_key = [0u8; 32];
        signer_public_key.copy_from_slice(&bytes[off..off + 32]);
        off += 32;
        let mut ed_sig = [0u8; 64];
        ed_sig.copy_from_slice(&bytes[off..off + 64]);
        off += 64;

        let pq_scheme_len = read_u16(bytes, &mut off)? as usize;
        let pq_scheme = read_bytes(bytes, &mut off, pq_scheme_len)?;
        let pq_sig_len = read_u32(bytes, &mut off)? as usize;
        let pq_sig = read_bytes(bytes, &mut off, pq_sig_len)?;
        if off != bytes.len() {
            bail!(
                "high-water-mark container: {} trailing bytes",
                bytes.len() - off
            );
        }

        let pq_signature = if pq_scheme_len == 0 && pq_sig_len == 0 {
            None
        } else if pq_scheme_len == 0 || pq_sig_len == 0 {
            bail!("high-water-mark container: partial post-quantum signature");
        } else {
            Some(pq_sig.to_vec())
        };
        let pq_scheme = if pq_scheme.is_empty() {
            None
        } else {
            Some(
                std::str::from_utf8(pq_scheme)
                    .context("high-water-mark container: pq scheme not UTF-8")?
                    .to_string(),
            )
        };
        let signatures = SignatureSet::from_storage(&ed_sig, pq_signature, pq_scheme)?;

        Ok(Self {
            seq,
            head_hash,
            signed_at_bucket,
            signer_public_key,
            signatures,
        })
    }
}

fn read_u16(bytes: &[u8], off: &mut usize) -> Result<u16> {
    let end = off
        .checked_add(2)
        .ok_or_else(|| anyhow!("length overflow"))?;
    if end > bytes.len() {
        bail!("high-water-mark container truncated");
    }
    let v = u16::from_le_bytes([bytes[*off], bytes[*off + 1]]);
    *off = end;
    Ok(v)
}

fn read_u32(bytes: &[u8], off: &mut usize) -> Result<u32> {
    let end = off
        .checked_add(4)
        .ok_or_else(|| anyhow!("length overflow"))?;
    if end > bytes.len() {
        bail!("high-water-mark container truncated");
    }
    let v = u32::from_le_bytes(bytes[*off..end].try_into().unwrap());
    *off = end;
    Ok(v)
}

fn read_bytes<'a>(bytes: &'a [u8], off: &mut usize, len: usize) -> Result<&'a [u8]> {
    let end = off
        .checked_add(len)
        .ok_or_else(|| anyhow!("length overflow"))?;
    if end > bytes.len() {
        bail!("high-water-mark container truncated");
    }
    let slice = &bytes[*off..end];
    *off = end;
    Ok(slice)
}

/// The live high-water position of the sealed log: the newest event id ever
/// assigned (`seq`) and the `entry_hash` of that newest row (`head`).
///
/// `seq` is read from `sqlite_sequence` when available (which retains the
/// highest id even after every row is deleted, so a `DELETE FROM sealed_events`
/// wipe still reports the pre-wipe high) *and* from `MAX(id)`, taking the
/// larger. `head` is the `entry_hash` of the `MAX(id)` row, or `None` when the
/// table currently holds no rows.
pub fn read_live_marker(conn: &Connection) -> Result<(u64, Option<[u8; 32]>)> {
    // sqlite_sequence only exists once an AUTOINCREMENT table has taken a row;
    // its absence is not an error, just "no recorded sequence yet".
    let has_seq_table: bool = conn
        .query_row(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='sqlite_sequence'",
            [],
            |_| Ok(true),
        )
        .optional()?
        .unwrap_or(false);
    let seq_from_sequence: i64 = if has_seq_table {
        conn.query_row(
            "SELECT seq FROM sqlite_sequence WHERE name='sealed_events'",
            [],
            |r| r.get(0),
        )
        .optional()?
        .unwrap_or(0)
    } else {
        0
    };

    let max_id: Option<i64> =
        conn.query_row("SELECT MAX(id) FROM sealed_events", [], |r| r.get(0))?;
    let seq = seq_from_sequence.max(max_id.unwrap_or(0)).max(0) as u64;

    let head = match max_id {
        Some(id) => {
            let bytes: Vec<u8> = conn.query_row(
                "SELECT entry_hash FROM sealed_events WHERE id = ?1",
                [id],
                |r| r.get(0),
            )?;
            if bytes.len() != 32 {
                bail!("corrupt sealed log: entry_hash size at newest row");
            }
            let mut out = [0u8; 32];
            out.copy_from_slice(&bytes);
            Some(out)
        }
        None => None,
    };
    Ok((seq, head))
}

/// Load a mark from disk. A missing file is `Ok(None)` (the log has simply not
/// been marked yet); a present-but-corrupt file is an error.
pub fn load(path: &Path) -> Result<Option<HighWaterMark>> {
    match std::fs::read(path) {
        Ok(bytes) => Ok(Some(HighWaterMark::decode(&bytes).with_context(|| {
            format!("decoding high-water-mark at {}", path.display())
        })?)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(anyhow!(
            "reading high-water-mark at {}: {e}",
            path.display()
        )),
    }
}

/// Advance the on-disk mark to `(seq, head)`, signing with the device key.
///
/// **Monotone by construction:** if a mark already exists with a *higher* seq,
/// this refuses (a regression is a signal, not something to overwrite); at an
/// *equal* seq it verifies the head matches and is otherwise a no-op (no
/// rewrite, so no needless fsync on the hot append path). Only a strictly
/// higher seq writes a new signed container.
pub fn advance(
    path: &Path,
    keys: &SignatureKeys<'_>,
    seq: u64,
    head_hash: [u8; 32],
) -> Result<HighWaterMark> {
    if let Some(existing) = load(path)? {
        if seq < existing.seq {
            bail!(
                "refusing to regress sealed-log high-water-mark from seq {} to {}",
                existing.seq,
                seq
            );
        }
        if seq == existing.seq {
            if head_hash != existing.head_hash {
                bail!(
                    "sealed-log high-water-mark head fork at seq {}: stored {} vs new {}",
                    seq,
                    hex::encode(existing.head_hash),
                    hex::encode(head_hash)
                );
            }
            return Ok(existing);
        }
    }
    let signed_at_bucket = crate::TimeBucket::now_10min()?.start_epoch_s;
    let mark = HighWaterMark::sign(seq, head_hash, signed_at_bucket, keys)?;
    write_atomic(path, &mark.encode())?;
    Ok(mark)
}

/// Atomic, owner-only write (temp file + fsync + rename). Mirrors the vault's
/// container writer so a crash mid-write can never leave a torn mark. The temp
/// name carries the writer's PID so two processes sharing the DB (witnessd plus
/// a bridge) never clobber each other's temp file — each renames its own into
/// place, and the rename is atomic, so the mark is always a whole container.
fn write_atomic(path: &Path, data: &[u8]) -> Result<()> {
    let tmp_path = path.with_extension(format!("hwm-tmp.{}", std::process::id()));
    {
        #[cfg(unix)]
        let mut file = {
            use std::os::unix::fs::OpenOptionsExt;
            let mut options = std::fs::OpenOptions::new();
            options.write(true).create(true).truncate(true).mode(0o600);
            options.open(&tmp_path)?
        };
        #[cfg(not(unix))]
        let mut file = std::fs::File::create(&tmp_path)?;

        file.write_all(data)?;
        file.sync_all()?;
    }
    std::fs::rename(&tmp_path, path)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::crypto::signatures::SignatureKeys;
    use ed25519_dalek::SigningKey;
    use rusqlite::params;

    fn keys(seed: u8) -> SigningKey {
        SigningKey::from_bytes(&[seed; 32])
    }

    fn sig_keys(sk: &SigningKey) -> SignatureKeys<'_> {
        SignatureKeys::new(sk)
    }

    #[test]
    fn encode_decode_round_trips() {
        let sk = keys(1);
        let mark = HighWaterMark::sign(42, [9u8; 32], 1_700_000_400, &sig_keys(&sk)).unwrap();
        let bytes = mark.encode();
        let back = HighWaterMark::decode(&bytes).unwrap();
        assert_eq!(back.seq, 42);
        assert_eq!(back.head_hash, [9u8; 32]);
        assert_eq!(back.signed_at_bucket, 1_700_000_400);
        assert_eq!(back.signer_public_key, sk.verifying_key().to_bytes());
        // The decoded mark verifies under the original signer.
        back.verify_signature(&sk.verifying_key(), SignatureMode::Compat, None)
            .unwrap();
    }

    #[test]
    fn signature_rejects_wrong_key_and_tamper() {
        let sk = keys(2);
        let other = keys(3);
        let mark = HighWaterMark::sign(7, [1u8; 32], 600, &sig_keys(&sk)).unwrap();

        // Wrong trusted key: the embedded-signer binding fails before any
        // curve math.
        assert!(mark
            .verify_signature(&other.verifying_key(), SignatureMode::Compat, None)
            .is_err());

        // Flip a byte in the signed body (seq) and the signature no longer
        // covers it.
        let mut bytes = mark.encode();
        bytes[10] ^= 0xff; // within the seq field
        let tampered = HighWaterMark::decode(&bytes).unwrap();
        assert!(tampered
            .verify_signature(&sk.verifying_key(), SignatureMode::Compat, None)
            .is_err());
    }

    #[test]
    fn decode_rejects_bad_magic_and_trailing_bytes() {
        let sk = keys(4);
        let mark = HighWaterMark::sign(1, [2u8; 32], 600, &sig_keys(&sk)).unwrap();
        let mut bytes = mark.encode();

        let mut bad_magic = bytes.clone();
        bad_magic[0] = b'X';
        assert!(HighWaterMark::decode(&bad_magic).is_err());

        bytes.push(0u8); // trailing byte
        assert!(HighWaterMark::decode(&bytes).is_err());

        assert!(HighWaterMark::decode(&[]).is_err());
    }

    #[test]
    fn advance_is_monotonic() {
        let dir = std::env::temp_dir();
        let path = dir.join(format!("hwm_test_{}.bin", rand_suffix()));
        let _ = std::fs::remove_file(&path);
        let sk = keys(5);
        let k = sig_keys(&sk);

        let m1 = advance(&path, &k, 1, [1u8; 32]).unwrap();
        assert_eq!(m1.seq, 1);
        let m2 = advance(&path, &k, 2, [2u8; 32]).unwrap();
        assert_eq!(m2.seq, 2);

        // Regression is refused.
        assert!(advance(&path, &k, 1, [1u8; 32]).is_err());

        // Same seq + same head is an idempotent no-op.
        let again = advance(&path, &k, 2, [2u8; 32]).unwrap();
        assert_eq!(again.seq, 2);
        assert_eq!(again.head_hash, [2u8; 32]);

        // Same seq + a *different* head is a fork error.
        assert!(advance(&path, &k, 2, [9u8; 32]).is_err());

        // The persisted mark is the latest good one.
        let loaded = load(&path).unwrap().unwrap();
        assert_eq!(loaded.seq, 2);
        assert_eq!(loaded.head_hash, [2u8; 32]);

        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn load_missing_is_none() {
        let path = std::env::temp_dir().join(format!("hwm_absent_{}.bin", rand_suffix()));
        let _ = std::fs::remove_file(&path);
        assert!(load(&path).unwrap().is_none());
    }

    fn mem_db_with_events(n: i64) -> Connection {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(
            "CREATE TABLE sealed_events (id INTEGER PRIMARY KEY AUTOINCREMENT, entry_hash BLOB NOT NULL);",
        )
        .unwrap();
        for i in 1..=n {
            conn.execute(
                "INSERT INTO sealed_events(entry_hash) VALUES (?1)",
                params![vec![i as u8; 32]],
            )
            .unwrap();
        }
        conn
    }

    #[test]
    fn read_live_marker_tracks_seq_and_head() {
        let conn = mem_db_with_events(3);
        let (seq, head) = read_live_marker(&conn).unwrap();
        assert_eq!(seq, 3);
        assert_eq!(head, Some([3u8; 32]));

        // Delete the newest row (tail truncation): MAX(id) drops to 2, but
        // sqlite_sequence retains 3, so seq stays at the high-water while the
        // head moves back — exactly the mismatch verify keys on.
        conn.execute("DELETE FROM sealed_events WHERE id = 3", [])
            .unwrap();
        let (seq, head) = read_live_marker(&conn).unwrap();
        assert_eq!(
            seq, 3,
            "sqlite_sequence retains the high-water after a tail delete"
        );
        assert_eq!(head, Some([2u8; 32]));

        // Wipe everything: seq still reads the retained high-water, head is gone.
        conn.execute("DELETE FROM sealed_events", []).unwrap();
        let (seq, head) = read_live_marker(&conn).unwrap();
        assert_eq!(seq, 3);
        assert_eq!(head, None);
    }

    // A tiny non-crypto suffix source for temp-file names (tests only).
    fn rand_suffix() -> u64 {
        use std::time::{SystemTime, UNIX_EPOCH};
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos() as u64
    }
}
