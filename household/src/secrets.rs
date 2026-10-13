//! Card keys, sealed.
//!
//! Card keys (and only card keys) are secret: they are clone-capable
//! material. They live in the checker's data directory in one file, sealed
//! with XChaCha20-Poly1305 under a key derived from a key-encryption key
//! (KEK) that is **not** in that directory: [`load_kek`] refuses a KEK path
//! inside it. Plaintext keys live only in [`Zeroizing`] buffers.
//!
//! File format (`cards.sealed`):
//!
//! ```text
//! "SCVK" ‖ 0x01 ‖ nonce(24) ‖ XChaCha20-Poly1305(k, nonce, aad, plaintext)
//! k         = HKDF-SHA256(ikm = KEK, salt = household, info = "securacv-hk-cards-v1")
//! aad       = "SCVK" ‖ 0x01 ‖ household
//! plaintext = be16(count) ‖ (handle(16) ‖ key(16))*
//! ```
//!
//! A hub compromise means re-keying every card. Exclude the data directory
//! from backups; the KEK is the only thing that opens the file.

use crate::roster::Handle;
use crate::{rng, Error, Result};
use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::XChaCha20Poly1305;
use hkdf::Hkdf;
use sha2::Sha256;
use std::collections::BTreeMap;
use std::path::Path;
use zeroize::Zeroizing;

const MAGIC: &[u8; 4] = b"SCVK";
const VERSION: u8 = 1;
const INFO: &[u8] = b"securacv-hk-cards-v1";
/// Largest sealed file accepted.
pub const MAX_SEALED_BYTES: usize = 64 * 1024;

/// The key-encryption key.
pub struct Kek(Zeroizing<[u8; 32]>);

impl std::fmt::Debug for Kek {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Kek(<redacted>)")
    }
}

impl Kek {
    /// Wrap raw KEK bytes.
    pub fn from_bytes(b: [u8; 32]) -> Kek {
        Kek(Zeroizing::new(b))
    }

    /// A fresh random KEK.
    pub fn generate() -> Result<Kek> {
        Ok(Kek(Zeroizing::new(rng::array()?)))
    }

    /// The KEK as 64 lowercase hex characters (for writing the KEK file).
    pub fn to_hex(&self) -> Zeroizing<String> {
        Zeroizing::new(crate::hex::encode(&self.0[..]))
    }

    fn aead(&self, household: &[u8; 16]) -> XChaCha20Poly1305 {
        let hk = Hkdf::<Sha256>::new(Some(household), &self.0[..]);
        let mut k = Zeroizing::new([0u8; 32]);
        hk.expand(INFO, &mut k[..])
            .expect("32 bytes is a valid HKDF length");
        XChaCha20Poly1305::new_from_slice(&k[..]).expect("32-byte key")
    }
}

/// Whether `path` lies inside `dir` (after resolving both).
pub fn is_inside(path: &Path, dir: &Path) -> Result<bool> {
    let d = dir
        .canonicalize()
        .map_err(Error::io("resolve the data directory"))?;
    let p = match path.canonicalize() {
        Ok(p) => p,
        Err(_) => {
            // Not created yet: resolve its parent.
            let parent = path
                .parent()
                .ok_or(Error::Invalid("KEK path has no parent"))?;
            let parent = if parent.as_os_str().is_empty() {
                Path::new(".")
            } else {
                parent
            };
            parent
                .canonicalize()
                .map_err(Error::io("resolve the KEK directory"))?
                .join(
                    path.file_name()
                        .ok_or(Error::Invalid("KEK path has no file name"))?,
                )
        }
    };
    Ok(p.starts_with(&d))
}

/// Read the KEK file (64 hex characters, optional trailing newline). Refuses
/// a KEK inside `data_dir`, and on Unix one readable by group or others.
pub fn load_kek(path: &Path, data_dir: &Path) -> Result<Kek> {
    if is_inside(path, data_dir)? {
        return Err(Error::Invalid(
            "the KEK must not live inside the data directory",
        ));
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let meta = std::fs::metadata(path).map_err(Error::io("stat the KEK file"))?;
        if meta.permissions().mode() & 0o077 != 0 {
            return Err(Error::Invalid(
                "the KEK file must not be readable by group or others",
            ));
        }
    }
    let raw = Zeroizing::new(std::fs::read(path).map_err(Error::io("read the KEK file"))?);
    let text = std::str::from_utf8(&raw).map_err(|_| Error::Invalid("KEK file is not text"))?;
    let bytes = Zeroizing::new(crate::hex::decode_array::<32>(text.trim_end())?);
    Ok(Kek::from_bytes(*bytes))
}

/// The card keys, by member handle.
#[derive(Default)]
pub struct CardStore {
    keys: BTreeMap<Handle, Zeroizing<[u8; 16]>>,
}

impl std::fmt::Debug for CardStore {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "CardStore({} keys)", self.keys.len())
    }
}

impl CardStore {
    /// An empty store.
    pub fn new() -> Self {
        Self::default()
    }

    /// Insert or replace a card key.
    pub fn insert(&mut self, handle: Handle, key: [u8; 16]) {
        self.keys.insert(handle, Zeroizing::new(key));
    }

    /// Remove a card key (after a revoke).
    pub fn remove(&mut self, handle: &Handle) -> bool {
        self.keys.remove(handle).is_some()
    }

    /// Whether a key exists for `handle`.
    pub fn contains(&self, handle: &Handle) -> bool {
        self.keys.contains_key(handle)
    }

    /// Number of keys.
    pub fn len(&self) -> usize {
        self.keys.len()
    }

    /// Whether the store is empty.
    pub fn is_empty(&self) -> bool {
        self.keys.is_empty()
    }

    /// Copies of the keys for `handles` (those present), for one
    /// authentication attempt.
    pub fn keys_for(&self, handles: &[Handle]) -> Vec<(Handle, Zeroizing<[u8; 16]>)> {
        handles
            .iter()
            .filter_map(|h| self.keys.get(h).map(|k| (*h, Zeroizing::new(**k))))
            .collect()
    }

    /// Seal under `kek` for `household`.
    pub fn seal(&self, kek: &Kek, household: &[u8; 16]) -> Result<Vec<u8>> {
        let mut pt = Zeroizing::new(Vec::with_capacity(2 + 32 * self.keys.len()));
        pt.extend_from_slice(&(self.keys.len() as u16).to_be_bytes());
        for (h, k) in &self.keys {
            pt.extend_from_slice(h);
            pt.extend_from_slice(&k[..]);
        }
        let nonce: [u8; 24] = rng::array()?;
        let mut aad = MAGIC.to_vec();
        aad.push(VERSION);
        aad.extend_from_slice(household);
        let ct = kek
            .aead(household)
            .encrypt(
                (&nonce).into(),
                Payload {
                    msg: &pt[..],
                    aad: &aad,
                },
            )
            .map_err(|_| Error::Invalid("sealing failed"))?;
        let mut out = MAGIC.to_vec();
        out.push(VERSION);
        out.extend_from_slice(&nonce);
        out.extend_from_slice(&ct);
        Ok(out)
    }

    /// Open a sealed store.
    pub fn open(sealed: &[u8], kek: &Kek, household: &[u8; 16]) -> Result<CardStore> {
        if sealed.len() < 5 + 24 + 16 || sealed.len() > MAX_SEALED_BYTES {
            return Err(Error::Invalid("sealed card store has the wrong size"));
        }
        if &sealed[..4] != MAGIC || sealed[4] != VERSION {
            return Err(Error::Invalid("not a sealed card store"));
        }
        let mut nonce = [0u8; 24];
        nonce.copy_from_slice(&sealed[5..29]);
        let mut aad = MAGIC.to_vec();
        aad.push(VERSION);
        aad.extend_from_slice(household);
        let pt = Zeroizing::new(
            kek.aead(household)
                .decrypt(
                    (&nonce).into(),
                    Payload {
                        msg: &sealed[29..],
                        aad: &aad,
                    },
                )
                .map_err(|_| Error::Rejected("the card store does not open under this KEK"))?,
        );
        if pt.len() < 2 {
            return Err(Error::Invalid("card store plaintext is short"));
        }
        let n = u16::from_be_bytes([pt[0], pt[1]]) as usize;
        if pt.len() != 2 + 32 * n {
            return Err(Error::Invalid("card store plaintext has the wrong length"));
        }
        let mut store = CardStore::new();
        for rec in pt[2..].as_chunks::<32>().0 {
            let mut h = [0u8; 16];
            h.copy_from_slice(&rec[..16]);
            let mut k = Zeroizing::new([0u8; 16]);
            k.copy_from_slice(&rec[16..]);
            store.keys.insert(h, k);
        }
        Ok(store)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const HH: [u8; 16] = [0x42; 16];

    #[test]
    fn seal_open_round_trip() {
        let kek = Kek::from_bytes([3; 32]);
        let mut s = CardStore::new();
        s.insert([1; 16], [0xa1; 16]);
        s.insert([2; 16], [0xa2; 16]);
        let sealed = s.seal(&kek, &HH).unwrap();
        // No key byte pattern appears in the sealed file.
        assert!(!sealed
            .windows(16)
            .any(|w| w == [0xa1; 16] || w == [0xa2; 16]));
        let o = CardStore::open(&sealed, &kek, &HH).unwrap();
        assert_eq!(o.len(), 2);
        let ks = o.keys_for(&[[2; 16], [9; 16]]);
        assert_eq!(ks.len(), 1);
        assert_eq!(*ks[0].1, [0xa2; 16]);
    }

    #[test]
    fn wrong_kek_household_or_tamper_fails() {
        let kek = Kek::from_bytes([3; 32]);
        let mut s = CardStore::new();
        s.insert([1; 16], [0xa1; 16]);
        let sealed = s.seal(&kek, &HH).unwrap();
        assert!(CardStore::open(&sealed, &Kek::from_bytes([4; 32]), &HH).is_err());
        assert!(CardStore::open(&sealed, &kek, &[0x43; 16]).is_err());
        let mut t = sealed.clone();
        let last = t.len() - 1;
        t[last] ^= 1;
        assert!(CardStore::open(&t, &kek, &HH).is_err());
        let mut t = sealed;
        t[0] = b'X';
        assert!(CardStore::open(&t, &kek, &HH).is_err());
    }

    #[test]
    fn nonces_differ_per_seal() {
        let kek = Kek::from_bytes([3; 32]);
        let s = CardStore::new();
        assert_ne!(s.seal(&kek, &HH).unwrap(), s.seal(&kek, &HH).unwrap());
    }

    #[test]
    fn debug_never_prints_key_material() {
        let kek = Kek::from_bytes([0xab; 32]);
        let mut s = CardStore::new();
        s.insert([1; 16], [0xcd; 16]);
        let d = format!("{kek:?} {s:?}");
        assert!(
            !d.contains("ab") && !d.contains("cd") && !d.contains("171"),
            "{d}"
        );
    }

    #[cfg(unix)]
    #[test]
    fn kek_inside_data_dir_or_world_readable_is_refused() {
        use std::os::unix::fs::PermissionsExt;
        let base = std::env::temp_dir().join(format!(
            "hk-kek-{}",
            crate::hex::encode(&rng::array::<8>().unwrap())
        ));
        let data = base.join("data");
        std::fs::create_dir_all(&data).unwrap();
        let inside = data.join("kek");
        std::fs::write(&inside, Kek::generate().unwrap().to_hex().as_bytes()).unwrap();
        std::fs::set_permissions(&inside, std::fs::Permissions::from_mode(0o600)).unwrap();
        assert!(load_kek(&inside, &data).is_err());
        let outside = base.join("kek");
        std::fs::write(&outside, Kek::generate().unwrap().to_hex().as_bytes()).unwrap();
        std::fs::set_permissions(&outside, std::fs::Permissions::from_mode(0o644)).unwrap();
        assert!(load_kek(&outside, &data).is_err());
        std::fs::set_permissions(&outside, std::fs::Permissions::from_mode(0o600)).unwrap();
        assert!(load_kek(&outside, &data).is_ok());
        // A path that sneaks back in through `..` is still inside.
        let sneaky = base.join("data").join("..").join("data").join("kek");
        assert!(load_kek(&sneaky, &data).is_err());
        std::fs::remove_dir_all(&base).unwrap();
    }
}
