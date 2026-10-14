//! The household roster: who may present what, with no names.
//!
//! The roster is a JSON object serialized with `securacv-cjson-v1`
//! ([`crate::cjson`]):
//!
//! ```json
//! {
//!   "v": 1,
//!   "household": "<16-byte hex>",
//!   "epoch": 7,
//!   "prev": "<SHA-256 hex of the previous roster's signing bytes, or 64 zeros>",
//!   "threshold": 2,
//!   "admins": [
//!     { "handle": "<16-byte hex>", "alg": "ed25519", "kind": "cli", "public_key": "<hex>" }
//!   ],
//!   "members": [
//!     { "handle": "<16-byte hex>", "kind": "phone", "class": "intent",
//!       "alg": "es256", "public_key": "<SEC1 uncompressed hex>", "not_after": null }
//!   ]
//! }
//! ```
//!
//! - Admin `alg`/`kind` pairs: `ed25519`/`cli`, `es256`/`enclave`,
//!   `es256`/`fido2`. FIDO2 entries add `rp_id` and `credential_id`.
//! - Member `alg`/`kind` pairs: `es256` with `phone`, `watch` or `fob`;
//!   `fido2-es256` with `fido2` (adds `rp_id`, `credential_id`);
//!   `aes128-ev2` with `card` (no key in the roster: the card's AES key is in
//!   the sealed store, [`crate::secrets`], under the member handle).
//! - `class` is `intent` or `possession`. Fobs and cards are always
//!   `possession`; the admin quorum declares the class of everything else at
//!   enrollment, because the hub cannot prove that a phone key requires user
//!   presence.
//! - Handles are random and never derived from a key, a UID or a name.
//!   Unknown fields are refused, so **no field can hold a name or label**.
//! - Both lists are sorted by handle, so a roster has exactly one encoding.
//!
//! Signing bytes: `"securacv-roster-v1\0" ‖ cjson(roster)`; the roster digest
//! is `SHA-256(signing bytes)`.

use crate::cjson::{self, MAX_SAFE_INTEGER};
use crate::{hex, verify, Error, Result};
use serde::Deserialize;
use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};

/// A random 16-byte handle.
pub type Handle = [u8; 16];

/// Signing-bytes domain prefix.
pub const ROSTER_DOMAIN: &[u8; 19] = b"securacv-roster-v1\0";
/// Roster format version.
pub const ROSTER_VERSION: u64 = 1;
/// Most admins a roster may hold.
pub const MAX_ADMINS: usize = 16;
/// Most members a roster may hold.
pub const MAX_MEMBERS: usize = 64;
/// Most card members a roster may hold. This bounds the number of keys the
/// hub trial-authenticates per tap ([`crate::ntag424::CARD_TRIAL_SLOTS`]).
pub const MAX_CARDS: usize = crate::ntag424::CARD_TRIAL_SLOTS;
/// Longest RP id accepted.
pub const MAX_RP_ID: usize = 253;
/// Longest FIDO2 credential id accepted (bytes).
pub const MAX_CREDENTIAL_ID: usize = 1023;

/// Admin signature algorithm.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AdminAlg {
    /// Ed25519.
    Ed25519,
    /// ECDSA P-256 / SHA-256.
    Es256,
}

/// Where an admin key lives (and so which signed message it produces).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AdminKind {
    /// A key file on a computer (Ed25519).
    Cli,
    /// A phone secure-enclave key (ES256).
    Enclave,
    /// A FIDO2 authenticator (ES256 assertion, UV required).
    Fido2,
}

/// Member credential algorithm.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MemberAlg {
    /// ECDSA P-256 / SHA-256 over the challenge message.
    Es256,
    /// A FIDO2 assertion over `SHA-256(msg)`.
    Fido2Es256,
    /// NTAG 424 DNA `AuthenticateEV2First`.
    Aes128Ev2,
}

/// The kind of member credential.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MemberKind {
    /// A phone key.
    Phone,
    /// A watch key.
    Watch,
    /// A button fob.
    Fob,
    /// A FIDO2 security key.
    Fido2,
    /// An NTAG 424 DNA card.
    Card,
}

/// What a credential may do.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CredentialClass {
    /// Required its holder's own user verification: may set home, away, night.
    Intent,
    /// Possession only: may set away and night, never home.
    Possession,
}

/// One admin. One admin is one person, decided at an enrollment ceremony;
/// approvals are counted by handle, never by distinct key bytes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AdminEntry {
    /// Random handle.
    pub handle: Handle,
    /// Signature algorithm.
    pub alg: AdminAlg,
    /// Key location.
    pub kind: AdminKind,
    /// Ed25519: 32 bytes. ES256: 65-byte SEC1 uncompressed.
    pub public_key: Vec<u8>,
    /// FIDO2 only.
    pub rp_id: Option<String>,
    /// FIDO2 only.
    pub credential_id: Option<Vec<u8>>,
}

/// One member credential.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MemberEntry {
    /// Random handle.
    pub handle: Handle,
    /// Credential kind.
    pub kind: MemberKind,
    /// Credential class.
    pub class: CredentialClass,
    /// Algorithm.
    pub alg: MemberAlg,
    /// 65-byte SEC1 uncompressed for `es256` and `fido2-es256`; absent for cards.
    pub public_key: Option<Vec<u8>>,
    /// FIDO2 only.
    pub rp_id: Option<String>,
    /// FIDO2 only.
    pub credential_id: Option<Vec<u8>>,
    /// Unix seconds after which this credential is refused, or `None`.
    pub not_after: Option<u64>,
}

/// A roster.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Roster {
    /// Household id.
    pub household: [u8; 16],
    /// Epoch: +1 on every change.
    pub epoch: u64,
    /// Digest of the previous roster's signing bytes, or zeros for genesis.
    pub prev: [u8; 32],
    /// Admin approvals a threshold change needs.
    pub threshold: u64,
    /// Admins, sorted by handle.
    pub admins: Vec<AdminEntry>,
    /// Members, sorted by handle.
    pub members: Vec<MemberEntry>,
}

impl AdminAlg {
    fn word(self) -> &'static str {
        match self {
            AdminAlg::Ed25519 => "ed25519",
            AdminAlg::Es256 => "es256",
        }
    }
    /// Parse the roster word.
    pub fn parse(w: &str) -> Result<Self> {
        match w {
            "ed25519" => Ok(AdminAlg::Ed25519),
            "es256" => Ok(AdminAlg::Es256),
            _ => Err(Error::Invalid("unknown admin alg")),
        }
    }
}

impl AdminKind {
    fn word(self) -> &'static str {
        match self {
            AdminKind::Cli => "cli",
            AdminKind::Enclave => "enclave",
            AdminKind::Fido2 => "fido2",
        }
    }
    /// Parse the roster word.
    pub fn parse(w: &str) -> Result<Self> {
        match w {
            "cli" => Ok(AdminKind::Cli),
            "enclave" => Ok(AdminKind::Enclave),
            "fido2" => Ok(AdminKind::Fido2),
            _ => Err(Error::Invalid("unknown admin kind")),
        }
    }
}

impl MemberAlg {
    fn word(self) -> &'static str {
        match self {
            MemberAlg::Es256 => "es256",
            MemberAlg::Fido2Es256 => "fido2-es256",
            MemberAlg::Aes128Ev2 => "aes128-ev2",
        }
    }
    /// Parse the roster word.
    pub fn parse(w: &str) -> Result<Self> {
        match w {
            "es256" => Ok(MemberAlg::Es256),
            "fido2-es256" => Ok(MemberAlg::Fido2Es256),
            "aes128-ev2" => Ok(MemberAlg::Aes128Ev2),
            _ => Err(Error::Invalid("unknown member alg")),
        }
    }
}

impl MemberKind {
    fn word(self) -> &'static str {
        match self {
            MemberKind::Phone => "phone",
            MemberKind::Watch => "watch",
            MemberKind::Fob => "fob",
            MemberKind::Fido2 => "fido2",
            MemberKind::Card => "card",
        }
    }
    /// Parse the roster word.
    pub fn parse(w: &str) -> Result<Self> {
        match w {
            "phone" => Ok(MemberKind::Phone),
            "watch" => Ok(MemberKind::Watch),
            "fob" => Ok(MemberKind::Fob),
            "fido2" => Ok(MemberKind::Fido2),
            "card" => Ok(MemberKind::Card),
            _ => Err(Error::Invalid("unknown member kind")),
        }
    }
}

impl CredentialClass {
    fn word(self) -> &'static str {
        match self {
            CredentialClass::Intent => "intent",
            CredentialClass::Possession => "possession",
        }
    }
    /// Parse the roster word.
    pub fn parse(w: &str) -> Result<Self> {
        match w {
            "intent" => Ok(CredentialClass::Intent),
            "possession" => Ok(CredentialClass::Possession),
            _ => Err(Error::Invalid("unknown credential class")),
        }
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RosterJson {
    v: u64,
    household: String,
    epoch: u64,
    prev: String,
    threshold: u64,
    admins: Vec<AdminJson>,
    members: Vec<MemberJson>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct AdminJson {
    handle: String,
    alg: String,
    kind: String,
    public_key: String,
    #[serde(default)]
    rp_id: Option<String>,
    #[serde(default)]
    credential_id: Option<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct MemberJson {
    handle: String,
    kind: String,
    class: String,
    alg: String,
    #[serde(default)]
    public_key: Option<String>,
    #[serde(default)]
    rp_id: Option<String>,
    #[serde(default)]
    credential_id: Option<String>,
    not_after: Option<u64>,
}

fn valid_rp_id(rp: &str) -> bool {
    !rp.is_empty()
        && rp.len() <= MAX_RP_ID
        && rp
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'.' || b == b'-')
}

fn check_fido_fields(
    rp_id: &Option<String>,
    credential_id: &Option<Vec<u8>>,
    is_fido: bool,
) -> Result<()> {
    if is_fido {
        match (rp_id, credential_id) {
            (Some(rp), Some(cid))
                if valid_rp_id(rp) && !cid.is_empty() && cid.len() <= MAX_CREDENTIAL_ID =>
            {
                Ok(())
            }
            _ => Err(Error::Invalid(
                "a FIDO2 entry needs a valid rp_id and credential_id",
            )),
        }
    } else if rp_id.is_some() || credential_id.is_some() {
        Err(Error::Invalid(
            "rp_id and credential_id are FIDO2-only fields",
        ))
    } else {
        Ok(())
    }
}

impl AdminEntry {
    /// Check the entry's own shape and key.
    pub fn validate(&self) -> Result<()> {
        match (self.alg, self.kind) {
            (AdminAlg::Ed25519, AdminKind::Cli) => {
                let pk: [u8; 32] = self
                    .public_key
                    .as_slice()
                    .try_into()
                    .map_err(|_| Error::Invalid("ed25519 admin key must be 32 bytes"))?;
                ed25519_dalek::VerifyingKey::from_bytes(&pk)
                    .map_err(|_| Error::Invalid("ed25519 admin key is not a valid point"))?;
            }
            (AdminAlg::Es256, AdminKind::Enclave) | (AdminAlg::Es256, AdminKind::Fido2) => {
                verify::parse_sec1_uncompressed(&self.public_key)
                    .ok_or(Error::Invalid("es256 admin key must be SEC1 uncompressed"))?;
            }
            _ => return Err(Error::Invalid("admin alg/kind pair not allowed")),
        }
        check_fido_fields(
            &self.rp_id,
            &self.credential_id,
            self.kind == AdminKind::Fido2,
        )
    }

    fn to_value(&self) -> Value {
        let mut m = Map::new();
        m.insert("handle".into(), json!(hex::encode(&self.handle)));
        m.insert("alg".into(), json!(self.alg.word()));
        m.insert("kind".into(), json!(self.kind.word()));
        m.insert("public_key".into(), json!(hex::encode(&self.public_key)));
        if let Some(rp) = &self.rp_id {
            m.insert("rp_id".into(), json!(rp));
        }
        if let Some(cid) = &self.credential_id {
            m.insert("credential_id".into(), json!(hex::encode(cid)));
        }
        Value::Object(m)
    }

    fn from_json(j: AdminJson) -> Result<Self> {
        Ok(AdminEntry {
            handle: hex::decode_array(&j.handle)?,
            alg: AdminAlg::parse(&j.alg)?,
            kind: AdminKind::parse(&j.kind)?,
            public_key: hex::decode(&j.public_key)?,
            rp_id: j.rp_id,
            credential_id: j.credential_id.as_deref().map(hex::decode).transpose()?,
        })
    }
}

impl MemberEntry {
    /// Check the entry's own shape and key.
    pub fn validate(&self) -> Result<()> {
        match (self.alg, self.kind) {
            (MemberAlg::Es256, MemberKind::Phone | MemberKind::Watch | MemberKind::Fob)
            | (MemberAlg::Fido2Es256, MemberKind::Fido2) => {
                let pk = self
                    .public_key
                    .as_deref()
                    .ok_or(Error::Invalid("es256 members need a public key"))?;
                verify::parse_sec1_uncompressed(pk)
                    .ok_or(Error::Invalid("member key must be SEC1 uncompressed"))?;
            }
            (MemberAlg::Aes128Ev2, MemberKind::Card) => {
                if self.public_key.is_some() {
                    return Err(Error::Invalid("cards carry no key in the roster"));
                }
            }
            _ => return Err(Error::Invalid("member alg/kind pair not allowed")),
        }
        if matches!(self.kind, MemberKind::Fob | MemberKind::Card)
            && self.class != CredentialClass::Possession
        {
            return Err(Error::Invalid("fobs and cards are possession credentials"));
        }
        if let Some(t) = self.not_after {
            if t > MAX_SAFE_INTEGER {
                return Err(Error::Invalid("not_after outside the JS safe range"));
            }
        }
        check_fido_fields(
            &self.rp_id,
            &self.credential_id,
            self.kind == MemberKind::Fido2,
        )
    }

    fn to_value(&self) -> Value {
        let mut m = Map::new();
        m.insert("handle".into(), json!(hex::encode(&self.handle)));
        m.insert("kind".into(), json!(self.kind.word()));
        m.insert("class".into(), json!(self.class.word()));
        m.insert("alg".into(), json!(self.alg.word()));
        if let Some(pk) = &self.public_key {
            m.insert("public_key".into(), json!(hex::encode(pk)));
        }
        if let Some(rp) = &self.rp_id {
            m.insert("rp_id".into(), json!(rp));
        }
        if let Some(cid) = &self.credential_id {
            m.insert("credential_id".into(), json!(hex::encode(cid)));
        }
        m.insert(
            "not_after".into(),
            match self.not_after {
                Some(t) => json!(t),
                None => Value::Null,
            },
        );
        Value::Object(m)
    }

    fn from_json(j: MemberJson) -> Result<Self> {
        Ok(MemberEntry {
            handle: hex::decode_array(&j.handle)?,
            kind: MemberKind::parse(&j.kind)?,
            class: CredentialClass::parse(&j.class)?,
            alg: MemberAlg::parse(&j.alg)?,
            public_key: j.public_key.as_deref().map(hex::decode).transpose()?,
            rp_id: j.rp_id,
            credential_id: j.credential_id.as_deref().map(hex::decode).transpose()?,
            not_after: j.not_after,
        })
    }

    /// Whether this credential is usable at `now_unix`.
    pub fn is_current(&self, now_unix: u64) -> bool {
        self.not_after.is_none_or(|t| now_unix < t)
    }
}

fn strictly_sorted<T>(items: &[T], key: impl Fn(&T) -> &[u8]) -> bool {
    items.windows(2).all(|w| key(&w[0]) < key(&w[1]))
}

impl Roster {
    /// The genesis roster: epoch 1, `prev` all zeros, no members.
    pub fn genesis(
        household: [u8; 16],
        mut admins: Vec<AdminEntry>,
        threshold: u64,
    ) -> Result<Roster> {
        admins.sort_by_key(|a| a.handle);
        let r = Roster {
            household,
            epoch: 1,
            prev: [0u8; 32],
            threshold,
            admins,
            members: Vec::new(),
        };
        r.validate()?;
        Ok(r)
    }

    /// Check every structural rule. A roster that fails is never loaded,
    /// signed or applied.
    pub fn validate(&self) -> Result<()> {
        if self.epoch == 0 || self.epoch > MAX_SAFE_INTEGER {
            return Err(Error::Invalid("epoch out of range"));
        }
        if self.admins.is_empty() || self.admins.len() > MAX_ADMINS {
            return Err(Error::Invalid("admin count out of range"));
        }
        if self.members.len() > MAX_MEMBERS {
            return Err(Error::Invalid("too many members"));
        }
        if self.threshold == 0 || self.threshold > self.admins.len() as u64 {
            return Err(Error::Invalid("threshold must be in 1..=admins"));
        }
        if !strictly_sorted(&self.admins, |a| &a.handle)
            || !strictly_sorted(&self.members, |m| &m.handle)
        {
            return Err(Error::Invalid(
                "admins and members must be sorted by unique handle",
            ));
        }
        for a in &self.admins {
            a.validate()?;
            if self.members.iter().any(|m| m.handle == a.handle) {
                return Err(Error::Invalid("a handle is both an admin and a member"));
            }
            // One credential is never both an admin and a member. A FIDO2
            // authenticator signs whatever clientDataHash the relaying device
            // hands it, so a credential enrolled in both lists would let a
            // compromised relay turn an ordinary door touch into an admin
            // approval. (Distinct credentials on one authenticator are still
            // only as safe as the device relaying them: spec §5.2.)
            if self.members.iter().any(|m| {
                m.public_key.as_deref() == Some(&a.public_key[..])
                    || (a.credential_id.is_some() && m.credential_id == a.credential_id)
            }) {
                return Err(Error::Invalid(
                    "a credential is enrolled as both an admin and a member",
                ));
            }
        }
        // One key is never two admins: approvals are counted by handle, so a
        // key enrolled under two handles would count twice.
        for (i, a) in self.admins.iter().enumerate() {
            if self.admins[i + 1..]
                .iter()
                .any(|b| b.public_key == a.public_key)
            {
                return Err(Error::Invalid("two admin entries share a key"));
            }
        }
        let mut cards = 0usize;
        for (i, m) in self.members.iter().enumerate() {
            m.validate()?;
            if m.kind == MemberKind::Card {
                cards += 1;
            }
            for o in &self.members[i + 1..] {
                if m.public_key.is_some() && m.public_key == o.public_key {
                    return Err(Error::Invalid("two members share a key"));
                }
                if m.credential_id.is_some() && m.credential_id == o.credential_id {
                    return Err(Error::Invalid("two members share a credential id"));
                }
            }
        }
        if cards > MAX_CARDS {
            return Err(Error::Invalid("too many cards"));
        }
        Ok(())
    }

    /// The roster as a JSON value (the shape [`cjson`] encodes).
    pub fn to_value(&self) -> Value {
        json!({
            "v": ROSTER_VERSION,
            "household": hex::encode(&self.household),
            "epoch": self.epoch,
            "prev": hex::encode(&self.prev),
            "threshold": self.threshold,
            "admins": self.admins.iter().map(AdminEntry::to_value).collect::<Vec<_>>(),
            "members": self.members.iter().map(MemberEntry::to_value).collect::<Vec<_>>(),
        })
    }

    /// Parse and validate a roster from a JSON value.
    pub fn from_value(v: &Value) -> Result<Roster> {
        let j: RosterJson = serde_json::from_value(v.clone())
            .map_err(|_| Error::Invalid("roster JSON does not match the schema"))?;
        if j.v != ROSTER_VERSION {
            return Err(Error::Invalid("unknown roster version"));
        }
        let r = Roster {
            household: hex::decode_array(&j.household)?,
            epoch: j.epoch,
            prev: hex::decode_array(&j.prev)?,
            threshold: j.threshold,
            admins: j
                .admins
                .into_iter()
                .map(AdminEntry::from_json)
                .collect::<Result<_>>()?,
            members: j
                .members
                .into_iter()
                .map(MemberEntry::from_json)
                .collect::<Result<_>>()?,
        };
        r.validate()?;
        Ok(r)
    }

    /// `"securacv-roster-v1\0" ‖ cjson(roster)`.
    pub fn signing_bytes(&self) -> Result<Vec<u8>> {
        let mut out = ROSTER_DOMAIN.to_vec();
        out.extend_from_slice(&cjson::to_canonical_bytes(&self.to_value())?);
        Ok(out)
    }

    /// `SHA-256(signing bytes)`.
    pub fn digest(&self) -> Result<[u8; 32]> {
        Ok(Sha256::digest(self.signing_bytes()?).into())
    }

    /// Find a member by handle.
    pub fn member(&self, handle: &Handle) -> Option<&MemberEntry> {
        self.members.iter().find(|m| &m.handle == handle)
    }

    /// Find an admin by handle.
    pub fn admin(&self, handle: &Handle) -> Option<&AdminEntry> {
        self.admins.iter().find(|a| &a.handle == handle)
    }

    /// The successor template: same content, `epoch + 1`, `prev` = this
    /// roster's digest. Edit the result, then propose it.
    pub fn successor(&self) -> Result<Roster> {
        let mut n = self.clone();
        n.epoch = self
            .epoch
            .checked_add(1)
            .filter(|e| *e <= MAX_SAFE_INTEGER)
            .ok_or(Error::Invalid("epoch overflow"))?;
        n.prev = self.digest()?;
        Ok(n)
    }

    /// Sort both lists by handle (call after editing).
    pub fn sort(&mut self) {
        self.admins.sort_by_key(|a| a.handle);
        self.members.sort_by_key(|m| m.handle);
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use p256::ecdsa::SigningKey as P256Key;

    pub fn p256_pub(seed: u8) -> Vec<u8> {
        let k = P256Key::from_slice(&[seed; 32]).unwrap();
        k.verifying_key().to_sec1_point(false).as_bytes().to_vec()
    }

    pub fn ed_admin(handle: u8, seed: u8) -> AdminEntry {
        let sk = ed25519_dalek::SigningKey::from_bytes(&[seed; 32]);
        AdminEntry {
            handle: [handle; 16],
            alg: AdminAlg::Ed25519,
            kind: AdminKind::Cli,
            public_key: sk.verifying_key().to_bytes().to_vec(),
            rp_id: None,
            credential_id: None,
        }
    }

    pub fn phone(handle: u8, seed: u8, class: CredentialClass) -> MemberEntry {
        MemberEntry {
            handle: [handle; 16],
            kind: MemberKind::Phone,
            class,
            alg: MemberAlg::Es256,
            public_key: Some(p256_pub(seed)),
            rp_id: None,
            credential_id: None,
            not_after: None,
        }
    }

    pub fn card(handle: u8) -> MemberEntry {
        MemberEntry {
            handle: [handle; 16],
            kind: MemberKind::Card,
            class: CredentialClass::Possession,
            alg: MemberAlg::Aes128Ev2,
            public_key: None,
            rp_id: None,
            credential_id: None,
            not_after: None,
        }
    }

    fn sample() -> Roster {
        let mut r = Roster::genesis([0xaa; 16], vec![ed_admin(1, 1), ed_admin(2, 2)], 2).unwrap();
        r.members.push(phone(0x10, 0x31, CredentialClass::Intent));
        r.members.push(card(0x20));
        r.validate().unwrap();
        r
    }

    #[test]
    fn one_credential_is_never_both_admin_and_member() {
        // An es256 key enrolled as a FIDO2 admin and as a FIDO2 member.
        let mut r = sample();
        let shared = p256_pub(0x41);
        r.admins.push(AdminEntry {
            handle: [3; 16],
            alg: AdminAlg::Es256,
            kind: AdminKind::Fido2,
            public_key: shared.clone(),
            rp_id: Some("hk.example".into()),
            credential_id: Some(vec![9; 8]),
        });
        r.members.push(MemberEntry {
            handle: [0x30; 16],
            kind: MemberKind::Fido2,
            class: CredentialClass::Intent,
            alg: MemberAlg::Fido2Es256,
            public_key: Some(shared),
            rp_id: Some("hk.example".into()),
            credential_id: Some(vec![8; 8]),
            not_after: None,
        });
        r.sort();
        assert!(r.validate().is_err(), "shared key accepted");
        // Different keys, same credential id: refused too.
        let m = r
            .members
            .iter_mut()
            .find(|m| m.handle == [0x30; 16])
            .unwrap();
        m.public_key = Some(p256_pub(0x42));
        m.credential_id = Some(vec![9; 8]);
        assert!(r.validate().is_err(), "shared credential id accepted");
        // Distinct key and credential id: fine.
        let m = r
            .members
            .iter_mut()
            .find(|m| m.handle == [0x30; 16])
            .unwrap();
        m.credential_id = Some(vec![8; 8]);
        r.validate().unwrap();
    }

    #[test]
    fn json_round_trip_and_digest_is_stable() {
        let r = sample();
        let v = r.to_value();
        let back = Roster::from_value(&v).unwrap();
        assert_eq!(back, r);
        assert_eq!(back.digest().unwrap(), r.digest().unwrap());
        let sb = r.signing_bytes().unwrap();
        assert!(sb.starts_with(b"securacv-roster-v1\0"));
        // The card entry carries no key; not_after is always present.
        let s = String::from_utf8(sb[19..].to_vec()).unwrap();
        assert!(s.contains(r#""kind":"card","not_after":null}"#), "{s}");
    }

    #[test]
    fn unknown_fields_are_refused_so_no_name_can_hide() {
        let mut v = sample().to_value();
        v["members"][0]["name"] = json!("Alex");
        assert!(Roster::from_value(&v).is_err());
        let mut v = sample().to_value();
        v["admins"][0]["label"] = json!("Mom");
        assert!(Roster::from_value(&v).is_err());
        let mut v = sample().to_value();
        v["note"] = json!("x");
        assert!(Roster::from_value(&v).is_err());
    }

    #[test]
    fn structural_rules() {
        let base = sample();
        let mut r = base.clone();
        r.threshold = 3;
        assert!(r.validate().is_err());
        let mut r = base.clone();
        r.threshold = 0;
        assert!(r.validate().is_err());
        let mut r = base.clone();
        r.admins.reverse();
        assert!(r.validate().is_err(), "unsorted admins accepted");
        let mut r = base.clone();
        r.admins[1].public_key = r.admins[0].public_key.clone();
        assert!(r.validate().is_err(), "one key as two admins accepted");
        let mut r = base.clone();
        r.members[1].class = CredentialClass::Intent;
        assert!(r.validate().is_err(), "intent card accepted");
        let mut r = base.clone();
        r.members[0].kind = MemberKind::Fob;
        r.members[0].class = CredentialClass::Intent;
        assert!(r.validate().is_err(), "intent fob accepted");
        let mut r = base.clone();
        r.members[1].public_key = Some(p256_pub(9));
        assert!(r.validate().is_err(), "card with a roster key accepted");
        let mut r = base.clone();
        r.members[0].rp_id = Some("hk.x".into());
        assert!(r.validate().is_err(), "rp_id on a phone accepted");
        let mut r = base.clone();
        r.members[0].handle = r.admins[0].handle;
        r.sort();
        assert!(r.validate().is_err(), "shared admin/member handle accepted");
        let mut r = base.clone();
        r.admins.clear();
        assert!(r.validate().is_err());
        let mut r = base;
        r.members[0].public_key = Some(vec![0x02; 33]);
        assert!(r.validate().is_err(), "compressed key accepted");
    }

    #[test]
    fn card_count_is_bounded() {
        let mut r = sample();
        r.members.clear();
        for i in 0..=MAX_CARDS as u8 {
            r.members.push(card(0x40 + i));
        }
        assert!(r.validate().is_err());
        r.members.pop();
        assert!(r.validate().is_ok());
    }

    #[test]
    fn hex_case_is_strict() {
        let mut v = sample().to_value();
        v["household"] = json!("AA".repeat(16));
        assert!(Roster::from_value(&v).is_err());
    }

    #[test]
    fn successor_links_the_chain() {
        let r = sample();
        let n = r.successor().unwrap();
        assert_eq!(n.epoch, r.epoch + 1);
        assert_eq!(n.prev, r.digest().unwrap());
    }
}
