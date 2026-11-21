//! `SHR1`, the self-describing share envelope (spec §2.5).
//!
//! A share file is one line, `securacv-share:v1 <lowercase hex of SHR1>`,
//! mode 0600, printable as a paper backup. The binary form:
//!
//! ```text
//! 0    4   magic "SHR1"            4    1   version 0x01         5   1  suite 0x01
//! 6    1   protection 1|2|3        7    1   role 1|2|3           8   1  release_box 0|1|2
//! 9    16  split_id                25   32  C_0 (KEK commitment)
//! 57   32  container_sha256 (zero only for rehearsal)
//! 89   32  device_public_key (zero only for rehearsal)
//! 121  8   issued_bucket_start u64le (a multiple of 600)
//! 129  1   n (2..=m)               130  1   m (n..=32)           131 1  index (1..=m)
//! 132  1   holder_len L (1..=64)   133  L   holder [A-Za-z0-9._-]
//! —    32  recipient_key_hash      —    32·(n−1) C_1..C_{n−1}
//! —    1   roster_count (= m)
//! —    m × (index u8 ‖ id_len u8 ‖ id ‖ signing_public_key[32] ‖ recipient_key_hash[32])
//! —    2+C container_len u16le ‖ the MKC1 copy (C ≤ 8192; C = 0 only for rehearsal)
//! ── Hd: every byte above is the AEAD's associated data ──
//! protection 1|2: enc_len u16le (32|1120) ‖ enc ‖ ct_len u16le (80) ‖ ct   HPKE to the recipient key
//! protection 3:   m u32le ‖ t u32le ‖ p u32le ‖ salt[16] ‖ kek_commit[32] ‖ nonce[12] ‖ ct[80]
//! file_check[32] = SHA-256("securacv-share-file:v1" ‖ every preceding byte)
//! ```
//!
//! The binary form is at most 16 384 bytes and the text form 32 787; both
//! caps are checked before any length is read. Every refusal is a closed
//! reason ([`ShareReason`]).

use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;
use zeroize::Zeroizing;

use super::group::{decode_commitment, random_array, Commitments, CustodyRng};
use super::hpke::{open_base, seal_base_derand, BOX2_ENC_LEN};
use super::recipient::{RecipientPublic, RecipientSecret};
use super::vss::SecretShare;
use super::{
    holder_id_ok, CustodyError, ShareReason, ARGON2_M_KIB, ARGON2_P, ARGON2_T,
    BOX_HPKE_X25519_PSK_MLKEM768, CONTAINER_COPY_MAX, HPKE_INFO_SHARE, MAX_HOLDERS, MIN_THRESHOLD,
    PROTECTION_AT_REST, PROTECTION_TRANSPORT_BOX1, PROTECTION_TRANSPORT_BOX2, ROLE_OPERATIONAL,
    ROLE_RECOVERY, ROLE_REHEARSAL, SHARE_AT_REST_AAD_PREFIX, SHARE_FILE_CHECK_DOMAIN, SHARE_MAGIC,
    SHARE_MAX_BYTES, SHARE_TEXT_MAX_BYTES, SHARE_TEXT_PREFIX, SHARE_VERSION, SUITE_V1,
};
use crate::vault::crypto::{
    decrypt_payload, encrypt_payload, keyguard_kek, keyguard_params_pinned,
};

/// One holder of a split, as every share of it pins them.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct RosterEntry {
    /// The holder's index, `1..=m`.
    pub(crate) index: u8,
    /// The holder id.
    pub(crate) id: String,
    /// The holder's Ed25519 signing public key.
    pub(crate) signing_public_key: [u8; 32],
    /// The holder's recipient-key hash.
    pub(crate) recipient_key_hash: [u8; 32],
}

/// The authenticated header of a share file (`bytes[0..Hd]`). Public data.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ShareHeader {
    pub(crate) suite: u8,
    pub(crate) protection: u8,
    pub(crate) role: u8,
    pub(crate) release_box: u8,
    pub(crate) split_id: [u8; 16],
    pub(crate) container_sha256: [u8; 32],
    pub(crate) device_public_key: [u8; 32],
    pub(crate) issued_bucket_start: u64,
    pub(crate) n: u8,
    pub(crate) m: u8,
    pub(crate) index: u8,
    pub(crate) holder: String,
    pub(crate) recipient_key_hash: [u8; 32],
    /// `C_0..C_{n−1}`; `C_0` is the KEK commitment at offset 25.
    pub(crate) commitments: Commitments,
    pub(crate) roster: Vec<RosterEntry>,
    pub(crate) container: Vec<u8>,
}

impl ShareHeader {
    /// `C_0`, the KEK commitment.
    pub(crate) fn kek_commitment(&self) -> [u8; 32] {
        self.commitments.kek_commitment()
    }

    /// The commitment-set hash of this split.
    pub(crate) fn commitments_sha256(&self) -> [u8; 32] {
        self.commitments
            .sha256(self.suite, self.role, &self.split_id, self.m)
    }

    /// `bytes[0..Hd]`. Encodes the fields as they are: [`ShareFile::decode`]
    /// is what checks them.
    pub(crate) fn to_bytes(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(1024 + self.container.len());
        out.extend_from_slice(SHARE_MAGIC);
        out.push(SHARE_VERSION);
        out.push(self.suite);
        out.push(self.protection);
        out.push(self.role);
        out.push(self.release_box);
        out.extend_from_slice(&self.split_id);
        let points = self.commitments.encode();
        out.extend_from_slice(&points[0]);
        out.extend_from_slice(&self.container_sha256);
        out.extend_from_slice(&self.device_public_key);
        out.extend_from_slice(&self.issued_bucket_start.to_le_bytes());
        out.push(self.n);
        out.push(self.m);
        out.push(self.index);
        out.push(self.holder.len() as u8);
        out.extend_from_slice(self.holder.as_bytes());
        out.extend_from_slice(&self.recipient_key_hash);
        for c in &points[1..] {
            out.extend_from_slice(c);
        }
        out.push(self.roster.len() as u8);
        for e in &self.roster {
            out.push(e.index);
            out.push(e.id.len() as u8);
            out.extend_from_slice(e.id.as_bytes());
            out.extend_from_slice(&e.signing_public_key);
            out.extend_from_slice(&e.recipient_key_hash);
        }
        out.extend_from_slice(&(self.container.len() as u16).to_le_bytes());
        out.extend_from_slice(&self.container);
        out
    }

    /// The roster entry of this share's holder.
    pub(crate) fn own_entry(&self) -> Option<&RosterEntry> {
        self.roster.iter().find(|e| e.index == self.index)
    }
}

/// What follows the header.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum ShareBody {
    /// Protection 1 or 2: HPKE to the holder's recipient key.
    Transport { enc: Vec<u8>, ct: [u8; 80] },
    /// Protection 3: ChaCha20-Poly1305 under Argon2id(passphrase, salt).
    AtRest {
        salt: [u8; 16],
        kek_commit: [u8; 32],
        nonce: [u8; 12],
        ct: [u8; 80],
    },
}

/// A decoded share file. It holds the share only encrypted.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ShareFile {
    pub(crate) header: ShareHeader,
    pub(crate) body: ShareBody,
}

/// A bounded cursor that reports `truncated` and never slices past the end.
struct Cursor<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Cursor<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], CustodyError> {
        let end = self
            .at
            .checked_add(n)
            .filter(|&e| e <= self.bytes.len())
            .ok_or(ShareReason::Truncated)?;
        let out = &self.bytes[self.at..end];
        self.at = end;
        Ok(out)
    }
    fn u8(&mut self) -> Result<u8, CustodyError> {
        Ok(self.take(1)?[0])
    }
    fn u16(&mut self) -> Result<u16, CustodyError> {
        Ok(u16::from_le_bytes(self.array()?))
    }
    fn u32(&mut self) -> Result<u32, CustodyError> {
        Ok(u32::from_le_bytes(self.array()?))
    }
    fn array<const N: usize>(&mut self) -> Result<[u8; N], CustodyError> {
        Ok(self.take(N)?.try_into().expect("N bytes"))
    }
    fn id(&mut self) -> Result<String, CustodyError> {
        let len = usize::from(self.u8()?);
        if len == 0 || len > super::HOLDER_ID_MAX {
            return Err(ShareReason::HolderId.into());
        }
        let id = std::str::from_utf8(self.take(len)?)
            .ok()
            .filter(|h| holder_id_ok(h))
            .ok_or(ShareReason::HolderId)?;
        Ok(id.to_string())
    }
}

fn file_check(bytes: &[u8]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(SHARE_FILE_CHECK_DOMAIN);
    h.update(bytes);
    h.finalize().into()
}

fn point_reason(e: CustodyError) -> CustodyError {
    match e {
        CustodyError::PointNotCanonical(j) => ShareReason::CommitmentNotCanonical(j).into(),
        CustodyError::PointIdentity(j) => ShareReason::CommitmentIdentity(j).into(),
        other => other,
    }
}

fn parse_header(c: &mut Cursor<'_>) -> Result<ShareHeader, CustodyError> {
    if c.take(4)? != SHARE_MAGIC {
        return Err(ShareReason::BadMagic.into());
    }
    let version = c.u8()?;
    if version != SHARE_VERSION {
        return Err(ShareReason::UnsupportedVersion(version).into());
    }
    let suite = c.u8()?;
    if suite != SUITE_V1 {
        return Err(ShareReason::UnsupportedSuite(suite).into());
    }
    let protection = c.u8()?;
    if !(PROTECTION_TRANSPORT_BOX1..=PROTECTION_AT_REST).contains(&protection) {
        return Err(ShareReason::UnknownProtection(protection).into());
    }
    let role = c.u8()?;
    if !(ROLE_OPERATIONAL..=ROLE_REHEARSAL).contains(&role) {
        return Err(ShareReason::UnknownRole(role).into());
    }
    let release_box = c.u8()?;
    if release_box > BOX_HPKE_X25519_PSK_MLKEM768 {
        return Err(ShareReason::UnknownReleaseBox(release_box).into());
    }
    let split_id = c.array::<16>()?;
    let c0 = c.array::<32>()?;
    let container_sha256 = c.array::<32>()?;
    let device_public_key = c.array::<32>()?;
    let issued_bucket_start = u64::from_le_bytes(c.array()?);
    let n = c.u8()?;
    let m = c.u8()?;
    if n < MIN_THRESHOLD || m > MAX_HOLDERS || n > m {
        return Err(ShareReason::Threshold.into());
    }
    let index = c.u8()?;
    if index == 0 || index > m {
        return Err(ShareReason::Index(m).into());
    }
    let holder = c.id()?;
    let recipient_key_hash = c.array::<32>()?;
    let mut points = vec![c0];
    for _ in 1..n {
        points.push(c.array::<32>()?);
    }
    let mut decoded = Vec::with_capacity(points.len());
    for (j, p) in points.iter().enumerate() {
        decoded.push(decode_commitment(p, j).map_err(point_reason)?);
    }
    let commitments = Commitments::from_points(decoded);
    let roster_count = c.u8()?;
    if roster_count != m {
        return Err(ShareReason::RosterIndices(m).into());
    }
    let mut roster = Vec::with_capacity(usize::from(m));
    for want in 1..=m {
        let entry_index = c.u8()?;
        if entry_index != want {
            return Err(ShareReason::RosterIndices(m).into());
        }
        let id = c.id()?;
        let signing_public_key = c.array::<32>()?;
        let entry_hash = c.array::<32>()?;
        roster.push(RosterEntry {
            index: entry_index,
            id,
            signing_public_key,
            recipient_key_hash: entry_hash,
        });
    }
    for (i, a) in roster.iter().enumerate() {
        for b in &roster[i + 1..] {
            if a.id == b.id
                || a.signing_public_key == b.signing_public_key
                || a.recipient_key_hash == b.recipient_key_hash
            {
                return Err(ShareReason::RosterRepeats.into());
            }
        }
    }
    let own = &roster[usize::from(index) - 1];
    if own.id != holder || own.recipient_key_hash != recipient_key_hash {
        return Err(ShareReason::HolderEntryMismatch(index).into());
    }
    let container_len = usize::from(c.u16()?);
    if container_len > CONTAINER_COPY_MAX {
        return Err(ShareReason::ContainerTooLong.into());
    }
    let container = c.take(container_len)?.to_vec();
    if issued_bucket_start % 600 != 0 {
        return Err(ShareReason::IssuedBucket.into());
    }
    match role {
        ROLE_REHEARSAL => {
            if device_public_key != [0u8; 32]
                || container_sha256 != [0u8; 32]
                || !container.is_empty()
                || release_box != 0
            {
                return Err(ShareReason::RehearsalCarries.into());
            }
        }
        _ => {
            if device_public_key == [0u8; 32] || container.is_empty() {
                return Err(ShareReason::OperationalLacks.into());
            }
            if role == ROLE_RECOVERY && release_box != 0 {
                return Err(ShareReason::RecoveryReleaseBox.into());
            }
            if role == ROLE_OPERATIONAL && release_box == 0 {
                return Err(ShareReason::UnknownReleaseBox(0).into());
            }
            let got: [u8; 32] = Sha256::digest(&container).into();
            if got != container_sha256 {
                return Err(ShareReason::ContainerHash.into());
            }
        }
    }
    Ok(ShareHeader {
        suite,
        protection,
        role,
        release_box,
        split_id,
        container_sha256,
        device_public_key,
        issued_bucket_start,
        n,
        m,
        index,
        holder,
        recipient_key_hash,
        commitments,
        roster,
        container,
    })
}

fn parse_body(c: &mut Cursor<'_>, protection: u8) -> Result<ShareBody, CustodyError> {
    if protection == PROTECTION_AT_REST {
        let (m, t, p) = (c.u32()?, c.u32()?, c.u32()?);
        if !keyguard_params_pinned(m, t, p) {
            return Err(ShareReason::ArgonParameters.into());
        }
        return Ok(ShareBody::AtRest {
            salt: c.array()?,
            kek_commit: c.array()?,
            nonce: c.array()?,
            ct: c.array()?,
        });
    }
    let enc_len = usize::from(c.u16()?);
    let want = if protection == PROTECTION_TRANSPORT_BOX1 {
        32
    } else {
        BOX2_ENC_LEN
    };
    if enc_len != want {
        return Err(ShareReason::EncapsulationLength(enc_len, protection).into());
    }
    let enc = c.take(enc_len)?.to_vec();
    if c.u16()? != 80 {
        return Err(ShareReason::CiphertextLength.into());
    }
    Ok(ShareBody::Transport {
        enc,
        ct: c.array()?,
    })
}

impl ShareFile {
    /// Parse a binary share file without failing on its file check: the
    /// header and body are checked against every closed reason, and the
    /// second value says whether the file check matched. `share inspect`
    /// uses this to show a damaged file's header; everything else uses
    /// [`Self::decode`].
    pub(crate) fn parse(bin: &[u8]) -> Result<(Self, bool), CustodyError> {
        if bin.len() > SHARE_MAX_BYTES {
            return Err(ShareReason::TooLong.into());
        }
        let mut c = Cursor { bytes: bin, at: 0 };
        let header = parse_header(&mut c)?;
        let body = parse_body(&mut c, header.protection)?;
        let checked_len = c.at;
        let recorded = c.array::<32>()?;
        if c.at != bin.len() {
            return Err(ShareReason::TrailingBytes.into());
        }
        let ok = bool::from(file_check(&bin[..checked_len]).ct_eq(&recorded));
        Ok((ShareFile { header, body }, ok))
    }

    /// Decode a binary share file, file check included.
    pub(crate) fn decode(bin: &[u8]) -> Result<Self, CustodyError> {
        let (file, ok) = Self::parse(bin)?;
        if !ok {
            return Err(ShareReason::FileCheck.into());
        }
        Ok(file)
    }

    /// The bytes of the text form's line: `securacv-share:v1 <hex>` and one
    /// LF; lowercase hex only, no CR, nothing after the LF.
    pub(crate) fn text_to_binary(text: &[u8]) -> Result<Vec<u8>, CustodyError> {
        if text.len() > SHARE_TEXT_MAX_BYTES {
            return Err(ShareReason::TooLong.into());
        }
        let prefix = SHARE_TEXT_PREFIX.as_bytes();
        if text.len() <= prefix.len()
            || &text[..prefix.len()] != prefix
            || text[prefix.len()] != b' '
        {
            return Err(ShareReason::NotShareText.into());
        }
        let rest = &text[prefix.len() + 1..];
        let body = rest.strip_suffix(b"\n").ok_or(ShareReason::HexMalformed)?;
        if body.is_empty()
            || body.len() % 2 != 0
            || !body.iter().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
        {
            return Err(ShareReason::HexMalformed.into());
        }
        hex::decode(body).map_err(|_| ShareReason::HexMalformed.into())
    }

    /// Decode the text form.
    pub(crate) fn from_text(text: &[u8]) -> Result<Self, CustodyError> {
        Self::decode(&Self::text_to_binary(text)?)
    }

    /// The binary form, file check included.
    pub(crate) fn encode(&self) -> Vec<u8> {
        let mut out = self.header_bytes();
        match &self.body {
            ShareBody::Transport { enc, ct } => {
                out.extend_from_slice(&(enc.len() as u16).to_le_bytes());
                out.extend_from_slice(enc);
                out.extend_from_slice(&80u16.to_le_bytes());
                out.extend_from_slice(ct);
            }
            ShareBody::AtRest {
                salt,
                kek_commit,
                nonce,
                ct,
            } => {
                out.extend_from_slice(&ARGON2_M_KIB.to_le_bytes());
                out.extend_from_slice(&ARGON2_T.to_le_bytes());
                out.extend_from_slice(&ARGON2_P.to_le_bytes());
                out.extend_from_slice(salt);
                out.extend_from_slice(kek_commit);
                out.extend_from_slice(nonce);
                out.extend_from_slice(ct);
            }
        }
        let check = file_check(&out);
        out.extend_from_slice(&check);
        out
    }

    /// The text form: `securacv-share:v1 <hex>\n`.
    pub(crate) fn to_text(&self) -> String {
        format!("{SHARE_TEXT_PREFIX} {}\n", hex::encode(self.encode()))
    }

    /// `bytes[0..Hd]`, the AEAD's associated data.
    pub(crate) fn header_bytes(&self) -> Vec<u8> {
        self.header.to_bytes()
    }

    /// Whether `raw`'s trailing file check matches the bytes before it.
    pub(crate) fn file_check_ok(&self, raw: &[u8]) -> bool {
        raw.len() >= 32 && {
            let (body, check) = raw.split_at(raw.len() - 32);
            bool::from(file_check(body).ct_eq(check))
        }
    }

    /// Re-decode what this file encodes: a writer refuses a file its own
    /// reader would refuse.
    fn checked(self) -> Result<Self, CustodyError> {
        let again = Self::decode(&self.encode())?;
        if again != self {
            return Err(CustodyError::SealMismatch);
        }
        Ok(self)
    }

    /// Seal `share` to `to` as a transport share (protection 1 or 2, as the
    /// header says) with the ephemeral input given (the KAT seam for box 1).
    pub(crate) fn seal_transport_box1_derand(
        header: ShareHeader,
        share: &SecretShare,
        to: &RecipientPublic,
        ikm_e: &[u8; 32],
    ) -> Result<Self, CustodyError> {
        check_seal(&header, share, to)?;
        if header.protection != PROTECTION_TRANSPORT_BOX1 {
            return Err(CustodyError::SealMismatch);
        }
        let aad = header.to_bytes();
        let (enc, ct) = seal_base_derand(
            &to.x25519_public()?,
            HPKE_INFO_SHARE,
            &aad,
            &share.to_bytes()[..],
            ikm_e,
        )?;
        ShareFile {
            header,
            body: ShareBody::Transport {
                enc: enc.to_vec(),
                ct: ct.try_into().map_err(|_| CustodyError::SealMismatch)?,
            },
        }
        .checked()
    }

    /// Box 2's HPKE layer with a given ML-KEM shared secret and ciphertext
    /// (the KAT seam: no ML-KEM runs).
    pub(crate) fn seal_transport_box2_layer_derand(
        header: ShareHeader,
        share: &SecretShare,
        to: &RecipientPublic,
        ss_m: &[u8; 32],
        ct_m: &[u8],
        ikm_e: &[u8; 32],
    ) -> Result<Self, CustodyError> {
        if header.protection != PROTECTION_TRANSPORT_BOX2 || share.index != header.index {
            return Err(CustodyError::SealMismatch);
        }
        let aad = header.to_bytes();
        let (enc, ct) = super::hpke::seal_box2_psk_layer_derand(
            &to.x25519_public()?,
            ss_m,
            ct_m,
            HPKE_INFO_SHARE,
            &aad,
            &share.to_bytes()[..],
            ikm_e,
        )?;
        ShareFile {
            header,
            body: ShareBody::Transport {
                enc,
                ct: ct.try_into().map_err(|_| CustodyError::SealMismatch)?,
            },
        }
        .checked()
    }

    /// Seal `share` to the holder's recipient key: box 1, or box 2 when the
    /// header says protection 2 (a `pqc-vault` build, and a recipient with
    /// an ML-KEM key).
    pub(crate) fn seal_transport(
        header: ShareHeader,
        share: &SecretShare,
        to: &RecipientPublic,
        rng: &mut dyn CustodyRng,
    ) -> Result<Self, CustodyError> {
        if header.protection == PROTECTION_TRANSPORT_BOX2 {
            return seal_box2(header, share, to, rng);
        }
        let ikm_e = Zeroizing::new(random_array::<32>(rng));
        Self::seal_transport_box1_derand(header, share, to, &ikm_e)
    }

    /// Open a transport share with the holder's recipient key. The caller
    /// then runs the Pedersen check (`vss::check_share`).
    ///
    /// Before any HPKE runs, the roster pin is checked against the key the
    /// holder's own recipient key file records: the share's own roster
    /// entry must pin exactly that signing key, and the recorded recipient
    /// hash must be the one computed under it. The hash is never rebuilt
    /// from a signing key the share header supplies, so a dealer who pins
    /// a key of their own under the holder's name (with the hash
    /// recomputed over it) is refused, not reported as checked.
    pub(crate) fn open_transport(
        &self,
        key: &RecipientSecret,
    ) -> Result<SecretShare, CustodyError> {
        let ShareBody::Transport { enc, ct } = &self.body else {
            return Err(CustodyError::WrongProtection);
        };
        let own = self.header.own_entry().ok_or(CustodyError::SealMismatch)?;
        let recorded = &self.header.recipient_key_hash;
        let yours = key.own_key_hash();
        let same_holder = key.holder() == self.header.holder;
        let pinned_is_yours = own.signing_public_key == *key.signing_public_key();
        let hash_is_yours = bool::from(yours.ct_eq(recorded));
        if same_holder && !pinned_is_yours {
            // The share names this holder but pins another signing key for
            // them. If the recorded hash is this very recipient key's, under
            // either signing key, the box is sealed to the holder's key and
            // only the pin is wrong: say so. Otherwise it is simply another
            // recipient's share.
            let under_pinned = key.key_hash(&own.signing_public_key);
            if hash_is_yours || bool::from(under_pinned.ct_eq(recorded)) {
                return Err(CustodyError::PinnedSigningKeyDiffers);
            }
        }
        if !same_holder || !pinned_is_yours || !hash_is_yours {
            return Err(CustodyError::WrongRecipient {
                recorded: super::hex8(recorded),
                yours: super::hex8(&yours),
            });
        }
        let aad = self.header_bytes();
        let pt = match self.header.protection {
            PROTECTION_TRANSPORT_BOX1 => {
                let enc: [u8; 32] = enc
                    .as_slice()
                    .try_into()
                    .map_err(|_| CustodyError::HpkeOpen)?;
                open_base(key.x25519_secret(), &enc, HPKE_INFO_SHARE, &aad, ct)?
            }
            _ => open_box2(key, enc, &aad, ct)?,
        };
        share_from_plaintext(self.header.index, &pt)
    }

    /// Open a box-2 transport share's HPKE layer with a given ML-KEM shared
    /// secret (the KAT seam).
    #[cfg(test)]
    pub(crate) fn open_transport_box2_layer(
        &self,
        key: &RecipientSecret,
        ss_m: &[u8; 32],
    ) -> Result<SecretShare, CustodyError> {
        let ShareBody::Transport { enc, ct } = &self.body else {
            return Err(CustodyError::WrongProtection);
        };
        let pt = super::hpke::open_box2_psk_layer(
            key.x25519_secret(),
            ss_m,
            enc,
            HPKE_INFO_SHARE,
            &self.header_bytes(),
            ct,
        )?;
        share_from_plaintext(self.header.index, &pt)
    }

    /// Rewrap as protection 3 under an Argon2id output, salt and nonce (the
    /// KAT seam).
    pub(crate) fn protect_with_kek(
        &self,
        share: &SecretShare,
        argon_out: &[u8; 32],
        salt: [u8; 16],
        nonce: [u8; 12],
    ) -> Result<Self, CustodyError> {
        if share.index != self.header.index {
            return Err(CustodyError::SealMismatch);
        }
        let mut header = self.header.clone();
        header.protection = PROTECTION_AT_REST;
        let mut aad = SHARE_AT_REST_AAD_PREFIX.to_vec();
        aad.extend_from_slice(&header.to_bytes());
        let mut buf = Zeroizing::new(share.to_bytes().to_vec());
        let tag = encrypt_payload(argon_out, &nonce, &aad, &mut buf)
            .map_err(|_| CustodyError::AtRestAuthentication)?;
        let mut ct = [0u8; 80];
        ct[..64].copy_from_slice(&buf);
        ct[64..].copy_from_slice(&tag);
        ShareFile {
            header,
            body: ShareBody::AtRest {
                salt,
                kek_commit: Sha256::digest(argon_out).into(),
                nonce,
                ct,
            },
        }
        .checked()
    }

    /// Rewrap `share` (already opened from this file) as protection 3,
    /// under `passphrase`: Argon2id 65536/3/1 with a fresh salt and nonce.
    pub(crate) fn protect(
        &self,
        share: &SecretShare,
        passphrase: &str,
        rng: &mut dyn CustodyRng,
    ) -> Result<Self, CustodyError> {
        let salt: [u8; 16] = random_array(rng);
        let nonce: [u8; 12] = random_array(rng);
        let kek = keyguard_kek(passphrase, &salt, ARGON2_M_KIB, ARGON2_T, ARGON2_P)
            .map_err(|_| CustodyError::AtRestAuthentication)?;
        self.protect_with_kek(share, &kek, salt, nonce)
    }

    /// Open a protection-3 share with an Argon2id output (the KAT seam).
    pub(crate) fn open_at_rest_with_kek(
        &self,
        argon_out: &[u8; 32],
    ) -> Result<SecretShare, CustodyError> {
        let ShareBody::AtRest {
            kek_commit,
            nonce,
            ct,
            ..
        } = &self.body
        else {
            return Err(CustodyError::WrongProtection);
        };
        let commit: [u8; 32] = Sha256::digest(argon_out).into();
        if !bool::from(commit.ct_eq(kek_commit)) {
            return Err(CustodyError::WrongPassphrase);
        }
        let mut aad = SHARE_AT_REST_AAD_PREFIX.to_vec();
        aad.extend_from_slice(&self.header_bytes());
        let mut buf = Zeroizing::new(ct[..64].to_vec());
        decrypt_payload(argon_out, nonce, &aad, &mut buf, &ct[64..])
            .map_err(|_| CustodyError::AtRestAuthentication)?;
        share_from_plaintext(self.header.index, &buf)
    }

    /// Open a protection-3 share with the holder's passphrase. The Argon2id
    /// parameters were pinned when the file was decoded.
    pub(crate) fn open_at_rest(&self, passphrase: &str) -> Result<SecretShare, CustodyError> {
        let ShareBody::AtRest { salt, .. } = &self.body else {
            return Err(CustodyError::WrongProtection);
        };
        let kek = keyguard_kek(passphrase, salt, ARGON2_M_KIB, ARGON2_T, ARGON2_P)
            .map_err(|_| CustodyError::AtRestAuthentication)?;
        self.open_at_rest_with_kek(&kek)
    }
}

fn share_from_plaintext(index: u8, pt: &[u8]) -> Result<SecretShare, CustodyError> {
    let bytes: &[u8; 64] = pt.try_into().map_err(|_| CustodyError::HpkeOpen)?;
    SecretShare::from_bytes(index, bytes)
}

/// The header must name the recipient it is sealed to, and the share.
fn check_seal(
    header: &ShareHeader,
    share: &SecretShare,
    to: &RecipientPublic,
) -> Result<(), CustodyError> {
    let to_hash = to.key_hash()?;
    let own = header.own_entry().ok_or(CustodyError::SealMismatch)?;
    if share.index != header.index
        || to.holder() != header.holder
        || to_hash != header.recipient_key_hash
        || own.signing_public_key != to.signing_public_key()?
    {
        return Err(CustodyError::SealMismatch);
    }
    Ok(())
}

#[cfg(feature = "pqc-vault")]
fn seal_box2(
    header: ShareHeader,
    share: &SecretShare,
    to: &RecipientPublic,
    rng: &mut dyn CustodyRng,
) -> Result<ShareFile, CustodyError> {
    check_seal(&header, share, to)?;
    let pk_m = to.mlkem768_public()?.ok_or(CustodyError::SealMismatch)?;
    let aad = header.to_bytes();
    let (enc, ct) = super::hpke::seal_box2(
        &to.x25519_public()?,
        &pk_m,
        HPKE_INFO_SHARE,
        &aad,
        &share.to_bytes()[..],
        rng,
    )?;
    ShareFile {
        header,
        body: ShareBody::Transport {
            enc,
            ct: ct.try_into().map_err(|_| CustodyError::SealMismatch)?,
        },
    }
    .checked()
}

#[cfg(not(feature = "pqc-vault"))]
fn seal_box2(
    _header: ShareHeader,
    _share: &SecretShare,
    _to: &RecipientPublic,
    _rng: &mut dyn CustodyRng,
) -> Result<ShareFile, CustodyError> {
    Err(CustodyError::Box2NeedsFeature)
}

#[cfg(feature = "pqc-vault")]
fn open_box2(
    key: &RecipientSecret,
    enc: &[u8],
    aad: &[u8],
    ct: &[u8],
) -> Result<Zeroizing<Vec<u8>>, CustodyError> {
    let (_, sk_m) = key.mlkem().ok_or(CustodyError::HpkeOpen)?;
    super::hpke::open_box2(key.x25519_secret(), sk_m, enc, HPKE_INFO_SHARE, aad, ct)
}

#[cfg(not(feature = "pqc-vault"))]
fn open_box2(
    _key: &RecipientSecret,
    _enc: &[u8],
    _aad: &[u8],
    _ct: &[u8],
) -> Result<Zeroizing<Vec<u8>>, CustodyError> {
    Err(CustodyError::Box2NeedsFeature)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::vault::custody::container::{encode_mkc1_with, CRYPTO_MODE_CLASSICAL};
    use crate::vault::custody::group::OsCustodyRng;
    use crate::vault::custody::recipient::tests::{signed_public, spk_of};
    use crate::vault::custody::recipient::HOLDER_ROLE_TRUSTEE;
    use crate::vault::custody::vss::{check_share, deal, Kek};
    use crate::vault::custody::BOX_HPKE_X25519;

    /// The bucket every share test is issued in.
    pub(crate) const BUCKET: u64 = 1_794_312_000;

    /// One dealt split with its holders' keys: the shares, the recipients
    /// (secret and signed public), and an unsealed header per index.
    pub(crate) struct TestSplit {
        pub(crate) kek: Kek,
        pub(crate) commitments: Commitments,
        pub(crate) shares: Vec<SecretShare>,
        pub(crate) secrets: Vec<RecipientSecret>,
        pub(crate) publics: Vec<RecipientPublic>,
        pub(crate) role: u8,
        pub(crate) container: Vec<u8>,
    }

    pub(crate) const IDS: [&str; 6] = ["alice", "bob", "carol", "dave", "erin", "frank"];

    /// An n-of-m split of a fresh KEK, holders `IDS[..m]`, signing seeds
    /// `[i; 32]`, recipient keys `[0x40 + i; 32]`.
    pub(crate) fn test_split(n: u8, m: u8, role: u8) -> TestSplit {
        let mut rng = OsCustodyRng;
        let kek = Kek::generate(&mut rng);
        let indices: Vec<u8> = (1..=m).collect();
        let (commitments, shares) = deal(&kek, n, &indices, &mut rng).unwrap();
        let mut secrets = Vec::new();
        let mut publics = Vec::new();
        for i in 1..=m {
            let id = IDS[usize::from(i) - 1];
            let secret = RecipientSecret::from_parts(
                id,
                HOLDER_ROLE_TRUSTEE,
                spk_of(&[i; 32]),
                [0x40 + i; 32],
                None,
            );
            publics.push(signed_public(&secret, &[i; 32]));
            secrets.push(secret);
        }
        let container = if role == ROLE_REHEARSAL {
            Vec::new()
        } else {
            encode_mkc1_with(
                &kek,
                crate::vault::custody::tv::<16>("01"),
                crate::vault::custody::tv::<12>("02"),
                CRYPTO_MODE_CLASSICAL,
                &[3; 32],
                None,
            )
            .unwrap()
        };
        TestSplit {
            kek,
            commitments,
            shares,
            secrets,
            publics,
            role,
            container,
        }
    }

    impl TestSplit {
        pub(crate) fn header(&self, index: u8, protection: u8) -> ShareHeader {
            let roster = self
                .publics
                .iter()
                .enumerate()
                .map(|(i, p)| RosterEntry {
                    index: i as u8 + 1,
                    id: p.holder().to_string(),
                    signing_public_key: p.signing_public_key().unwrap(),
                    recipient_key_hash: p.key_hash().unwrap(),
                })
                .collect::<Vec<_>>();
            let own = &roster[usize::from(index) - 1];
            let rehearsal = self.role == ROLE_REHEARSAL;
            ShareHeader {
                suite: SUITE_V1,
                protection,
                role: self.role,
                release_box: match self.role {
                    ROLE_OPERATIONAL => BOX_HPKE_X25519,
                    _ => 0,
                },
                split_id: [0x5c; 16],
                container_sha256: if rehearsal {
                    [0; 32]
                } else {
                    Sha256::digest(&self.container).into()
                },
                device_public_key: if rehearsal { [0; 32] } else { [0xde; 32] },
                issued_bucket_start: BUCKET,
                n: self.commitments.threshold(),
                m: roster.len() as u8,
                index,
                holder: own.id.clone(),
                recipient_key_hash: own.recipient_key_hash,
                commitments: self.commitments.clone(),
                roster,
                container: self.container.clone(),
            }
        }

        /// The box-1 transport share file for `index`.
        pub(crate) fn sealed(&self, index: u8) -> ShareFile {
            let i = usize::from(index) - 1;
            ShareFile::seal_transport(
                self.header(index, PROTECTION_TRANSPORT_BOX1),
                &self.shares[i],
                &self.publics[i],
                &mut OsCustodyRng,
            )
            .unwrap()
        }
    }

    /// Recompute the trailing file check of `bin` (so a test that edits a
    /// field reaches the field's own check, not the file check).
    pub(crate) fn refile(mut bin: Vec<u8>) -> Vec<u8> {
        bin.truncate(bin.len() - 32);
        let check = file_check(&bin);
        bin.extend_from_slice(&check);
        bin
    }

    fn reason(bin: &[u8]) -> String {
        ShareFile::decode(bin).unwrap_err().to_string()
    }

    /// The bytes of `file` with its header replaced by `header`'s encoding
    /// (no validation: the decoder is what is under test).
    fn with_header(file: &ShareFile, header: ShareHeader) -> Vec<u8> {
        ShareFile {
            header,
            body: file.body.clone(),
        }
        .encode()
    }

    #[test]
    fn round_trip_transport_box1() {
        let split = test_split(3, 5, ROLE_OPERATIONAL);
        for index in 1..=5u8 {
            let i = usize::from(index) - 1;
            let file = split.sealed(index);
            let bin = file.encode();
            assert!(bin.len() <= SHARE_MAX_BYTES);
            assert_eq!(ShareFile::decode(&bin).unwrap(), file);
            let text = file.to_text();
            assert_eq!(ShareFile::from_text(text.as_bytes()).unwrap(), file);
            assert!(file.file_check_ok(&bin));
            assert_eq!(&bin[..file.header_bytes().len()], &file.header_bytes()[..]);
            let opened = file.open_transport(&split.secrets[i]).unwrap();
            assert_eq!(opened.index, index);
            assert_eq!(opened.scalars(), split.shares[i].scalars());
            assert!(check_share(&file.header.commitments, &opened));
            assert_eq!(file.header.kek_commitment(), split.kek.commitment());
        }
        // Another holder's key does not open it, and the refusal names both
        // recipients by 8-hex prefix only.
        let file = split.sealed(1);
        let err = file.open_transport(&split.secrets[1]).unwrap_err();
        let recorded = hex::encode(&file.header.recipient_key_hash[..4]);
        assert!(err
            .to_string()
            .starts_with(&format!("cannot open this share with the supplied recipient key: it was encrypted to recipient {recorded}…, yours is ")));
        // A transport share does not open as an at-rest one.
        assert_eq!(
            file.open_at_rest_with_kek(&[0; 32]).unwrap_err(),
            CustodyError::WrongProtection
        );
        // Sealing refuses a header that names another holder or share.
        assert_eq!(
            ShareFile::seal_transport(
                split.header(1, PROTECTION_TRANSPORT_BOX1),
                &split.shares[1],
                &split.publics[0],
                &mut OsCustodyRng
            )
            .unwrap_err(),
            CustodyError::SealMismatch
        );
        assert_eq!(
            ShareFile::seal_transport(
                split.header(1, PROTECTION_TRANSPORT_BOX1),
                &split.shares[0],
                &split.publics[1],
                &mut OsCustodyRng
            )
            .unwrap_err(),
            CustodyError::SealMismatch
        );
    }

    /// The dealer's forgery of a roster pin: holder `index`'s own roster
    /// entry pins `forged_spk`; with `recompute`, the recorded recipient
    /// hash (header and roster) is recomputed over the forged key, else the
    /// honest hash is kept. The box is sealed, honestly, to the holder's
    /// real X25519 key, so only the pin is false.
    pub(crate) fn forge_pin(
        split: &TestSplit,
        index: u8,
        forged_spk: [u8; 32],
        recompute: bool,
    ) -> ShareFile {
        let i = usize::from(index) - 1;
        let mut header = split.header(index, PROTECTION_TRANSPORT_BOX1);
        let secret = &split.secrets[i];
        header.roster[i].signing_public_key = forged_spk;
        if recompute {
            let forged_hash = secret.key_hash(&forged_spk);
            header.roster[i].recipient_key_hash = forged_hash;
            header.recipient_key_hash = forged_hash;
        }
        let (enc, ct) = super::super::hpke::seal_base_derand(
            &secret.x25519_public(),
            HPKE_INFO_SHARE,
            &header.to_bytes(),
            &split.shares[i].to_bytes()[..],
            &crate::vault::custody::tv::<32>("77"),
        )
        .unwrap();
        let forged = ShareFile {
            header,
            body: ShareBody::Transport {
                enc: enc.to_vec(),
                ct: ct.try_into().unwrap(),
            },
        };
        // A well-formed file: the decoder has no way to know the pin is
        // false, and the box really opens with the holder's key.
        ShareFile::decode(&forged.encode()).unwrap()
    }

    #[test]
    fn roster_pin_must_be_the_holders_own_signing_key() {
        let split = test_split(2, 3, ROLE_OPERATIONAL);
        let alice = &split.secrets[0];
        // A key the dealer holds, pinned under alice's name.
        let dealer_key = spk_of(&crate::vault::custody::tv::<32>("d1"));
        for recompute in [true, false] {
            let forged = forge_pin(&split, 1, dealer_key, recompute);
            // The box itself opens with alice's X25519 key: without the pin
            // check, every share check would pass.
            let ShareBody::Transport { enc, ct } = &forged.body else {
                panic!("a transport share");
            };
            let enc: [u8; 32] = enc.as_slice().try_into().unwrap();
            assert!(super::super::hpke::open_base(
                alice.x25519_secret(),
                &enc,
                HPKE_INFO_SHARE,
                &forged.header_bytes(),
                ct
            )
            .is_ok());
            let err = forged.open_transport(alice).unwrap_err();
            assert_eq!(err, CustodyError::PinnedSigningKeyDiffers, "{recompute}");
            assert_eq!(
                err.to_string(),
                "custody: the roster in this share pins a different signing key for you than \
                 your recipient key records"
            );
            // Protecting at rest goes through the same open, so the forged
            // pin never reaches an at-rest file.
        }
        // The same recipient key bound to the dealer's key (what the dealer
        // could build from alice's public file) is not alice's key file, and
        // a different holder's key is still simply the wrong recipient.
        let forged = forge_pin(&split, 1, dealer_key, true);
        assert!(matches!(
            forged.open_transport(&split.secrets[1]).unwrap_err(),
            CustodyError::WrongRecipient { .. }
        ));
        // Alice's honest share still opens.
        let honest = split.sealed(1);
        assert_eq!(
            honest.open_transport(alice).unwrap().scalars(),
            split.shares[0].scalars()
        );
        // A recipient key for alice under another signing key of her own
        // (not this share's recipient at all) is the wrong recipient, not
        // a forged pin: neither hash names this recipient key.
        let other = RecipientSecret::from_parts(
            "alice",
            HOLDER_ROLE_TRUSTEE,
            spk_of(&crate::vault::custody::tv::<32>("a2")),
            [0x99; 32],
            None,
        );
        assert!(matches!(
            honest.open_transport(&other).unwrap_err(),
            CustodyError::WrongRecipient { .. }
        ));
    }

    #[test]
    fn round_trip_at_rest_and_wrong_passphrase() {
        let split = test_split(2, 3, ROLE_OPERATIONAL);
        let file = split.sealed(2);
        let share = file.open_transport(&split.secrets[1]).unwrap();
        let protected = file
            .protect(&share, "share passphrase", &mut OsCustodyRng)
            .unwrap();
        assert_eq!(protected.header.protection, PROTECTION_AT_REST);
        let mut expect = file.header.clone();
        expect.protection = PROTECTION_AT_REST;
        assert_eq!(protected.header, expect);
        let bin = protected.encode();
        assert_eq!(ShareFile::decode(&bin).unwrap(), protected);
        let opened = protected.open_at_rest("share passphrase").unwrap();
        assert_eq!(opened.scalars(), share.scalars());
        assert!(check_share(&protected.header.commitments, &opened));
        assert_eq!(
            protected
                .open_at_rest("not the passphrase")
                .unwrap_err()
                .to_string(),
            "custody: wrong passphrase"
        );
        // A protected share no longer opens with the recipient key.
        assert_eq!(
            protected.open_transport(&split.secrets[1]).unwrap_err(),
            CustodyError::WrongProtection
        );
        // The KAT seam: a given Argon2id output; a body altered under the
        // right key fails authentication.
        let at_rest = file
            .protect_with_kek(
                &share,
                &[7; 32],
                crate::vault::custody::tv::<16>("08"),
                crate::vault::custody::tv::<12>("09"),
            )
            .unwrap();
        assert_eq!(
            at_rest.open_at_rest_with_kek(&[7; 32]).unwrap().scalars(),
            share.scalars()
        );
        let mut altered = at_rest.clone();
        if let ShareBody::AtRest { ct, .. } = &mut altered.body {
            ct[0] ^= 1;
        }
        assert_eq!(
            altered.open_at_rest_with_kek(&[7; 32]).unwrap_err(),
            CustodyError::AtRestAuthentication
        );
        assert_eq!(
            at_rest.open_at_rest_with_kek(&[6; 32]).unwrap_err(),
            CustodyError::WrongPassphrase
        );
    }

    #[test]
    fn every_header_byte_is_authenticated() {
        let split = test_split(2, 3, ROLE_OPERATIONAL);
        let file = split.sealed(1);
        let bin = file.encode();
        let hd = file.header_bytes().len();
        let mut decode_refusals = 0;
        let mut open_refusals = 0;
        for at in 0..hd {
            let mut flipped = bin.clone();
            flipped[at] ^= 0x01;
            match ShareFile::decode(&refile(flipped)) {
                Err(_) => decode_refusals += 1,
                Ok(f) => {
                    assert!(
                        f.open_transport(&split.secrets[0]).is_err(),
                        "header byte {at} is not authenticated"
                    );
                    open_refusals += 1;
                }
            }
        }
        assert_eq!(decode_refusals + open_refusals, hd);
        // Both paths are exercised: structural checks and the AEAD.
        assert!(decode_refusals > 0 && open_refusals > 0);

        // The same holds for the at-rest form's AAD.
        let share = file.open_transport(&split.secrets[0]).unwrap();
        let at_rest = file
            .protect_with_kek(
                &share,
                &[7; 32],
                crate::vault::custody::tv::<16>("08"),
                crate::vault::custody::tv::<12>("09"),
            )
            .unwrap();
        let bin = at_rest.encode();
        for at in 0..hd {
            let mut flipped = bin.clone();
            flipped[at] ^= 0x80;
            if let Ok(f) = ShareFile::decode(&refile(flipped)) {
                assert!(
                    f.open_at_rest_with_kek(&[7; 32]).is_err(),
                    "at-rest header byte {at} is not authenticated"
                );
            }
        }
    }

    #[test]
    fn file_check_detects_damage() {
        let split = test_split(2, 3, ROLE_OPERATIONAL);
        let file = split.sealed(3);
        let bin = file.encode();
        for at in [0, 9, 100, bin.len() - 100, bin.len() - 33, bin.len() - 1] {
            let mut damaged = bin.clone();
            damaged[at] ^= 0x10;
            let err = ShareFile::decode(&damaged).unwrap_err();
            // A damaged magic is caught by the magic; everything else the
            // header and body parse accepts is caught by the file check.
            if at == 0 {
                assert_eq!(err.to_string(), "share file: bad magic");
            } else {
                assert_eq!(
                    err.to_string(),
                    "share file: file check mismatch — the file is damaged",
                    "byte {at}"
                );
                let (parsed, ok) = ShareFile::parse(&damaged).unwrap();
                assert!(!ok);
                assert!(!parsed.file_check_ok(&damaged));
            }
        }
        assert!(file.file_check_ok(&bin));
    }

    #[test]
    fn text_form_round_trip_and_rejects_uppercase_cr_and_two_lfs() {
        let split = test_split(2, 2, ROLE_REHEARSAL);
        let file = split.sealed(1);
        let text = file.to_text();
        assert!(text.starts_with("securacv-share:v1 "));
        assert!(text.ends_with('\n'));
        assert_eq!(text.matches('\n').count(), 1);
        assert_eq!(ShareFile::from_text(text.as_bytes()).unwrap(), file);
        let hex_part = &text["securacv-share:v1 ".len()..text.len() - 1];
        let malformed = "share file: the hex form is malformed";
        let not_one = "share file: not a securacv-share:v1 file";
        let cases: Vec<(String, &str)> = vec![
            (
                format!("securacv-share:v1 {}\n", hex_part.to_uppercase()),
                malformed,
            ),
            (format!("securacv-share:v1 {hex_part}\r\n"), malformed),
            (format!("securacv-share:v1 {hex_part}\n\n"), malformed),
            (format!("securacv-share:v1 {hex_part}"), malformed),
            (format!("securacv-share:v1 {}\n", &hex_part[1..]), malformed),
            (format!("securacv-share:v1  {hex_part}\n"), malformed),
            ("securacv-share:v1 \n".to_string(), malformed),
            (format!("securacv-share:v2 {hex_part}\n"), not_one),
            (format!("securacv-share:v1\t{hex_part}\n"), not_one),
            (format!(" securacv-share:v1 {hex_part}\n"), not_one),
            ("securacv-share:v1".to_string(), not_one),
            (String::new(), not_one),
        ];
        for (input, want) in cases {
            assert_eq!(
                ShareFile::from_text(input.as_bytes())
                    .unwrap_err()
                    .to_string(),
                want,
                "{input:?}"
            );
        }
        let mut long = text.into_bytes();
        long.resize(SHARE_TEXT_MAX_BYTES + 1, b'0');
        assert_eq!(
            ShareFile::from_text(&long).unwrap_err().to_string(),
            "share file: longer than 16384 bytes"
        );
    }

    #[test]
    fn bounds_table() {
        let split = test_split(3, 5, ROLE_OPERATIONAL);
        let file = split.sealed(2);
        let bin = file.encode();
        let hl = 133 + "bob".len();
        let set = |at: usize, v: u8| {
            let mut b = bin.clone();
            b[at] = v;
            refile(b)
        };
        let hdr = |f: &dyn Fn(&mut ShareHeader)| {
            let mut h = file.header.clone();
            f(&mut h);
            with_header(&file, h)
        };
        let mut c1_bad = bin.clone();
        c1_bad[hl + 32..hl + 64].copy_from_slice(&[0xff; 32]);
        let mut c2_zero = bin.clone();
        c2_zero[hl + 64..hl + 96].copy_from_slice(&[0; 32]);
        let mut c0_zero = bin.clone();
        c0_zero[25..57].copy_from_slice(&[0; 32]);
        let roster_at = hl + 32 + 64;
        let body_at = file.header_bytes().len();
        let mut long_enc = bin[..body_at].to_vec();
        long_enc.extend_from_slice(&31u16.to_le_bytes());
        long_enc.extend_from_slice(&[0; 31]);
        long_enc.extend_from_slice(&80u16.to_le_bytes());
        long_enc.extend_from_slice(&[0; 80 + 32]);
        let box2_enc32 = set(6, PROTECTION_TRANSPORT_BOX2);
        let mut ct79 = bin.clone();
        ct79[body_at + 2 + 32..body_at + 2 + 32 + 2].copy_from_slice(&79u16.to_le_bytes());
        let mut trailing = bin.clone();
        trailing.push(0);
        let mut damaged = bin.clone();
        let last = damaged.len() - 1;
        damaged[last] ^= 1;

        let table: Vec<(Vec<u8>, &str)> = vec![
            (set(0, b'X'), "bad magic"),
            (set(4, 2), "unsupported version 2"),
            (set(5, 2), "unsupported suite 2"),
            (set(6, 0), "unknown protection 0"),
            (set(6, 4), "unknown protection 4"),
            (set(7, 0), "unknown role 0"),
            (set(7, 4), "unknown role 4"),
            (set(8, 3), "unknown release box 3"),
            (vec![0; SHARE_MAX_BYTES + 1], "longer than 16384 bytes"),
            (bin[..200].to_vec(), "truncated"),
            (bin[..bin.len() - 1].to_vec(), "truncated"),
            (trailing, "trailing bytes"),
            (
                set(129, 1),
                "threshold must be between 2 and 32 and at most the holder count",
            ),
            (
                set(129, 6),
                "threshold must be between 2 and 32 and at most the holder count",
            ),
            (
                set(130, 33),
                "threshold must be between 2 and 32 and at most the holder count",
            ),
            (set(131, 0), "index must be between 1 and 5"),
            (set(131, 6), "index must be between 1 and 5"),
            (
                set(132, 0),
                "holder id must be 1 to 64 bytes of [A-Za-z0-9._-]",
            ),
            (
                set(132, 65),
                "holder id must be 1 to 64 bytes of [A-Za-z0-9._-]",
            ),
            (
                set(133, b'/'),
                "holder id must be 1 to 64 bytes of [A-Za-z0-9._-]",
            ),
            (
                refile(c1_bad),
                "commitment 1 is not a canonical ristretto255 encoding",
            ),
            (refile(c2_zero), "commitment 2 is the identity"),
            (refile(c0_zero), "commitment 0 is the identity"),
            (
                set(roster_at, 4),
                "the roster does not list indices 1..=5 exactly once",
            ),
            (
                set(roster_at + 1, 2),
                "the roster does not list indices 1..=5 exactly once",
            ),
            (
                hdr(&|h| h.roster[3].signing_public_key = h.roster[0].signing_public_key),
                "the roster repeats an id or a key",
            ),
            (
                hdr(&|h| h.roster[4].id = "alice".to_string()),
                "the roster repeats an id or a key",
            ),
            (
                hdr(&|h| h.roster[1].recipient_key_hash = [9; 32]),
                "the holder entry does not match the roster entry at index 2",
            ),
            (
                hdr(&|h| h.container_sha256 = [9; 32]),
                "the container copy does not match its recorded hash",
            ),
            (
                hdr(&|h| h.container = vec![0; CONTAINER_COPY_MAX + 1]),
                "container longer than 8192 bytes",
            ),
            (
                refile(long_enc),
                "encapsulation length 31 does not match protection 1",
            ),
            (
                box2_enc32,
                "encapsulation length 32 does not match protection 2",
            ),
            (refile(ct79), "ciphertext length must be 80"),
            (damaged, "file check mismatch — the file is damaged"),
            (
                hdr(&|h| {
                    h.role = ROLE_REHEARSAL;
                    h.release_box = 0;
                }),
                "a rehearsal share carries a device key, container or release box",
            ),
            (
                hdr(&|h| h.device_public_key = [0; 32]),
                "an operational or recovery share lacks its device key or container",
            ),
            (
                hdr(&|h| h.role = ROLE_RECOVERY),
                "a recovery share names a release box",
            ),
            (
                hdr(&|h| h.issued_bucket_start = BUCKET + 1),
                "the issued bucket must be a multiple of 600 seconds",
            ),
        ];
        for (input, want) in &table {
            assert_eq!(reason(input), format!("share file: {want}"));
        }
        // The Argon2id parameter refusal (test 30 covers it in full).
        let share = file.open_transport(&split.secrets[1]).unwrap();
        let at_rest = file
            .protect_with_kek(
                &share,
                &[7; 32],
                crate::vault::custody::tv::<16>("08"),
                crate::vault::custody::tv::<12>("09"),
            )
            .unwrap()
            .encode();
        let mut p2 = at_rest.clone();
        p2[body_at + 8] = 2;
        assert_eq!(
            reason(&refile(p2)),
            "share file: unexpected Argon2id parameters"
        );
        // Text-form reasons.
        assert_eq!(
            ShareFile::from_text(b"hello\n").unwrap_err().to_string(),
            "share file: not a securacv-share:v1 file"
        );
        assert_eq!(
            ShareFile::from_text(b"securacv-share:v1 zz\n")
                .unwrap_err()
                .to_string(),
            "share file: the hex form is malformed"
        );
        // An operational share without a release box is refused as an
        // unknown box.
        assert_eq!(
            reason(&hdr(&|h| h.release_box = 0)),
            "share file: unknown release box 0"
        );
        // The worst case of spec §2.5 (32 holders with 64-byte ids, n = 32,
        // a full container copy, box 2) stays under the cap.
        let worst =
            133 + 64 + 32 + 32 * 31 + 1 + 32 * (2 + 64 + 64) + 2 + 8192 + 2 + 1120 + 2 + 80 + 32;
        assert!(worst <= SHARE_MAX_BYTES, "{worst}");
    }

    #[test]
    fn roster_must_list_every_index_once_and_match_the_holder() {
        let split = test_split(2, 4, ROLE_OPERATIONAL);
        let file = split.sealed(3);
        let refuse = |h: ShareHeader| reason(&with_header(&file, h));
        let mut h = file.header.clone();
        h.roster.swap(0, 1);
        assert_eq!(
            refuse(h),
            "share file: the roster does not list indices 1..=4 exactly once"
        );
        let mut h = file.header.clone();
        h.roster.pop();
        assert_eq!(
            refuse(h),
            "share file: the roster does not list indices 1..=4 exactly once"
        );
        let mut h = file.header.clone();
        h.roster[1].index = 1;
        assert_eq!(
            refuse(h),
            "share file: the roster does not list indices 1..=4 exactly once"
        );
        let mut h = file.header.clone();
        h.roster[2].id = "mallory".to_string();
        assert_eq!(
            refuse(h),
            "share file: the holder entry does not match the roster entry at index 3"
        );
        let mut h = file.header.clone();
        h.recipient_key_hash = h.roster[0].recipient_key_hash;
        assert_eq!(
            refuse(h),
            "share file: the holder entry does not match the roster entry at index 3"
        );
        let mut h = file.header.clone();
        h.roster[0].recipient_key_hash = h.roster[1].recipient_key_hash;
        assert_eq!(refuse(h), "share file: the roster repeats an id or a key");
        // The pinned roster is what the file says, entry for entry.
        let decoded = ShareFile::decode(&file.encode()).unwrap();
        let ids: Vec<&str> = decoded
            .header
            .roster
            .iter()
            .map(|e| e.id.as_str())
            .collect();
        assert_eq!(ids, ["alice", "bob", "carol", "dave"]);
        assert_eq!(decoded.header.own_entry().unwrap().id, "carol");
        for (e, p) in decoded.header.roster.iter().zip(&split.publics) {
            assert_eq!(e.signing_public_key, p.signing_public_key().unwrap());
            assert_eq!(e.recipient_key_hash, p.key_hash().unwrap());
        }
    }

    #[test]
    fn container_copy_hash_is_checked() {
        let split = test_split(2, 3, ROLE_OPERATIONAL);
        let file = split.sealed(1);
        let mut h = file.header.clone();
        h.container[10] ^= 1;
        assert_eq!(
            reason(&with_header(&file, h)),
            "share file: the container copy does not match its recorded hash"
        );
        let mut h = file.header.clone();
        h.container.push(0);
        assert_eq!(
            reason(&with_header(&file, h)),
            "share file: the container copy does not match its recorded hash"
        );
        // The copy decodes as the container it is, and names the same KEK.
        let mkc1 =
            crate::vault::custody::container::decode_mkc1_header(&file.header.container).unwrap();
        assert_eq!(mkc1.kek_commitment, file.header.kek_commitment());
        let got: [u8; 32] = Sha256::digest(&file.header.container).into();
        assert_eq!(got, file.header.container_sha256);
    }

    #[test]
    fn rehearsal_requires_zero_device_container_and_box() {
        let split = test_split(2, 3, ROLE_REHEARSAL);
        let file = split.sealed(1);
        assert!(file.header.container.is_empty());
        assert_eq!(file.header.device_public_key, [0; 32]);
        assert_eq!(file.header.container_sha256, [0; 32]);
        assert_eq!(file.header.release_box, 0);
        assert_eq!(ShareFile::decode(&file.encode()).unwrap(), file);
        let carries =
            "share file: a rehearsal share carries a device key, container or release box";
        let cases: [&dyn Fn(&mut ShareHeader); 3] = [
            &|h| h.device_public_key = [1; 32],
            &|h| h.release_box = BOX_HPKE_X25519,
            &|h| h.container_sha256 = [1; 32],
        ];
        for f in cases {
            let mut h = file.header.clone();
            f(&mut h);
            assert_eq!(reason(&with_header(&file, h)), carries);
        }
        let other = test_split(2, 3, ROLE_OPERATIONAL);
        let mut h = file.header.clone();
        h.container = other.container.clone();
        assert_eq!(reason(&with_header(&file, h)), carries);
        // A recovery share must carry a device key and a container.
        let recovery = test_split(2, 3, ROLE_RECOVERY);
        let rfile = recovery.sealed(2);
        assert_eq!(ShareFile::decode(&rfile.encode()).unwrap(), rfile);
        let mut h = rfile.header.clone();
        h.container = Vec::new();
        h.container_sha256 = Sha256::digest([]).into();
        assert_eq!(
            reason(&with_header(&rfile, h)),
            "share file: an operational or recovery share lacks its device key or container"
        );
    }

    #[test]
    fn at_rest_params_refused_before_kdf() {
        // The pattern of vault::crypto's keyguard_rejects_out_of_spec_params:
        // a hostile file that asks for an enormous Argon2id cost is refused
        // when it is decoded, so no passphrase is ever run through the KDF.
        let split = test_split(2, 3, ROLE_OPERATIONAL);
        let file = split.sealed(1);
        let share = file.open_transport(&split.secrets[0]).unwrap();
        let at_rest = file
            .protect_with_kek(
                &share,
                &[7; 32],
                crate::vault::custody::tv::<16>("08"),
                crate::vault::custody::tv::<12>("09"),
            )
            .unwrap();
        let bin = at_rest.encode();
        let body_at = at_rest.header_bytes().len();
        assert_eq!(
            &bin[body_at..body_at + 12],
            &[0, 0, 1, 0, 3, 0, 0, 0, 1, 0, 0, 0]
        );
        for (off, v) in [(0usize, 9_999_999u32), (4, 1), (8, 4), (0, 65_535)] {
            let mut b = bin.clone();
            b[body_at + off..body_at + off + 4].copy_from_slice(&v.to_le_bytes());
            assert_eq!(
                reason(&refile(b)),
                "share file: unexpected Argon2id parameters"
            );
        }
    }

    #[cfg(feature = "pqc-vault")]
    #[test]
    fn round_trip_transport_box2_with_mlkem() {
        // A real ML-KEM-768 recipient key: the share seals with box 2 and
        // opens with both halves of the recipient key.
        let mut split = test_split(2, 3, ROLE_OPERATIONAL);
        let secret = RecipientSecret::generate(
            "alice",
            HOLDER_ROLE_TRUSTEE,
            &spk_of(&crate::vault::custody::tv::<32>("01")),
            true,
            &mut OsCustodyRng,
        )
        .unwrap();
        split.publics[0] = signed_public(&secret, &crate::vault::custody::tv::<32>("01"));
        split.secrets[0] = secret;
        let mut header = split.header(1, PROTECTION_TRANSPORT_BOX2);
        header.release_box = BOX_HPKE_X25519_PSK_MLKEM768;
        let file = ShareFile::seal_transport(
            header,
            &split.shares[0],
            &split.publics[0],
            &mut OsCustodyRng,
        )
        .unwrap();
        let ShareBody::Transport { enc, .. } = &file.body else {
            panic!("a transport share");
        };
        assert_eq!(enc.len(), BOX2_ENC_LEN);
        let decoded = ShareFile::decode(&file.encode()).unwrap();
        let opened = decoded.open_transport(&split.secrets[0]).unwrap();
        assert_eq!(opened.scalars(), split.shares[0].scalars());
        // The X25519 half alone does not open it.
        let x_only = RecipientSecret::from_parts(
            "alice",
            HOLDER_ROLE_TRUSTEE,
            *split.secrets[0].signing_public_key(),
            *split.secrets[0].x25519_secret(),
            None,
        );
        assert!(decoded.open_transport(&x_only).is_err());
    }

    #[cfg(not(feature = "pqc-vault"))]
    #[test]
    fn box2_without_feature_message() {
        let want = "custody: this share is sealed with HPKE X25519 and ML-KEM-768 (box 2); \
                    this build has no pqc-vault feature to open or seal it";
        let split = test_split(2, 3, ROLE_OPERATIONAL);
        let err = ShareFile::seal_transport(
            split.header(1, PROTECTION_TRANSPORT_BOX2),
            &split.shares[0],
            &split.publics[0],
            &mut OsCustodyRng,
        )
        .unwrap_err();
        assert_eq!(err.to_string(), want);
        // A box-2 file (built through the HPKE-layer seam, no ML-KEM) is
        // read, inspected and refused at open with the same sentence.
        let file = ShareFile::seal_transport_box2_layer_derand(
            split.header(1, PROTECTION_TRANSPORT_BOX2),
            &split.shares[0],
            &split.publics[0],
            &[5; 32],
            &[6; crate::vault::custody::hpke::MLKEM768_CT_LEN],
            &crate::vault::custody::tv::<32>("07"),
        )
        .unwrap();
        let decoded = ShareFile::decode(&file.encode()).unwrap();
        assert_eq!(decoded.header.protection, PROTECTION_TRANSPORT_BOX2);
        assert_eq!(
            decoded
                .open_transport(&split.secrets[0])
                .unwrap_err()
                .to_string(),
            want
        );
        // The layer itself opens with the right shared secret.
        let opened = decoded
            .open_transport_box2_layer(&split.secrets[0], &[5; 32])
            .unwrap();
        assert_eq!(opened.scalars(), split.shares[0].scalars());
    }
}
