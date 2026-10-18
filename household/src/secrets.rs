//! The hub's secrets, sealed: card keys and the hub's two signing keys.
//!
//! Two kinds of material in the checker's data directory are secret:
//!
//! - **card keys**, which are clone-capable: whoever holds one can program a
//!   card that passes as that household card;
//! - **the hub's Ed25519 keys**: the posture key, which every witness pins
//!   and which alone decides whether indoor witnesses sense (whoever holds it
//!   can sign `home` and skip the intent-credential rule entirely), and the
//!   roster key, which countersigns every accepted roster.
//!
//! Both files are sealed with XChaCha20-Poly1305 under keys derived from one
//! key-encryption key (KEK) that is **not** in the data directory:
//! [`load_kek`] refuses a KEK path inside it. Plaintext keys live only in
//! [`Zeroizing`] buffers (and in `ed25519_dalek::SigningKey`, which zeroizes
//! itself on drop). File formats:
//!
//! ```text
//! cards.sealed     "SCVK" ‖ 0x01 ‖ nonce(24) ‖ XChaCha20-Poly1305(k, nonce, aad, plaintext)
//!                  k         = HKDF-SHA256(ikm = KEK, salt = household, info = "securacv-hk-cards-v1")
//!                  aad       = "SCVK" ‖ 0x01 ‖ household
//!                  plaintext = be16(count) ‖ (handle(16) ‖ key(16))*
//! hub_keys.sealed  "SCVH" ‖ 0x01 ‖ nonce(24) ‖ XChaCha20-Poly1305(k, nonce, aad, plaintext)
//!                  k         = HKDF-SHA256(ikm = KEK, salt = household, info = "securacv-hk-hub-keys-v1")
//!                  aad       = "SCVH" ‖ 0x01 ‖ household
//!                  plaintext = posture_secret(32) ‖ roster_secret(32)
//! ```
//!
//! Sealing protects a copy of the data directory: a backup or a tarball of
//! it. It does not protect a disk image that also holds the KEK file. A
//! leaked posture key means a new key and re-pairing every witness (a forged
//! statement with a huge `seq` would otherwise lock out every genuine one); a
//! leaked card key means re-keying that card.

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
const HUB_MAGIC: &[u8; 4] = b"SCVH";
const HUB_INFO: &[u8] = b"securacv-hk-hub-keys-v1";
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

    /// A copy of the KEK, for tests that need two owners of one key.
    #[cfg(test)]
    pub(crate) fn clone_for_test(&self) -> Kek {
        Kek(Zeroizing::new(*self.0))
    }

    /// A fresh random KEK.
    pub fn generate() -> Result<Kek> {
        Ok(Kek(Zeroizing::new(rng::array()?)))
    }

    /// The KEK as 64 lowercase hex characters (for writing the KEK file).
    pub fn to_hex(&self) -> Zeroizing<String> {
        Zeroizing::new(crate::hex::encode(&self.0[..]))
    }

    fn aead(&self, household: &[u8; 16], info: &[u8]) -> XChaCha20Poly1305 {
        let hk = Hkdf::<Sha256>::new(Some(household), &self.0[..]);
        let mut k = Zeroizing::new([0u8; 32]);
        hk.expand(info, &mut k[..])
            .expect("32 bytes is a valid HKDF length");
        XChaCha20Poly1305::new_from_slice(&k[..]).expect("32-byte key")
    }

    /// Seal `plaintext` as `magic ‖ 0x01 ‖ nonce ‖ ciphertext`.
    fn seal(
        &self,
        household: &[u8; 16],
        magic: &[u8; 4],
        info: &[u8],
        plaintext: &[u8],
    ) -> Result<Vec<u8>> {
        let nonce: [u8; 24] = rng::array()?;
        let mut aad = magic.to_vec();
        aad.push(VERSION);
        aad.extend_from_slice(household);
        let ct = self
            .aead(household, info)
            .encrypt(
                (&nonce).into(),
                Payload {
                    msg: plaintext,
                    aad: &aad,
                },
            )
            .map_err(|_| Error::Invalid("sealing failed"))?;
        let mut out = magic.to_vec();
        out.push(VERSION);
        out.extend_from_slice(&nonce);
        out.extend_from_slice(&ct);
        Ok(out)
    }

    /// Open what [`Kek::seal`] made.
    fn open(
        &self,
        sealed: &[u8],
        household: &[u8; 16],
        magic: &[u8; 4],
        info: &[u8],
    ) -> Result<Zeroizing<Vec<u8>>> {
        if sealed.len() < 5 + 24 + 16 || sealed.len() > MAX_SEALED_BYTES {
            return Err(Error::Invalid("a sealed file has the wrong size"));
        }
        if &sealed[..4] != magic || sealed[4] != VERSION {
            return Err(Error::Invalid("not the sealed file expected"));
        }
        let mut nonce = [0u8; 24];
        nonce.copy_from_slice(&sealed[5..29]);
        let mut aad = magic.to_vec();
        aad.push(VERSION);
        aad.extend_from_slice(household);
        Ok(Zeroizing::new(
            self.aead(household, info)
                .decrypt(
                    (&nonce).into(),
                    Payload {
                        msg: &sealed[29..],
                        aad: &aad,
                    },
                )
                .map_err(|_| Error::Rejected("a sealed file does not open under this KEK"))?,
        ))
    }
}

/// The hub's two Ed25519 signing keys.
pub struct HubKeys {
    /// The posture key: signs every household posture statement. Witnesses
    /// pin its public half.
    pub posture: ed25519_dalek::SigningKey,
    /// The roster key: countersigns every accepted roster.
    pub roster: ed25519_dalek::SigningKey,
}

impl std::fmt::Debug for HubKeys {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("HubKeys(<redacted>)")
    }
}

impl HubKeys {
    /// Two fresh keys from the OS RNG.
    pub fn generate() -> Result<HubKeys> {
        let p = Zeroizing::new(rng::array::<32>()?);
        let r = Zeroizing::new(rng::array::<32>()?);
        Ok(HubKeys {
            posture: ed25519_dalek::SigningKey::from_bytes(&p),
            roster: ed25519_dalek::SigningKey::from_bytes(&r),
        })
    }

    /// Seal under `kek` for `household` (`hub_keys.sealed`).
    pub fn seal(&self, kek: &Kek, household: &[u8; 16]) -> Result<Vec<u8>> {
        let mut pt = Zeroizing::new(Vec::with_capacity(64));
        pt.extend_from_slice(self.posture.as_bytes());
        pt.extend_from_slice(self.roster.as_bytes());
        kek.seal(household, HUB_MAGIC, HUB_INFO, &pt)
    }

    /// Open a sealed `hub_keys.sealed`.
    pub fn open(sealed: &[u8], kek: &Kek, household: &[u8; 16]) -> Result<HubKeys> {
        let pt = kek.open(sealed, household, HUB_MAGIC, HUB_INFO)?;
        let wrong = || Error::Invalid("sealed hub keys have the wrong length");
        if pt.len() != 64 {
            return Err(wrong());
        }
        // Built straight from the plaintext, never as zeroed buffers filled
        // afterwards (see rng::array).
        let p: Zeroizing<[u8; 32]> = Zeroizing::new(pt[..32].try_into().map_err(|_| wrong())?);
        let r: Zeroizing<[u8; 32]> = Zeroizing::new(pt[32..].try_into().map_err(|_| wrong())?);
        Ok(HubKeys {
            posture: ed25519_dalek::SigningKey::from_bytes(&p),
            roster: ed25519_dalek::SigningKey::from_bytes(&r),
        })
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

    /// Keep only the keys whose handle `keep` accepts; returns how many were
    /// removed.
    pub fn retain(&mut self, mut keep: impl FnMut(&Handle) -> bool) -> usize {
        let before = self.keys.len();
        self.keys.retain(|h, _| keep(h));
        before - self.keys.len()
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
        kek.seal(household, MAGIC, INFO, &pt)
    }

    /// Open a sealed store.
    pub fn open(sealed: &[u8], kek: &Kek, household: &[u8; 16]) -> Result<CardStore> {
        let pt = kek.open(sealed, household, MAGIC, INFO)?;
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
    fn hub_keys_seal_open_and_refuse_the_wrong_kek() {
        let kek = Kek::generate().unwrap();
        let keys = HubKeys::generate().unwrap();
        let sealed = keys.seal(&kek, &HH).unwrap();
        assert!(!sealed
            .windows(32)
            .any(|w| w == keys.posture.as_bytes() || w == keys.roster.as_bytes()));
        let o = HubKeys::open(&sealed, &kek, &HH).unwrap();
        assert_eq!(o.posture.to_bytes(), keys.posture.to_bytes());
        assert_eq!(o.roster.to_bytes(), keys.roster.to_bytes());
        assert!(HubKeys::open(&sealed, &Kek::generate().unwrap(), &HH).is_err());
        assert!(HubKeys::open(&sealed, &kek, &[0x43; 16]).is_err());
        // The card store and the hub keys are sealed under different derived
        // keys and magics: one never opens as the other.
        assert!(CardStore::open(&sealed, &kek, &HH).is_err());
        let cards = CardStore::new().seal(&kek, &HH).unwrap();
        assert!(HubKeys::open(&cards, &kek, &HH).is_err());
        assert_eq!(format!("{o:?}"), "HubKeys(<redacted>)");
    }

    #[test]
    fn retain_drops_keys_and_counts_them() {
        let mut s = CardStore::new();
        s.insert([1; 16], [0xa1; 16]);
        s.insert([2; 16], [0xa2; 16]);
        assert_eq!(s.retain(|h| *h == [2; 16]), 1);
        assert!(!s.contains(&[1; 16]) && s.contains(&[2; 16]));
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
