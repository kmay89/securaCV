//! The holder-side threshold-custody commands (spec `quorum_unseal_v2.md`
//! §2; design record `docs/design/threshold_custody.md`):
//!
//! - `custody recipient keygen`: a holder makes the recipient key shares are
//!   sealed to, bound to their signing key by a signature;
//! - `custody rehearse`: a throwaway split dealt to real recipient keys, so
//!   holders can practice checking and protecting a share;
//! - `share inspect`, `share verify`, `share protect`: a holder reads, checks
//!   and rewraps their share on their own machine, with no hub involved.
//!
//! None of these commands touches a vault, a policy, a receipt or the
//! database: nothing here can put a vault under custody. Every printed block
//! is built by a pure function so the tests pin each line. No line carries a
//! path, a full hash, a full key or a secret; hashes and keys print as
//! 16-hex prefixes, and a Pedersen check is reported as "checks passed",
//! never as "verified" (that word is reserved for an Ed25519 signature
//! checked against a pinned key).

use std::io::Read;
use std::path::Path;

use anyhow::{anyhow, Result};
use clap::Subcommand;
use subtle::ConstantTimeEq;

use super::cli::{write_new_secret_file, OUTPUT_EXISTS};
use super::trustee_key::{load_trustee_signer, TrusteeSigner};
use crate::crypto::passphrase::{PassphraseError, PassphraseInput};
use crate::crypto::signatures::DOMAIN_CUSTODY_RECIPIENT_BINDING;
use crate::vault::custody::container::decode_mkc1_header;
use crate::vault::custody::group::{CustodyRng, OsCustodyRng};
use crate::vault::custody::recipient::{
    role_name as holder_role_name, RecipientPublic, RecipientSecret, HOLDER_ROLE_RECOVERY,
    HOLDER_ROLE_TRUSTEE, RECIPIENT_FILE_MAX, RECIPIENT_KEY_FILE_MAX,
};
use crate::vault::custody::share::{RosterEntry, ShareBody, ShareFile, ShareHeader};
use crate::vault::custody::split_record::{SplitRecord, SPLIT_RECORD_MAX};
use crate::vault::custody::vss::{check_share, deal_and_prove, Kek};
use crate::vault::custody::{
    hex16, hex8, holder_id_ok, CustodyError, MAX_HOLDERS, MIN_THRESHOLD, PROTECTION_AT_REST,
    PROTECTION_TRANSPORT_BOX1, PROTECTION_TRANSPORT_BOX2, ROLE_OPERATIONAL, ROLE_RECOVERY,
    ROLE_REHEARSAL, SHARE_TEXT_MAX_BYTES, SUITE_V1,
};

/// `break_glass custody …`
#[derive(Subcommand, Debug)]
pub(crate) enum CustodyCommand {
    /// Recipient keys: run by each custody holder on their own machine
    Recipient {
        #[command(subcommand)]
        command: RecipientCommand,
    },
    /// Deal a REHEARSAL split of a throwaway key that protects nothing to the
    /// given recipient keys, so holders can practice `share verify` and
    /// `share protect`. Touches no vault, policy or database.
    Rehearse {
        /// Any this many shares open the throwaway key (2..=recipients)
        #[arg(long)]
        threshold: u8,
        /// The holders' recipient public files (`<holder>.recipient`), in
        /// index order
        #[arg(long = "recipient", value_name = "PATH", num_args = 1.., required = true)]
        recipients: Vec<String>,
        /// An empty (or new) directory for the share files and split record
        #[arg(long, value_name = "DIR")]
        output_dir: String,
    },
}

/// `break_glass custody recipient …`
#[derive(Subcommand, Debug)]
pub(crate) enum RecipientCommand {
    /// Make the recipient key your custody shares are sealed to: an X25519
    /// key (plus ML-KEM-768 with --pq), bound to your signing key by a
    /// signature, the secret part wrapped under a new passphrase (Argon2id).
    /// With --passphrase-fd, the lines are: your signing key's passphrase
    /// (only if that key is wrapped), then the new passphrase.
    Keygen {
        /// Your holder id (the id the quorum policy names you by)
        #[arg(long)]
        holder: String,
        /// `trustee` (operational split) or `recovery` (recovery split)
        #[arg(long, value_parser = ["trustee", "recovery"])]
        role: String,
        /// Your trustee signing key (`trustee keygen` output, or a legacy
        /// plaintext key)
        #[arg(long, value_name = "PATH")]
        signing_key: String,
        /// Also make an ML-KEM-768 key (box 2; needs a pqc-vault build)
        #[arg(long)]
        pq: bool,
        /// Read passphrases from this file descriptor, one per line, instead
        /// of the terminal. Never from argv or the environment.
        #[arg(long, value_name = "N")]
        passphrase_fd: Option<i32>,
        /// Where to write the secret part (mode 0600); never overwritten
        #[arg(long, value_name = "PATH")]
        output_key: String,
        /// Where to write the public part for the operator (mode 0644);
        /// never overwritten
        #[arg(long, value_name = "PATH")]
        output_public: String,
    },
}

/// `break_glass share …`
#[derive(Subcommand, Debug)]
pub(crate) enum ShareCommand {
    /// Show what a share file says about itself. Needs no key.
    Inspect {
        /// The share file
        #[arg(long, value_name = "PATH")]
        share: String,
    },
    /// Open your share with your recipient key (a transport share) or your
    /// passphrase (`--at-rest`), and check it against its split's public
    /// commitments and, if given, the split record you were handed.
    Verify {
        /// The share file
        #[arg(long, value_name = "PATH")]
        share: String,
        /// Your recipient key file (for a transport share)
        #[arg(long, value_name = "PATH", conflicts_with = "at_rest")]
        recipient_key: Option<String>,
        /// The share is wrapped at rest under your passphrase
        #[arg(long)]
        at_rest: bool,
        /// The split record (`<split8>.split.json`) to compare against
        #[arg(long, value_name = "PATH")]
        split_record: Option<String>,
        /// Read the passphrase from this file descriptor (one line) instead
        /// of the terminal
        #[arg(long, value_name = "N")]
        passphrase_fd: Option<i32>,
    },
    /// Rewrap a transport share under your own passphrase (Argon2id), so it
    /// no longer depends on your recipient key at rest. With
    /// --passphrase-fd, line 1 is the recipient key's passphrase and line 2
    /// the new share passphrase.
    Protect {
        /// The transport share file
        #[arg(long, value_name = "PATH")]
        share: String,
        /// Your recipient key file
        #[arg(long, value_name = "PATH")]
        recipient_key: String,
        /// Read passphrases from this file descriptor instead of the terminal
        #[arg(long, value_name = "N")]
        passphrase_fd: Option<i32>,
        /// Where to write the protected share (mode 0600); never overwritten
        #[arg(long, value_name = "PATH")]
        output: String,
    },
}

/// The refusal of `--pq` in a build without ML-KEM.
pub(crate) const PQ_NEEDS_FEATURE: &str = "--pq needs a build with the pqc-vault feature";
/// The refusal when a new recipient key's passphrase has no source.
const NO_NEW_RECIPIENT_PASSPHRASE: &str =
    "a new recipient key needs a passphrase: run on a terminal or pass --passphrase-fd";
/// The refusal when a passphrase is needed and has no source.
const NO_PASSPHRASE: &str = "this needs a passphrase: run on a terminal or pass --passphrase-fd";
/// `share verify` given the wrong kind of key for the share.
pub(crate) const VERIFY_NEEDS_KEY: &str =
    "share verify needs --recipient-key for a transport share and --at-rest for a protected one";
/// The share fails its Pedersen check.
pub(crate) const SHARE_MISMATCH: &str = "share check FAILED: the share does not match its split's commitments — do not acknowledge it; record a class-d-security exception (the dealer or the file is faulty)";
/// The share's commitments are not the split record's.
pub(crate) const RECORD_COMMITMENTS_DIFFER: &str = "share check FAILED: the share's commitments differ from the split record you were given — do not acknowledge it; the dealer showed holders different commitments";
/// The commitments agree but some other pin (holders, container, device
/// key, bucket) does not.
pub(crate) const RECORD_SPLIT_DIFFERS: &str = "share check FAILED: the share's split differs from the split record you were given (its holders, container, device key or bucket) — do not acknowledge it";
/// The container copy is sealed under another KEK than the share's.
pub(crate) const CONTAINER_OTHER_KEK: &str = "share check FAILED: the container copy names a different KEK than the share — do not acknowledge it; record a class-d-security exception";
/// A wrong share passphrase.
pub(crate) const WRONG_SHARE_PASSPHRASE: &str = "cannot open this share: wrong passphrase";
/// `share protect` of a share that is already protected.
pub(crate) const PROTECT_NEEDS_TRANSPORT: &str =
    "share protect needs a transport share; this one is already wrapped at rest";
/// `custody rehearse` into a directory that holds files.
pub(crate) const REHEARSE_DIR_NOT_EMPTY: &str =
    "custody rehearse: the output directory is not empty";
/// `custody rehearse` with two recipients that share an id or a key.
pub(crate) const REHEARSE_DUPLICATE: &str =
    "custody rehearse: two recipient files name the same holder or key";

/// Read at most `cap` bytes of a file, refusing a longer one before reading
/// it all. Errors name no path.
fn read_capped(path: &str, cap: usize, what: &str) -> Result<Vec<u8>> {
    let file = std::fs::File::open(path).map_err(|e| anyhow!("failed to read the {what}: {e}"))?;
    let mut out = Vec::new();
    file.take(cap as u64 + 1)
        .read_to_end(&mut out)
        .map_err(|e| anyhow!("failed to read the {what}: {e}"))?;
    if out.len() > cap {
        return Err(anyhow!("the {what} is longer than {cap} bytes"));
    }
    Ok(out)
}

/// Write a NEW public file, mode 0644, never overwriting and never through
/// a symlink. Errors name no path.
fn write_new_public_file(path: &str, bytes: &[u8]) -> Result<()> {
    use std::io::Write;
    let mut options = std::fs::OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o644).custom_flags(libc::O_NOFOLLOW);
    }
    let mut file = options.open(path).map_err(|e| {
        if e.kind() == std::io::ErrorKind::AlreadyExists {
            anyhow!("{OUTPUT_EXISTS}")
        } else {
            anyhow!("failed to create the file: {e}")
        }
    })?;
    file.write_all(bytes)
        .and_then(|()| file.sync_all())
        .map_err(|e| anyhow!("failed to write the file: {e}"))
}

fn no_source(e: PassphraseError, refusal: &str) -> anyhow::Error {
    match e {
        PassphraseError::NoSource => anyhow!("{refusal}"),
        PassphraseError::Failed(e) => e,
    }
}

fn exists(path: &str) -> bool {
    Path::new(path).symlink_metadata().is_ok()
}

/// The UTC minute of a bucket start: `YYYY-MM-DD HH:MM`.
fn utc_minute(epoch_s: u64) -> String {
    let (y, m, d, hh, mm, _) = crate::epoch_civil_utc(epoch_s);
    format!("{y:04}-{m:02}-{d:02} {hh:02}:{mm:02}")
}

/// A label padded with a dot leader to a fixed column, then `OK`.
fn ok_line(label: &str) -> String {
    let dots = 58usize.saturating_sub(label.chars().count() + 1).max(3);
    format!("  {label} {} OK\n", ".".repeat(dots))
}

// ---------------------------------------------------------------------------
// custody recipient keygen
// ---------------------------------------------------------------------------

/// What `custody recipient keygen` produces: the two files' text and the
/// printed block.
pub(crate) struct KeygenOutput {
    /// The secret file's line (TEK1, under the holder's passphrase).
    pub(crate) key_text: zeroize::Zeroizing<String>,
    /// The public file's line (JSON).
    pub(crate) public_text: String,
    /// What the command prints.
    pub(crate) lines: String,
}

/// What `custody recipient keygen` prints.
pub(crate) fn keygen_lines(
    holder: &str,
    role: u8,
    pq: bool,
    fingerprint: &str,
    signing_public_key: &[u8; 32],
) -> String {
    format!(
        "Recipient key for '{holder}' ({}): X25519{}\n  fingerprint {fingerprint} — read it aloud at the issuance ceremony\n  bound to signing key {}… by a signature under {DOMAIN_CUSTODY_RECIPIENT_BINDING}\n  the secret part is in the --output-key file, wrapped under your passphrase (Argon2id, mode 0600); give the --output-public file to the operator, never the key file\n",
        holder_role_name(role),
        if pq { " + ML-KEM-768" } else { "" },
        hex16(signing_public_key),
    )
}

/// Make a recipient key for `holder`, sign its binding with `signer`, wrap
/// the secret under `passphrase`, and prove both files open before they
/// are returned.
pub(crate) fn keygen_core(
    holder: &str,
    role: u8,
    pq: bool,
    signer: &dyn TrusteeSigner,
    passphrase: &str,
    rng: &mut dyn CustodyRng,
) -> Result<KeygenOutput> {
    if pq && !cfg!(feature = "pqc-vault") {
        return Err(anyhow!("{PQ_NEEDS_FEATURE}"));
    }
    let secret = RecipientSecret::generate(holder, role, pq, rng)?;
    let signing_public_key = signer.public_key();
    let unsigned = secret.public_unsigned(&signing_public_key);
    let hash = unsigned.key_hash()?;
    let signature = signer.sign(DOMAIN_CUSTODY_RECIPIENT_BINDING, &hash)?;
    let public = unsigned.with_binding(&signature);
    public.check_binding_ed25519()?;
    let key_text = zeroize::Zeroizing::new(secret.wrap(&signing_public_key, passphrase, rng)?);
    // Open what is about to be written: a key this tool reports as made
    // must open with the passphrase just given, to the same keys.
    let (reopened, header) = RecipientSecret::unwrap(key_text.as_bytes(), passphrase)?;
    let same: bool = reopened
        .x25519_secret()
        .ct_eq(secret.x25519_secret())
        .into();
    if !same || header.recipient_key_hash != hash {
        return Err(anyhow!(
            "recipient key file: the wrapped file did not open to the same key; nothing was written"
        ));
    }
    let public_text = public.to_json_text();
    RecipientPublic::from_json(public_text.as_bytes())?.check_binding_ed25519()?;
    Ok(KeygenOutput {
        key_text,
        public_text,
        lines: keygen_lines(
            holder,
            role,
            pq,
            &public.fingerprint()?,
            &signing_public_key,
        ),
    })
}

/// `break_glass custody recipient keygen`.
#[allow(clippy::too_many_arguments)]
pub(crate) fn cmd_recipient_keygen(
    holder: &str,
    role: &str,
    signing_key: &str,
    pq: bool,
    passphrase_fd: Option<i32>,
    output_key: &str,
    output_public: &str,
) -> Result<()> {
    if !holder_id_ok(holder) {
        return Err(anyhow!("holder id must be 1 to 64 bytes of [A-Za-z0-9._-]"));
    }
    if pq && !cfg!(feature = "pqc-vault") {
        return Err(anyhow!("{PQ_NEEDS_FEATURE}"));
    }
    let role = if role == "recovery" {
        HOLDER_ROLE_RECOVERY
    } else {
        HOLDER_ROLE_TRUSTEE
    };
    if output_key == output_public {
        return Err(anyhow!(
            "--output-key and --output-public must name different files"
        ));
    }
    if exists(output_key) || exists(output_public) {
        return Err(anyhow!("{OUTPUT_EXISTS}"));
    }
    let mut input = PassphraseInput::new(passphrase_fd).map_err(|e| anyhow!("{e}"))?;
    let signer = load_trustee_signer(signing_key, &mut input)?;
    let passphrase = input
        .read_new(
            &format!("new passphrase for recipient key '{holder}': "),
            "repeat it: ",
        )
        .map_err(|e| no_source(e, NO_NEW_RECIPIENT_PASSPHRASE))?;
    let out = keygen_core(
        holder,
        role,
        pq,
        signer.as_ref(),
        &passphrase,
        &mut OsCustodyRng,
    )?;
    write_new_secret_file(output_key, out.key_text.as_bytes())?;
    write_new_public_file(output_public, out.public_text.as_bytes())?;
    print!("{}", out.lines);
    Ok(())
}

// ---------------------------------------------------------------------------
// custody rehearse
// ---------------------------------------------------------------------------

/// One share a rehearsal wrote: index, holder, fingerprint.
pub(crate) struct RehearsalShare {
    pub(crate) index: u8,
    pub(crate) holder: String,
    pub(crate) fingerprint: String,
}

/// What `custody rehearse` prints.
pub(crate) fn rehearse_lines(
    split8: &str,
    n: u8,
    m: u8,
    shares: &[RehearsalShare],
    subsets: usize,
) -> String {
    let mut out = format!(
        "REHEARSAL split {split8}: any {n} of {m} shares open a throwaway key that protects nothing\n"
    );
    for s in shares {
        out.push_str(&format!(
            "  share {} → '{}' (fingerprint {})\n",
            s.index, s.holder, s.fingerprint
        ));
    }
    out.push_str(&format!(
        "every share passed its commitment check, and {subsets} subsets of {n} rebuilt the throwaway key before anything was written\n"
    ));
    out.push_str(&format!(
        "give each holder their share and {split8}.split.json; each runs: break_glass share verify --share <file> --recipient-key <file> --split-record {split8}.split.json\n"
    ));
    out
}

/// Deal a rehearsal split to `recipients` and write one share file each,
/// plus the split record, into `dir` (which must be empty or absent).
/// `bucket` is the issued 10-minute bucket start.
pub(crate) fn rehearse_core(
    threshold: u8,
    recipients: &[RecipientPublic],
    dir: &Path,
    bucket: u64,
    rng: &mut dyn CustodyRng,
) -> Result<String> {
    let m = recipients.len();
    if m > usize::from(MAX_HOLDERS) {
        return Err(anyhow!("custody rehearse: a split has at most 32 holders"));
    }
    let m = m as u8;
    if threshold < MIN_THRESHOLD || threshold > m {
        return Err(anyhow!(
            "custody rehearse: threshold must be between 2 and the number of recipients ({m})"
        ));
    }
    for r in recipients {
        if r.check_binding_ed25519().is_err() {
            return Err(anyhow!(
                "recipient file for '{}' carries a binding signature that does not check against its own signing key",
                r.holder()
            ));
        }
    }
    let mut roster = Vec::with_capacity(recipients.len());
    for (i, r) in recipients.iter().enumerate() {
        roster.push(RosterEntry {
            index: i as u8 + 1,
            id: r.holder().to_string(),
            signing_public_key: r.signing_public_key()?,
            recipient_key_hash: r.key_hash()?,
        });
    }
    for (i, a) in roster.iter().enumerate() {
        if roster[i + 1..].iter().any(|b| {
            a.id == b.id
                || a.signing_public_key == b.signing_public_key
                || a.recipient_key_hash == b.recipient_key_hash
        }) {
            return Err(anyhow!("{REHEARSE_DUPLICATE}"));
        }
    }
    if dir.exists() {
        let mut entries = std::fs::read_dir(dir)
            .map_err(|e| anyhow!("failed to read the output directory: {e}"))?;
        if entries.next().is_some() {
            return Err(anyhow!("{REHEARSE_DIR_NOT_EMPTY}"));
        }
    } else {
        let mut builder = std::fs::DirBuilder::new();
        builder.recursive(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::DirBuilderExt;
            builder.mode(0o700);
        }
        builder
            .create(dir)
            .map_err(|e| anyhow!("failed to create the output directory: {e}"))?;
    }
    let box2 = cfg!(feature = "pqc-vault")
        && recipients
            .iter()
            .all(|r| matches!(r.mlkem768_public(), Ok(Some(_))));
    let protection = if box2 {
        PROTECTION_TRANSPORT_BOX2
    } else {
        PROTECTION_TRANSPORT_BOX1
    };
    // The throwaway KEK exists only for this call.
    let kek = Kek::generate(rng);
    let indices: Vec<u8> = (1..=m).collect();
    let dealt = deal_and_prove(&kek, threshold, &indices, rng)?;
    drop(kek);
    let mut files = Vec::with_capacity(recipients.len());
    let mut written = Vec::with_capacity(recipients.len());
    let mut first_header = None;
    for (share, r) in dealt.shares.iter().zip(recipients) {
        let own = &roster[usize::from(share.index) - 1];
        let header = ShareHeader {
            suite: SUITE_V1,
            protection,
            role: ROLE_REHEARSAL,
            release_box: 0,
            split_id: dealt.split_id,
            container_sha256: [0; 32],
            device_public_key: [0; 32],
            issued_bucket_start: bucket,
            n: threshold,
            m,
            index: share.index,
            holder: own.id.clone(),
            recipient_key_hash: own.recipient_key_hash,
            commitments: dealt.commitments.clone(),
            roster: roster.clone(),
            container: Vec::new(),
        };
        if first_header.is_none() {
            first_header = Some(header.clone());
        }
        let file = ShareFile::seal_transport(header, share, r, rng)?;
        files.push((
            format!("{}.{}.rehearsal.share", own.id, hex8(&dealt.split_id)),
            file,
        ));
        written.push(RehearsalShare {
            index: share.index,
            holder: own.id.clone(),
            fingerprint: r.fingerprint()?,
        });
    }
    let record = SplitRecord::from_header(&first_header.expect("m >= 2 shares"));
    let split8 = hex8(&dealt.split_id);
    for (name, file) in &files {
        let path = dir.join(name);
        write_new_secret_file(&path.to_string_lossy(), file.to_text().as_bytes())?;
    }
    write_new_public_file(
        &dir.join(format!("{split8}.split.json")).to_string_lossy(),
        record.to_json_text().as_bytes(),
    )?;
    Ok(rehearse_lines(
        &split8,
        threshold,
        m,
        &written,
        dealt.subsets_checked,
    ))
}

/// `break_glass custody rehearse`.
pub(crate) fn cmd_rehearse(threshold: u8, recipients: &[String], output_dir: &str) -> Result<()> {
    let mut parsed = Vec::with_capacity(recipients.len());
    for path in recipients {
        let bytes = read_capped(path, RECIPIENT_FILE_MAX, "recipient file")?;
        parsed.push(RecipientPublic::from_json(&bytes)?);
    }
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|_| anyhow!("the system clock is before 1970"))?
        .as_secs();
    let bucket = now - now % 600;
    let lines = rehearse_core(
        threshold,
        &parsed,
        Path::new(output_dir),
        bucket,
        &mut OsCustodyRng,
    )?;
    print!("{lines}");
    Ok(())
}

// ---------------------------------------------------------------------------
// share inspect
// ---------------------------------------------------------------------------

fn protection_text(protection: u8) -> &'static str {
    match protection {
        PROTECTION_TRANSPORT_BOX1 => "encrypted to the holder's recipient key (HPKE X25519)",
        PROTECTION_TRANSPORT_BOX2 => {
            "encrypted to the holder's recipient key (HPKE X25519 with ML-KEM-768)"
        }
        _ => "wrapped at rest under the holder's passphrase (Argon2id)",
    }
}

fn role_text(role: u8) -> &'static str {
    match role {
        ROLE_OPERATIONAL => "operational",
        ROLE_RECOVERY => "recovery",
        _ => "REHEARSAL — protects nothing",
    }
}

/// What `share inspect` prints for a parsed file; `file_ok` is its file
/// check.
pub(crate) fn share_inspect_lines(file: &ShareFile, file_ok: bool) -> String {
    let h = &file.header;
    let rehearsal = h.role == ROLE_REHEARSAL;
    let roster = h
        .roster
        .iter()
        .map(|e| e.id.as_str())
        .collect::<Vec<_>>()
        .join(", ");
    let mut out = format!(
        "Share file:   SHR1 v1 — ristretto255 Pedersen shares; {}\n",
        protection_text(h.protection)
    );
    out.push_str(&format!("Role:         {}\n", role_text(h.role)));
    out.push_str(&format!(
        "Split:        {} — share {} of {}; any {} open it\n",
        hex8(&h.split_id),
        h.index,
        h.m,
        h.n
    ));
    out.push_str(&format!("Holder:       {}\n", h.holder));
    out.push_str(&format!(
        "Roster:       {roster} ({} holders, each pinned with its signing and recipient keys)\n",
        h.m
    ));
    out.push_str(&format!(
        "KEK:          commitment {}…\n",
        hex16(&h.kek_commitment())
    ));
    if rehearsal {
        out.push_str("Container:    — (rehearsal)\n");
        out.push_str("Device key:   — (rehearsal)\n");
    } else {
        out.push_str(&format!(
            "Container:    copy included, {} bytes, sha256 {}…\n",
            h.container.len(),
            hex16(&h.container_sha256)
        ));
        out.push_str(&format!("Device key:   {}…\n", hex16(&h.device_public_key)));
    }
    out.push_str(&format!(
        "Issued:       {} UTC (10-minute bucket)\n",
        utc_minute(h.issued_bucket_start)
    ));
    out.push_str(if file_ok {
        "File check:   intact\n"
    } else {
        "File check:   FAILED — the file is damaged; ask for your issuance copy\n"
    });
    out
}

fn read_share_file(path: &str) -> Result<Vec<u8>> {
    let text = read_capped(path, SHARE_TEXT_MAX_BYTES, "share file")?;
    Ok(ShareFile::text_to_binary(&text)?)
}

/// `break_glass share inspect`.
pub(crate) fn cmd_share_inspect(share: &str) -> Result<()> {
    let bin = read_share_file(share)?;
    let (file, ok) = ShareFile::parse(&bin)?;
    print!("{}", share_inspect_lines(&file, ok));
    Ok(())
}

// ---------------------------------------------------------------------------
// share verify / share protect
// ---------------------------------------------------------------------------

/// The key a holder opens their share with.
pub(crate) enum ShareKey<'a> {
    /// A transport share's recipient key.
    Recipient(&'a RecipientSecret),
    /// A protected share's passphrase.
    Passphrase(&'a str),
}

/// Map an open failure to the line a holder reads.
fn open_refusal(e: CustodyError) -> anyhow::Error {
    match e {
        CustodyError::WrongRecipient { .. } | CustodyError::Box2NeedsFeature => anyhow!("{e}"),
        CustodyError::WrongPassphrase => anyhow!("{WRONG_SHARE_PASSPHRASE}"),
        CustodyError::WrongProtection => anyhow!("{VERIFY_NEEDS_KEY}"),
        other => anyhow!("share check FAILED: {other}"),
    }
}

fn open_share(
    file: &ShareFile,
    key: &ShareKey<'_>,
) -> Result<crate::vault::custody::vss::SecretShare> {
    let share = match key {
        ShareKey::Recipient(k) => file.open_transport(k),
        ShareKey::Passphrase(p) => file.open_at_rest(p),
    }
    .map_err(open_refusal)?;
    if !check_share(&file.header.commitments, &share) {
        return Err(anyhow!("{SHARE_MISMATCH}"));
    }
    Ok(share)
}

/// Refuse the wrong kind of key before any passphrase is asked for.
fn key_kind_ok(file: &ShareFile, at_rest: bool) -> Result<()> {
    let protected = matches!(file.body, ShareBody::AtRest { .. });
    if protected != at_rest {
        return Err(anyhow!("{VERIFY_NEEDS_KEY}"));
    }
    Ok(())
}

/// Open and check a share; on success, what `share verify` prints.
pub(crate) fn share_verify_core(
    file: &ShareFile,
    key: ShareKey<'_>,
    record: Option<&SplitRecord>,
) -> Result<String> {
    open_share(file, &key)?;
    let h = &file.header;
    if let Some(record) = record {
        if !record.same_commitments(h) {
            return Err(anyhow!("{RECORD_COMMITMENTS_DIFFER}"));
        }
        if !record.matches(h) {
            return Err(anyhow!("{RECORD_SPLIT_DIFFERS}"));
        }
    }
    let mut out = format!(
        "Share {} of {} for '{}' (split {}; any {} of {} open it):\n",
        h.index,
        h.m,
        h.holder,
        hex8(&h.split_id),
        h.n,
        h.m
    );
    out.push_str(&ok_line(match key {
        ShareKey::Recipient(_) => "opens with your recipient key",
        ShareKey::Passphrase(_) => "opens with your passphrase",
    }));
    out.push_str(&ok_line(&format!(
        "matches the split's {} commitments",
        h.n
    )));
    if record.is_some() {
        out.push_str(&ok_line("matches the split record you were given"));
    }
    if h.role != ROLE_REHEARSAL {
        // The copy's hash was checked when the file was decoded; its header
        // must also name this share's KEK.
        let container =
            decode_mkc1_header(&h.container).map_err(|e| anyhow!("share check FAILED: {e}"))?;
        if container.kek_commitment != h.kek_commitment() {
            return Err(anyhow!("{CONTAINER_OTHER_KEK}"));
        }
        out.push_str(&ok_line("container copy matches its recorded hash"));
    }
    out.push_str("share checks passed. Keep this file apart from the hub; it is useless without your key and passphrase.\n");
    Ok(out)
}

fn read_recipient_key(path: &str, input: &mut PassphraseInput) -> Result<RecipientSecret> {
    let text = zeroize::Zeroizing::new(read_capped(
        path,
        RECIPIENT_KEY_FILE_MAX,
        "recipient key file",
    )?);
    // Parse before asking for a passphrase: a file that is not a recipient
    // key is refused without a prompt.
    crate::vault::custody::recipient::parse_tek1(
        &crate::vault::custody::recipient::tek1_from_text(&text)?,
    )?;
    let passphrase = input
        .read("recipient key passphrase: ")
        .map_err(|e| no_source(e, NO_PASSPHRASE))?;
    Ok(RecipientSecret::unwrap(&text, &passphrase)?.0)
}

/// `break_glass share verify`.
pub(crate) fn cmd_share_verify(
    share: &str,
    recipient_key: Option<&str>,
    at_rest: bool,
    split_record: Option<&str>,
    passphrase_fd: Option<i32>,
) -> Result<()> {
    let file = ShareFile::decode(&read_share_file(share)?)
        .map_err(|e| anyhow!("share check FAILED: {e}"))?;
    if recipient_key.is_none() && !at_rest {
        return Err(anyhow!("{VERIFY_NEEDS_KEY}"));
    }
    key_kind_ok(&file, at_rest)?;
    let record = match split_record {
        Some(path) => Some(SplitRecord::from_json(&read_capped(
            path,
            SPLIT_RECORD_MAX,
            "split record",
        )?)?),
        None => None,
    };
    let mut input = PassphraseInput::new(passphrase_fd).map_err(|e| anyhow!("{e}"))?;
    let lines = match recipient_key {
        Some(path) => {
            let key = read_recipient_key(path, &mut input)?;
            share_verify_core(&file, ShareKey::Recipient(&key), record.as_ref())?
        }
        None => {
            let passphrase = input
                .read("share passphrase: ")
                .map_err(|e| no_source(e, NO_PASSPHRASE))?;
            share_verify_core(&file, ShareKey::Passphrase(&passphrase), record.as_ref())?
        }
    };
    print!("{lines}");
    Ok(())
}

/// What `share protect` prints.
pub(crate) fn share_protect_lines(index: u8, split8: &str) -> String {
    format!(
        "share {index} of split {split8} is now wrapped under your passphrase (Argon2id) in the --output file, mode 0600\nit no longer depends on your recipient key at rest; now destroy the transport share file (--share) — this tool does not delete it\n"
    )
}

/// Rewrap a transport share under `new_passphrase`, then open the result
/// with it and check the share again before returning it.
pub(crate) fn share_protect_core(
    file: &ShareFile,
    key: &RecipientSecret,
    new_passphrase: &str,
    rng: &mut dyn CustodyRng,
) -> Result<(ShareFile, String)> {
    if file.header.protection == PROTECTION_AT_REST {
        return Err(anyhow!("{PROTECT_NEEDS_TRANSPORT}"));
    }
    let share = open_share(file, &ShareKey::Recipient(key))?;
    let protected = file.protect(&share, new_passphrase, rng)?;
    let again = ShareFile::from_text(protected.to_text().as_bytes())?;
    let reopened = open_share(&again, &ShareKey::Passphrase(new_passphrase))?;
    let same: bool = reopened.to_bytes()[..].ct_eq(&share.to_bytes()[..]).into();
    if !same || again != protected {
        return Err(anyhow!(
            "share protect: the protected file did not open to the same share; nothing was written"
        ));
    }
    let lines = share_protect_lines(file.header.index, &hex8(&file.header.split_id));
    Ok((protected, lines))
}

/// `break_glass share protect`.
pub(crate) fn cmd_share_protect(
    share: &str,
    recipient_key: &str,
    passphrase_fd: Option<i32>,
    output: &str,
) -> Result<()> {
    let file = ShareFile::decode(&read_share_file(share)?)
        .map_err(|e| anyhow!("share check FAILED: {e}"))?;
    if file.header.protection == PROTECTION_AT_REST {
        return Err(anyhow!("{PROTECT_NEEDS_TRANSPORT}"));
    }
    if exists(output) {
        return Err(anyhow!("{OUTPUT_EXISTS}"));
    }
    let mut input = PassphraseInput::new(passphrase_fd).map_err(|e| anyhow!("{e}"))?;
    let key = read_recipient_key(recipient_key, &mut input)?;
    let passphrase = input
        .read_new("new share passphrase: ", "repeat it: ")
        .map_err(|e| no_source(e, NO_PASSPHRASE))?;
    let (protected, lines) = share_protect_core(&file, &key, &passphrase, &mut OsCustodyRng)?;
    write_new_secret_file(output, protected.to_text().as_bytes())?;
    print!("{lines}");
    Ok(())
}

/// Dispatch `break_glass custody …`.
pub(crate) fn run_custody(command: CustodyCommand) -> Result<()> {
    match command {
        CustodyCommand::Recipient {
            command:
                RecipientCommand::Keygen {
                    holder,
                    role,
                    signing_key,
                    pq,
                    passphrase_fd,
                    output_key,
                    output_public,
                },
        } => cmd_recipient_keygen(
            &holder,
            &role,
            &signing_key,
            pq,
            passphrase_fd,
            &output_key,
            &output_public,
        ),
        CustodyCommand::Rehearse {
            threshold,
            recipients,
            output_dir,
        } => cmd_rehearse(threshold, &recipients, &output_dir),
    }
}

/// Dispatch `break_glass share …`.
pub(crate) fn run_share(command: ShareCommand) -> Result<()> {
    match command {
        ShareCommand::Inspect { share } => cmd_share_inspect(&share),
        ShareCommand::Verify {
            share,
            recipient_key,
            at_rest,
            split_record,
            passphrase_fd,
        } => cmd_share_verify(
            &share,
            recipient_key.as_deref(),
            at_rest,
            split_record.as_deref(),
            passphrase_fd,
        ),
        ShareCommand::Protect {
            share,
            recipient_key,
            passphrase_fd,
            output,
        } => cmd_share_protect(&share, &recipient_key, passphrase_fd, &output),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::vault::custody::kat::{
        fixture_share, fx, h, recipient_public, recipient_secret, s, KatRng,
    };
    use clap::{CommandFactory, Parser};
    use ed25519_dalek::SigningKey;

    /// A signer over a fixture seed (the TrusteeSigner seam).
    struct TestSigner(SigningKey);

    impl TrusteeSigner for TestSigner {
        fn public_key(&self) -> [u8; 32] {
            self.0.verifying_key().to_bytes()
        }
        fn sign(&self, domain: &str, message_hash: &[u8; 32]) -> Result<Vec<u8>> {
            Ok(
                crate::crypto::signatures::sign_ed25519_only(domain, &self.0, message_hash)
                    .to_vec(),
            )
        }
    }

    fn alice_signer() -> TestSigner {
        let seed: [u8; 32] = h(&fx()["recipients"]["alice"]["signing_seed"])
            .try_into()
            .unwrap();
        TestSigner(SigningKey::from_bytes(&seed))
    }

    fn alice_file() -> ShareFile {
        ShareFile::from_text(s(&fx()["share_files"]["alice"]["text"]).as_bytes()).unwrap()
    }

    fn record(name: &str) -> SplitRecord {
        SplitRecord::from_json(s(&fx()["split_records"][name]).as_bytes()).unwrap()
    }

    /// The custody and share subcommands, parsed on their own.
    #[derive(Parser, Debug)]
    struct Custody {
        #[command(subcommand)]
        command: CustodyCommand,
    }

    #[derive(Parser, Debug)]
    struct Share {
        #[command(subcommand)]
        command: ShareCommand,
    }

    const ALICE_INSPECT: &str = "\
Share file:   SHR1 v1 — ristretto255 Pedersen shares; encrypted to the holder's recipient key (HPKE X25519)
Role:         operational
Split:        e668426d — share 1 of 5; any 3 open it
Holder:       alice
Roster:       alice, bob, carol, dave, erin (5 holders, each pinned with its signing and recipient keys)
KEK:          commitment 00e1eda69e898552…
Container:    copy included, 155 bytes, sha256 339b850214b1c267…
Device key:   15b2a8b807f8577e…
Issued:       2026-11-10 12:00 UTC (10-minute bucket)
File check:   intact
";

    #[test]
    fn recipient_keygen_lines_pinned() {
        assert_eq!(
            keygen_lines("alice", HOLDER_ROLE_TRUSTEE, false, "3f2a9c1d 5b7e0a44", &[0x5f; 32]),
            "Recipient key for 'alice' (trustee): X25519\n  \
             fingerprint 3f2a9c1d 5b7e0a44 — read it aloud at the issuance ceremony\n  \
             bound to signing key 5f5f5f5f5f5f5f5f… by a signature under securacv:pwk:custody-recipient-binding:v1\n  \
             the secret part is in the --output-key file, wrapped under your passphrase (Argon2id, mode 0600); give the --output-public file to the operator, never the key file\n"
        );
        assert!(keygen_lines(
            "r1",
            HOLDER_ROLE_RECOVERY,
            true,
            "00000000 00000000",
            &[0; 32]
        )
        .starts_with("Recipient key for 'r1' (recovery): X25519 + ML-KEM-768\n"));
        // The real thing: alice's fixture signing key binds a fresh key.
        let out = keygen_core(
            "alice",
            HOLDER_ROLE_TRUSTEE,
            false,
            &alice_signer(),
            "kat passphrase",
            &mut OsCustodyRng,
        )
        .unwrap();
        let public = RecipientPublic::from_json(out.public_text.as_bytes()).unwrap();
        public.check_binding_ed25519().unwrap();
        assert_eq!(
            out.lines,
            keygen_lines(
                "alice",
                HOLDER_ROLE_TRUSTEE,
                false,
                &public.fingerprint().unwrap(),
                &alice_signer().public_key()
            )
        );
        assert!(out.lines.contains("bound to signing key 8c2bdf78dafa9c61…"));
        assert!(out
            .key_text
            .starts_with("securacv-custody-recipient-key:v1 "));
        let (opened, header) =
            RecipientSecret::unwrap(out.key_text.as_bytes(), "kat passphrase").unwrap();
        assert_eq!(header.recipient_key_hash, public.key_hash().unwrap());
        assert_eq!(opened.x25519_public(), public.x25519_public().unwrap());
    }

    #[cfg(feature = "pqc-vault")]
    #[test]
    fn keygen_pq_binds_an_mlkem_key() {
        let out = keygen_core(
            "alice",
            HOLDER_ROLE_TRUSTEE,
            true,
            &alice_signer(),
            "kat passphrase",
            &mut OsCustodyRng,
        )
        .unwrap();
        let public = RecipientPublic::from_json(out.public_text.as_bytes()).unwrap();
        assert!(public.mlkem768_public().unwrap().is_some());
        public.check_binding_ed25519().unwrap();
        assert!(out
            .lines
            .starts_with("Recipient key for 'alice' (trustee): X25519 + ML-KEM-768\n"));
    }

    #[cfg(not(feature = "pqc-vault"))]
    #[test]
    fn keygen_refuses_pq_without_feature() {
        let err = keygen_core(
            "alice",
            HOLDER_ROLE_TRUSTEE,
            true,
            &alice_signer(),
            "kat passphrase",
            &mut OsCustodyRng,
        )
        .err()
        .unwrap();
        assert_eq!(
            err.to_string(),
            "--pq needs a build with the pqc-vault feature"
        );
        // The command refuses before it reads any file or passphrase: the
        // signing key named here does not exist.
        let err = cmd_recipient_keygen(
            "alice",
            "trustee",
            "/nonexistent/securacv-test/alice.tkey",
            true,
            None,
            "/nonexistent/securacv-test/a",
            "/nonexistent/securacv-test/b",
        )
        .unwrap_err();
        assert_eq!(err.to_string(), PQ_NEEDS_FEATURE);
    }

    #[test]
    fn keygen_never_reads_passphrase_from_argv_or_env() {
        // No argument of any custody or share command takes a passphrase,
        // and none reads the environment.
        for root in [Custody::command(), Share::command()] {
            let mut stack = vec![root];
            let mut seen = 0;
            while let Some(cmd) = stack.pop() {
                for arg in cmd.get_arguments() {
                    let id = arg.get_id().as_str();
                    assert!(
                        id == "passphrase_fd" || !id.contains("passphrase"),
                        "{} has --{id}",
                        cmd.get_name()
                    );
                    assert!(arg.get_env().is_none(), "{id} reads the environment");
                    seen += 1;
                }
                stack.extend(cmd.get_subcommands().cloned());
            }
            assert!(seen > 5);
        }
        let err = Custody::try_parse_from([
            "t",
            "recipient",
            "keygen",
            "--holder",
            "alice",
            "--role",
            "trustee",
            "--signing-key",
            "k",
            "--output-key",
            "x",
            "--output-public",
            "y",
            "--passphrase",
            "hunter2",
        ])
        .unwrap_err();
        assert_eq!(err.kind(), clap::error::ErrorKind::UnknownArgument);
        // With no fd and no terminal, a new passphrase is refused in the
        // command's own words.
        let refused = PassphraseInput::without_terminal()
            .read_new("new passphrase for recipient key 'alice': ", "repeat it: ")
            .map_err(|e| no_source(e, NO_NEW_RECIPIENT_PASSPHRASE))
            .unwrap_err();
        assert_eq!(
            refused.to_string(),
            "a new recipient key needs a passphrase: run on a terminal or pass --passphrase-fd"
        );
    }

    #[test]
    fn rehearse_writes_m_shares_and_a_record() {
        let ids = ["alice", "bob", "carol", "dave", "erin"];
        let recipients: Vec<RecipientPublic> = ids.iter().map(|id| recipient_public(id)).collect();
        let temp = tempfile::tempdir().unwrap();
        let dir = temp.path().join("rehearsal");
        let lines = rehearse_core(
            3,
            &recipients,
            &dir,
            1_794_312_000,
            &mut KatRng::new("rehearse-test"),
        )
        .unwrap();
        assert_eq!(lines, REHEARSE_PIN);
        let split8 = &lines["REHEARSAL split ".len().."REHEARSAL split ".len() + 8];
        let mut names: Vec<String> = std::fs::read_dir(&dir)
            .unwrap()
            .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        names.sort();
        let mut want: Vec<String> = ids
            .iter()
            .map(|id| format!("{id}.{split8}.rehearsal.share"))
            .collect();
        want.push(format!("{split8}.split.json"));
        want.sort();
        assert_eq!(names, want);
        let record_text = std::fs::read(dir.join(format!("{split8}.split.json"))).unwrap();
        let record = SplitRecord::from_json(&record_text).unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = |p: &Path| std::fs::metadata(p).unwrap().permissions().mode() & 0o777;
            assert_eq!(mode(&dir), 0o700);
            assert_eq!(mode(&dir.join(format!("{split8}.split.json"))), 0o644);
            assert_eq!(
                mode(&dir.join(format!("alice.{split8}.rehearsal.share"))),
                0o600
            );
        }
        for (i, id) in ids.iter().enumerate() {
            let text = std::fs::read(dir.join(format!("{id}.{split8}.rehearsal.share"))).unwrap();
            let file = ShareFile::from_text(&text).unwrap();
            assert_eq!(file.header.role, ROLE_REHEARSAL);
            assert_eq!(file.header.index as usize, i + 1);
            assert!(record.matches(&file.header));
            let lines = share_verify_core(
                &file,
                ShareKey::Recipient(&recipient_secret(id)),
                Some(&record),
            )
            .unwrap();
            assert!(lines.ends_with("share checks passed. Keep this file apart from the hub; it is useless without your key and passphrase.\n"));
            assert!(!lines.contains("container copy"));
        }
        // A second run into the same directory is refused.
        assert_eq!(
            rehearse_core(3, &recipients, &dir, 1_794_312_000, &mut OsCustodyRng)
                .unwrap_err()
                .to_string(),
            REHEARSE_DIR_NOT_EMPTY
        );
        let fresh = temp.path().join("other");
        for t in [1u8, 6] {
            assert_eq!(
                rehearse_core(t, &recipients, &fresh, 1_794_312_000, &mut OsCustodyRng)
                    .unwrap_err()
                    .to_string(),
                "custody rehearse: threshold must be between 2 and the number of recipients (5)"
            );
        }
        let twice = [recipient_public("alice"), recipient_public("alice")];
        assert_eq!(
            rehearse_core(2, &twice, &fresh, 1_794_312_000, &mut OsCustodyRng)
                .unwrap_err()
                .to_string(),
            REHEARSE_DUPLICATE
        );
        // A recipient file whose binding signature is another holder's.
        let mut json: serde_json::Value =
            serde_json::from_str(s(&fx()["recipients"]["bob"]["recipient_json"])).unwrap();
        json["binding_signature"] = fx()["recipients"]["alice"]["binding_signature"].clone();
        let forged = RecipientPublic::from_json(json.to_string().as_bytes()).unwrap();
        assert_eq!(
            rehearse_core(2, &[recipient_public("alice"), forged], &fresh, 1_794_312_000, &mut OsCustodyRng)
                .unwrap_err()
                .to_string(),
            "recipient file for 'bob' carries a binding signature that does not check against its own signing key"
        );
        assert!(!fresh.exists(), "a refused rehearsal writes nothing");
    }

    /// Deterministic: the fixture's recipients and a KatRng stream.
    const REHEARSE_PIN: &str = "\
REHEARSAL split b3030878: any 3 of 5 shares open a throwaway key that protects nothing
  share 1 → 'alice' (fingerprint 07db1842 18b01402)
  share 2 → 'bob' (fingerprint 588b7464 d3eac5ca)
  share 3 → 'carol' (fingerprint 95e1fafd 8683a6e7)
  share 4 → 'dave' (fingerprint 328c9582 24c7778e)
  share 5 → 'erin' (fingerprint c8593522 0bbe0a69)
every share passed its commitment check, and 10 subsets of 3 rebuilt the throwaway key before anything was written
give each holder their share and b3030878.split.json; each runs: break_glass share verify --share <file> --recipient-key <file> --split-record b3030878.split.json
";

    #[test]
    fn share_inspect_lines_pinned() {
        let file = alice_file();
        assert_eq!(share_inspect_lines(&file, true), ALICE_INSPECT);
        // The other forms' differing lines.
        let rec = ShareFile::from_text(s(&fx()["share_files"]["r2"]["text"]).as_bytes()).unwrap();
        let lines = share_inspect_lines(&rec, true);
        assert!(lines.contains("\nRole:         recovery\n"));
        assert!(lines.contains("\nSplit:        6874059e — share 2 of 6; any 4 open it\n"));
        assert!(lines.contains("\nRoster:       r1, r2, r3, r4, r5, r6 (6 holders, each pinned with its signing and recipient keys)\n"));
        let box2 = ShareFile::decode(&h(&fx()["share_file_box2"]["file_hex"])).unwrap();
        assert!(share_inspect_lines(&box2, true).starts_with(
            "Share file:   SHR1 v1 — ristretto255 Pedersen shares; encrypted to the holder's recipient key (HPKE X25519 with ML-KEM-768)\n"
        ));
        let at_rest = ShareFile::decode(&h(&fx()["share_file_at_rest"]["file_hex"])).unwrap();
        assert!(share_inspect_lines(&at_rest, true).starts_with(
            "Share file:   SHR1 v1 — ristretto255 Pedersen shares; wrapped at rest under the holder's passphrase (Argon2id)\n"
        ));
    }

    #[test]
    fn share_inspect_damaged_line() {
        let mut bin = h(&fx()["share_files"]["alice"]["file_hex"]);
        let at = bin.len() - 40;
        bin[at] ^= 1;
        let (file, ok) = ShareFile::parse(&bin).unwrap();
        assert!(!ok);
        let damaged = ALICE_INSPECT.replace(
            "File check:   intact\n",
            "File check:   FAILED — the file is damaged; ask for your issuance copy\n",
        );
        assert_eq!(share_inspect_lines(&file, ok), damaged);
        // A file the parser refuses outright gets its closed reason.
        bin[0] = b'X';
        assert_eq!(
            ShareFile::parse(&bin).unwrap_err().to_string(),
            "share file: bad magic"
        );
    }

    #[test]
    fn share_verify_success_lines_pinned() {
        let file = alice_file();
        let key = recipient_secret("alice");
        assert_eq!(
            share_verify_core(&file, ShareKey::Recipient(&key), Some(&record("operational")))
                .unwrap(),
            "\
Share 1 of 5 for 'alice' (split e668426d; any 3 of 5 open it):
  opens with your recipient key ............................ OK
  matches the split's 3 commitments ........................ OK
  matches the split record you were given .................. OK
  container copy matches its recorded hash ................. OK
share checks passed. Keep this file apart from the hub; it is useless without your key and passphrase.
"
        );
        assert_eq!(
            share_verify_core(&file, ShareKey::Recipient(&key), None).unwrap(),
            "\
Share 1 of 5 for 'alice' (split e668426d; any 3 of 5 open it):
  opens with your recipient key ............................ OK
  matches the split's 3 commitments ........................ OK
  container copy matches its recorded hash ................. OK
share checks passed. Keep this file apart from the hub; it is useless without your key and passphrase.
"
        );
        // The wrong kind of key is refused by name.
        assert_eq!(
            share_verify_core(&file, ShareKey::Passphrase("x"), None)
                .unwrap_err()
                .to_string(),
            VERIFY_NEEDS_KEY
        );
        assert_eq!(
            key_kind_ok(&file, true).unwrap_err().to_string(),
            VERIFY_NEEDS_KEY
        );
        key_kind_ok(&file, false).unwrap();
    }

    #[test]
    fn share_verify_wrong_recipient_line() {
        let file = alice_file();
        let bob = recipient_secret("bob");
        let spk: [u8; 32] = h(&fx()["recipients"]["alice"]["signing_public_key"])
            .try_into()
            .unwrap();
        let yours = hex8(&bob.key_hash(&spk));
        assert_eq!(
            share_verify_core(&file, ShareKey::Recipient(&bob), None)
                .unwrap_err()
                .to_string(),
            format!(
                "cannot open this share with the supplied recipient key: it was encrypted to recipient 07db1842…, yours is {yours}…"
            )
        );
        // A forged share inside a well-formed file: it opens, and fails its
        // Pedersen check.
        let mut header = file.header.clone();
        header.protection = PROTECTION_TRANSPORT_BOX1;
        let forged = fixture_share("alice").shifted(
            curve25519_dalek::scalar::Scalar::ONE,
            curve25519_dalek::scalar::Scalar::ZERO,
        );
        let bad = ShareFile::seal_transport_box1_derand(
            header,
            &forged,
            &recipient_public("alice"),
            &[9; 32],
        )
        .unwrap();
        assert_eq!(
            share_verify_core(&bad, ShareKey::Recipient(&recipient_secret("alice")), None)
                .unwrap_err()
                .to_string(),
            SHARE_MISMATCH
        );
    }

    #[test]
    fn share_verify_split_record_mismatch_line() {
        let file = alice_file();
        let key = recipient_secret("alice");
        assert_eq!(
            share_verify_core(&file, ShareKey::Recipient(&key), Some(&record("recovery")))
                .unwrap_err()
                .to_string(),
            "share check FAILED: the share's commitments differ from the split record you were given — do not acknowledge it; the dealer showed holders different commitments"
        );
        // Same commitments, another device key: a different split record.
        let text = s(&fx()["split_records"]["operational"])
            .replace(s(&fx()["device"]["public_key"]), &"ab".repeat(32));
        let other_device = SplitRecord::from_json(text.as_bytes()).unwrap();
        assert_eq!(
            share_verify_core(&file, ShareKey::Recipient(&key), Some(&other_device))
                .unwrap_err()
                .to_string(),
            RECORD_SPLIT_DIFFERS
        );
    }

    #[test]
    fn share_protect_lines_and_reopen() {
        let file = alice_file();
        let key = recipient_secret("alice");
        let (protected, lines) =
            share_protect_core(&file, &key, "new share passphrase", &mut OsCustodyRng).unwrap();
        assert_eq!(
            lines,
            "share 1 of split e668426d is now wrapped under your passphrase (Argon2id) in the --output file, mode 0600\n\
             it no longer depends on your recipient key at rest; now destroy the transport share file (--share) — this tool does not delete it\n"
        );
        let reread = ShareFile::from_text(protected.to_text().as_bytes()).unwrap();
        assert_eq!(reread.header.protection, PROTECTION_AT_REST);
        assert_eq!(
            share_verify_core(&reread, ShareKey::Passphrase("new share passphrase"), None)
                .unwrap(),
            "\
Share 1 of 5 for 'alice' (split e668426d; any 3 of 5 open it):
  opens with your passphrase ............................... OK
  matches the split's 3 commitments ........................ OK
  container copy matches its recorded hash ................. OK
share checks passed. Keep this file apart from the hub; it is useless without your key and passphrase.
"
        );
        assert_eq!(
            share_verify_core(&reread, ShareKey::Passphrase("wrong"), None)
                .unwrap_err()
                .to_string(),
            "cannot open this share: wrong passphrase"
        );
        assert_eq!(
            share_protect_core(&reread, &key, "x", &mut OsCustodyRng)
                .unwrap_err()
                .to_string(),
            PROTECT_NEEDS_TRANSPORT
        );
        assert_eq!(
            share_protect_core(&file, &recipient_secret("bob"), "x", &mut OsCustodyRng)
                .unwrap_err()
                .to_string()
                .split(':')
                .next(),
            Some("cannot open this share with the supplied recipient key")
        );
    }

    #[test]
    fn no_output_line_says_verified() {
        let mut texts: Vec<String> = vec![
            keygen_lines(
                "alice",
                HOLDER_ROLE_TRUSTEE,
                true,
                "00000000 00000000",
                &[0; 32],
            ),
            rehearse_lines(
                "00000000",
                2,
                3,
                &[RehearsalShare {
                    index: 1,
                    holder: "alice".into(),
                    fingerprint: "00000000 00000000".into(),
                }],
                3,
            ),
            ALICE_INSPECT.to_string(),
            share_inspect_lines(&alice_file(), false),
            share_verify_core(
                &alice_file(),
                ShareKey::Recipient(&recipient_secret("alice")),
                Some(&record("operational")),
            )
            .unwrap(),
            share_protect_lines(1, "00000000"),
            REHEARSE_PIN.to_string(),
        ];
        for c in [
            PQ_NEEDS_FEATURE,
            NO_NEW_RECIPIENT_PASSPHRASE,
            NO_PASSPHRASE,
            VERIFY_NEEDS_KEY,
            SHARE_MISMATCH,
            RECORD_COMMITMENTS_DIFFER,
            RECORD_SPLIT_DIFFERS,
            CONTAINER_OTHER_KEK,
            WRONG_SHARE_PASSPHRASE,
            PROTECT_NEEDS_TRANSPORT,
            REHEARSE_DIR_NOT_EMPTY,
            REHEARSE_DUPLICATE,
        ] {
            texts.push(c.to_string());
        }
        for case in fx()["negative"].as_array().unwrap() {
            let want = case
                .get("error")
                .or_else(|| case.get("open_error"))
                .unwrap();
            texts.push(s(want).to_string());
        }
        texts.push(Custody::command().render_long_help().to_string());
        texts.push(Share::command().render_long_help().to_string());
        for sub in ["recipient", "rehearse"] {
            let mut cmd = Custody::command();
            texts.push(
                cmd.find_subcommand_mut(sub)
                    .unwrap()
                    .render_long_help()
                    .to_string(),
            );
        }
        for sub in ["inspect", "verify", "protect"] {
            let mut cmd = Share::command();
            texts.push(
                cmd.find_subcommand_mut(sub)
                    .unwrap()
                    .render_long_help()
                    .to_string(),
            );
        }
        assert!(texts.len() > 50);
        for t in &texts {
            assert!(!t.to_lowercase().contains("verified"), "{t}");
        }
    }
}
