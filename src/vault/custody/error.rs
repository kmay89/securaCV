//! The custody core's one error type. Every `Display` string is a closed
//! sentence: none carries a scalar, a key, a share, a passphrase, a path or
//! a full hash (`kat::tests::no_error_string_contains_a_fixture_secret`).

use std::fmt;

/// Why a share file was refused. Each prints after `share file: `.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum ShareReason {
    BadMagic,
    UnsupportedVersion(u8),
    UnsupportedSuite(u8),
    UnknownProtection(u8),
    UnknownRole(u8),
    UnknownReleaseBox(u8),
    TooLong,
    Truncated,
    TrailingBytes,
    Threshold,
    Index(u8),
    HolderId,
    CommitmentNotCanonical(usize),
    CommitmentIdentity(usize),
    RosterIndices(u8),
    RosterRepeats,
    HolderEntryMismatch(u8),
    ContainerHash,
    ContainerTooLong,
    EncapsulationLength(usize, u8),
    CiphertextLength,
    ArgonParameters,
    FileCheck,
    RehearsalCarries,
    OperationalLacks,
    RecoveryReleaseBox,
    NotShareText,
    HexMalformed,
    IssuedBucket,
}

impl fmt::Display for ShareReason {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        use ShareReason::*;
        match self {
            BadMagic => write!(f, "bad magic"),
            UnsupportedVersion(v) => write!(f, "unsupported version {v}"),
            UnsupportedSuite(s) => write!(f, "unsupported suite {s}"),
            UnknownProtection(p) => write!(f, "unknown protection {p}"),
            UnknownRole(r) => write!(f, "unknown role {r}"),
            UnknownReleaseBox(b) => write!(f, "unknown release box {b}"),
            TooLong => write!(f, "longer than 16384 bytes"),
            Truncated => write!(f, "truncated"),
            TrailingBytes => write!(f, "trailing bytes"),
            Threshold => write!(
                f,
                "threshold must be between 2 and 32 and at most the holder count"
            ),
            Index(m) => write!(f, "index must be between 1 and {m}"),
            HolderId => write!(f, "holder id must be 1 to 64 bytes of [A-Za-z0-9._-]"),
            CommitmentNotCanonical(j) => {
                write!(f, "commitment {j} is not a canonical ristretto255 encoding")
            }
            CommitmentIdentity(j) => write!(f, "commitment {j} is the identity"),
            RosterIndices(m) => write!(f, "the roster does not list indices 1..={m} exactly once"),
            RosterRepeats => write!(f, "the roster repeats an id or a key"),
            HolderEntryMismatch(i) => write!(
                f,
                "the holder entry does not match the roster entry at index {i}"
            ),
            ContainerHash => write!(f, "the container copy does not match its recorded hash"),
            ContainerTooLong => write!(f, "container longer than 8192 bytes"),
            EncapsulationLength(e, p) => {
                write!(f, "encapsulation length {e} does not match protection {p}")
            }
            CiphertextLength => write!(f, "ciphertext length must be 80"),
            ArgonParameters => write!(f, "unexpected Argon2id parameters"),
            FileCheck => write!(f, "file check mismatch — the file is damaged"),
            RehearsalCarries => write!(
                f,
                "a rehearsal share carries a device key, container or release box"
            ),
            OperationalLacks => write!(
                f,
                "an operational or recovery share lacks its device key or container"
            ),
            RecoveryReleaseBox => write!(f, "a recovery share names a release box"),
            NotShareText => write!(f, "not a securacv-share:v1 file"),
            HexMalformed => write!(f, "the hex form is malformed"),
            IssuedBucket => write!(f, "the issued bucket must be a multiple of 600 seconds"),
        }
    }
}

/// Everything the custody core refuses.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum CustodyError {
    /// A share file's closed refusal reason.
    ShareFile(ShareReason),
    /// A commitment that does not decode (outside a share file).
    PointNotCanonical(usize),
    /// A commitment that is the identity (outside a share file).
    PointIdentity(usize),
    /// A 32-byte scalar at or above the group order.
    ScalarNotCanonical,
    /// A dealing whose parameters are out of bounds.
    DealParameters,
    /// Reconstruction was given the wrong number of shares.
    WrongShareCount { given: usize, needed: u8 },
    /// Reconstruction or Lagrange was given a zero, duplicate or
    /// out-of-range index.
    BadIndices { m: u8 },
    /// A share fails its Pedersen check.
    ShareMismatch(u8),
    /// The rebuilt `(k, r)` does not commit to the split's KEK commitment.
    KekMismatch,
    /// Deal-and-prove found a fault; nothing was written.
    DealAndProve(String),
    /// X25519 produced the all-zero output.
    HpkeZeroDh,
    /// An HPKE open failed (wrong key, info, AAD, PSK, or altered bytes).
    HpkeOpen,
    /// HPKE PSK inputs given without each other, or to base mode.
    HpkePskInputs,
    /// A passphrase does not open an at-rest share or a TEK1 file.
    WrongPassphrase,
    /// A share wrapped at rest failed its AEAD.
    AtRestAuthentication,
    /// A box-2 operation in a build without `pqc-vault`.
    #[cfg_attr(feature = "pqc-vault", allow(dead_code))]
    Box2NeedsFeature,
    /// An ML-KEM-768 operation failed (a malformed key or ciphertext).
    MlKem,
    /// The custody container refused (`custody container: {why}`).
    Container(&'static str),
    /// `custody container: ciphertext length {n} is out of bounds`.
    ContainerCiphertextLength(u32),
    /// A recipient public file refused (`recipient file: {why}`).
    Recipient(&'static str),
    /// A recipient secret file refused (`recipient key file: {why}`).
    RecipientKey(&'static str),
    /// `recipient key file: unsupported version {v}`.
    RecipientKeyVersion(u8),
    /// A split record refused (`split record: {why}`).
    SplitRecord(&'static str),
    /// A transport share opened with another holder's recipient key; the
    /// two 8-hex prefixes name the recorded and the supplied key hashes.
    WrongRecipient { recorded: String, yours: String },
    /// A share whose roster pins, under the holder's own name and for the
    /// holder's own recipient key, a signing key other than the one the
    /// holder's recipient key file records (a faulty dealer).
    PinnedSigningKeyDiffers,
    /// A share asked to open in a way its protection does not allow.
    WrongProtection,
    /// A share header that does not describe the share, recipient or
    /// split it is sealed with.
    SealMismatch,
}

impl From<ShareReason> for CustodyError {
    fn from(r: ShareReason) -> Self {
        CustodyError::ShareFile(r)
    }
}

impl fmt::Display for CustodyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        use CustodyError::*;
        match self {
            ShareFile(r) => write!(f, "share file: {r}"),
            PointNotCanonical(j) => {
                write!(f, "commitment {j} is not a canonical ristretto255 encoding")
            }
            PointIdentity(j) => write!(f, "commitment {j} is the identity"),
            ScalarNotCanonical => write!(
                f,
                "custody: a scalar is not a canonical encoding (it must be below the group order)"
            ),
            DealParameters => write!(
                f,
                "custody: a split needs 2 <= n <= m <= 32 and distinct holder indices between 1 and 32"
            ),
            WrongShareCount { given, needed } => write!(
                f,
                "custody: {given} shares given; this split needs exactly {needed}"
            ),
            BadIndices { m } => write!(
                f,
                "custody: share indices must be distinct and between 1 and {m}"
            ),
            ShareMismatch(i) => write!(
                f,
                "custody: share {i} does not match the split's commitments"
            ),
            KekMismatch => write!(
                f,
                "custody: the key rebuilt from the shares does not match the split's KEK commitment"
            ),
            DealAndProve(which) => {
                write!(f, "deal-and-prove failed: {which} — nothing was written")
            }
            HpkeZeroDh => write!(
                f,
                "HPKE: X25519 produced an all-zero shared secret (invalid public key)"
            ),
            HpkeOpen => write!(f, "HPKE: open failed"),
            HpkePskInputs => write!(
                f,
                "HPKE: a PSK and its id must be given together, and only in PSK mode"
            ),
            WrongPassphrase => write!(f, "custody: wrong passphrase"),
            AtRestAuthentication => write!(
                f,
                "custody: the share wrapped at rest failed authentication — the file was altered"
            ),
            Box2NeedsFeature => write!(
                f,
                "custody: this share is sealed with HPKE X25519 and ML-KEM-768 (box 2); \
                 this build has no pqc-vault feature to open or seal it"
            ),
            MlKem => write!(f, "custody: an ML-KEM-768 key or ciphertext is malformed"),
            Container(why) => write!(f, "custody container: {why}"),
            ContainerCiphertextLength(n) => write!(
                f,
                "custody container: ciphertext length {n} is out of bounds (17 to 4096)"
            ),
            Recipient(why) => write!(f, "recipient file: {why}"),
            RecipientKey(why) => write!(f, "recipient key file: {why}"),
            RecipientKeyVersion(v) => write!(f, "recipient key file: unsupported version {v}"),
            SplitRecord(why) => write!(f, "split record: {why}"),
            WrongRecipient { recorded, yours } => write!(
                f,
                "cannot open this share with the supplied recipient key: it was encrypted to \
                 recipient {recorded}…, yours is {yours}…"
            ),
            PinnedSigningKeyDiffers => write!(
                f,
                "custody: the roster in this share pins a different signing key for you than \
                 your recipient key records"
            ),
            WrongProtection => write!(
                f,
                "custody: this share is not protected that way (a transport share opens with a \
                 recipient key, a protected one with its passphrase)"
            ),
            SealMismatch => write!(
                f,
                "custody: the share header does not describe this share, recipient or split"
            ),
        }
    }
}

impl std::error::Error for CustodyError {}
