//! OpenTimestamps: the trust-kind-independent anchoring leg beside the two
//! RFC 3161 TSAs (`docs/design/opentimestamps_leg.md`).
//!
//! A hand-rolled codec for the `.ots` DetachedTimestampFile format, pinned
//! byte for byte to the `opentimestamps` crate's two real test proofs, plus
//! the frozen `ots_anchors` table, the Bitcoin header check `log_anchor
//! verify --bitcoin-headers` runs, and the calendar leg of `anchor-all`.
//!
//! Always compiled: `run_full_verify` (inside `witnessd`, at boot) checks OTS
//! rows structurally and must give the same verdict in every build. Only the
//! HTTP transport (`UreqCalendar`) is behind the non-default `ots` feature;
//! `witnessd` never contacts a calendar.
//!
//! Wire format (the Python reference `opentimestamps` 0.5.0 is the protocol
//! reference, the `ots` client the second implementation):
//!
//! * file = MAGIC ‖ version 1 ‖ 0x08 (SHA-256) ‖ 32-byte digest ‖ timestamp ‖ EOF;
//! * timestamp = items, each but the last prefixed 0xff; an item is
//!   `0x00 ‖ attestation` (a leaf) or `op ‖ sub-timestamp`;
//! * attestation = 8-byte tag ‖ varbytes payload, consumed exactly.
//!
//! Every bound the reference enforces is enforced here, plus two it does
//! not: a varuint is at most 9 bytes (the crate's `read_uint` is unbounded)
//! and an attestation payload must be consumed exactly. SHA-1, RIPEMD-160
//! and Keccak operations are refused at parse with a message that names
//! them — no calendar emits them and executing them would need two more
//! hash crates. Serialization is canonical (Python's `Timestamp.serialize`
//! order), by explicit comparators, never a derived `Ord`.
//!
//! What this module never does: store or print a block time, contact the
//! network outside `UreqCalendar`, or say "verified". A Bitcoin attestation
//! is a block height until its merkle root is compared with a header the
//! verifier supplies.

use anyhow::{anyhow, bail, Result};
use rusqlite::{Connection, OptionalExtension};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::cmp::Ordering;

use crate::anchor_policy::OtsPolicy;
use crate::tsa::{self, AnchorSubject};
use crate::TimeBucket;

/// The table OpenTimestamps proofs live in (never `tsa_anchors`).
pub const OTS_ANCHORS_TABLE: &str = "ots_anchors";
/// `\x00OpenTimestamps\x00\x00Proof\x00\xbf\x89\xe2\xe8\x84\xe8\x92\x94`.
pub const OTS_MAGIC: [u8; 31] =
    *b"\x00OpenTimestamps\x00\x00Proof\x00\xbf\x89\xe2\xe8\x84\xe8\x92\x94";
/// The only DetachedTimestampFile major version.
pub const OTS_VERSION: u64 = 1;
/// The file-hash op tag for SHA-256.
pub const OTS_DIGEST_TAG_SHA256: u8 = 0x08;
/// Bytes of client-side blinding appended before the commitment's SHA-256.
pub const OTS_NONCE_LEN: usize = 16;
/// A whole stored proof.
pub const OTS_MAX_FILE_BYTES: usize = 64 * 1024;
/// A calendar's response body (the reference client's limit).
pub const OTS_MAX_RESPONSE_BYTES: usize = 10_000;
/// A varuint's encoded length (63 bits).
pub const OTS_MAX_VARUINT_BYTES: usize = 9;
/// An append/prepend argument (1..=4096).
pub const OTS_MAX_OP_ARG: usize = 4096;
/// An op's input message and its result.
pub const OTS_MAX_MSG: usize = 4096;
/// A pending attestation's URI.
pub const OTS_MAX_URI_LEN: usize = 1000;
/// An attestation payload.
pub const OTS_MAX_ATTESTATION_PAYLOAD: usize = 8192;
/// The largest block height an attestation may carry.
pub const OTS_MAX_HEIGHT: u64 = u32::MAX as u64;
/// How deep a timestamp tree may nest (the reference's limit).
pub const OTS_RECURSION_LIMIT: usize = 256;
/// The `Accept` header both calendar calls send.
pub const OTS_ACCEPT: &str = "application/vnd.opentimestamps.v1";
/// Pending rows older than this are reported, never polled (Invariant III).
pub const OTS_UPGRADE_HORIZON_S: u64 = 7 * 86_400;
/// `anchor-all --offline-dir` request file format literal.
pub const OTS_REQUEST_FORMAT: &str = "securacv-ots-request:v1";
/// `anchor-all --offline-dir` upgrade file format literal.
pub const OTS_UPGRADE_FORMAT: &str = "securacv-ots-upgrade:v1";
/// `ots_anchors.attestation` while no Bitcoin path is stored.
pub const OTS_ATTESTATION_PENDING: &str = "pending";
/// `ots_anchors.attestation` once the proof attests a Bitcoin block.
pub const OTS_ATTESTATION_BITCOIN: &str = "bitcoin";
/// BitcoinBlockHeaderAttestation tag.
pub const TAG_BITCOIN: [u8; 8] = [0x05, 0x88, 0x96, 0x0d, 0x73, 0xd7, 0x19, 0x01];
/// PendingAttestation tag.
pub const TAG_PENDING: [u8; 8] = [0x83, 0xdf, 0xe3, 0x0d, 0x2e, 0xf9, 0x0c, 0x8e];
/// Bitcoin mainnet's minimum-difficulty compact target (`powLimit`).
pub const BITCOIN_POW_LIMIT_BITS: u32 = 0x1d00_ffff;

// -------------------- ops and attestations --------------------

/// A commitment operation the verifier executes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Op {
    Sha256,
    Append(Vec<u8>),
    Prepend(Vec<u8>),
    Reverse,
    Hexlify,
}

impl Op {
    pub fn tag(&self) -> u8 {
        match self {
            Op::Sha256 => 0x08,
            Op::Append(_) => 0xf0,
            Op::Prepend(_) => 0xf1,
            Op::Reverse => 0xf2,
            Op::Hexlify => 0xf3,
        }
    }

    fn arg(&self) -> &[u8] {
        match self {
            Op::Append(a) | Op::Prepend(a) => a,
            _ => &[],
        }
    }

    fn name(&self) -> &'static str {
        match self {
            Op::Sha256 => "sha256",
            Op::Append(_) => "append",
            Op::Prepend(_) => "prepend",
            Op::Reverse => "reverse",
            Op::Hexlify => "hexlify",
        }
    }

    /// Apply the op, with the reference's message and result bounds.
    pub fn apply(&self, msg: &[u8]) -> Result<Vec<u8>> {
        let max_msg = match self {
            Op::Hexlify => OTS_MAX_MSG / 2,
            _ => OTS_MAX_MSG,
        };
        if msg.len() > max_msg {
            bail!(
                "{} over a {}-byte message (limit {max_msg})",
                self.name(),
                msg.len()
            );
        }
        let out = match self {
            Op::Sha256 => Sha256::digest(msg).to_vec(),
            Op::Append(a) => [msg, a].concat(),
            Op::Prepend(a) => [a.as_slice(), msg].concat(),
            Op::Reverse | Op::Hexlify if msg.is_empty() => {
                bail!("{} of an empty message", self.name())
            }
            Op::Reverse => msg.iter().rev().copied().collect(),
            Op::Hexlify => hex::encode(msg).into_bytes(),
        };
        if out.len() > OTS_MAX_MSG {
            bail!(
                "{} result is {} bytes (limit {OTS_MAX_MSG})",
                self.name(),
                out.len()
            );
        }
        Ok(out)
    }
}

impl std::fmt::Display for Op {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Op::Append(a) | Op::Prepend(a) => write!(f, "{} {}", self.name(), hex::encode(a)),
            _ => f.write_str(self.name()),
        }
    }
}

/// Python's op order (`op.py` `__lt__`): by tag byte, then by argument bytes.
pub fn op_cmp(a: &Op, b: &Op) -> Ordering {
    a.tag().cmp(&b.tag()).then_with(|| a.arg().cmp(b.arg()))
}

/// A leaf of the timestamp tree.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Attestation {
    /// The message is the merkle root of Bitcoin block `height`.
    Bitcoin { height: u64 },
    /// The calendar at `uri` received the message and promises a path later.
    Pending { uri: String },
    /// Any other tag (Litecoin, Ethereum, a future kind), kept verbatim and
    /// never read as a Bitcoin claim.
    Unknown { tag: [u8; 8], payload: Vec<u8> },
}

impl Attestation {
    pub fn tag(&self) -> [u8; 8] {
        match self {
            Attestation::Bitcoin { .. } => TAG_BITCOIN,
            Attestation::Pending { .. } => TAG_PENDING,
            Attestation::Unknown { tag, .. } => *tag,
        }
    }

    fn payload(&self) -> Vec<u8> {
        match self {
            Attestation::Bitcoin { height } => {
                let mut p = Vec::new();
                write_varuint(&mut p, *height);
                p
            }
            Attestation::Pending { uri } => {
                let mut p = Vec::new();
                write_varbytes(&mut p, uri.as_bytes());
                p
            }
            Attestation::Unknown { payload, .. } => payload.clone(),
        }
    }
}

/// Python's attestation order (`notary.py`): different kinds by their 8-byte
/// tag (so Bitcoin `05…` precedes Pending `83…`); Bitcoin by height, Pending
/// by URI bytes, Unknown by `(tag, payload)`.
pub fn attestation_cmp(a: &Attestation, b: &Attestation) -> Ordering {
    match (a, b) {
        (Attestation::Bitcoin { height: x }, Attestation::Bitcoin { height: y }) => x.cmp(y),
        (Attestation::Pending { uri: x }, Attestation::Pending { uri: y }) => {
            x.as_bytes().cmp(y.as_bytes())
        }
        (
            Attestation::Unknown {
                tag: t1,
                payload: p1,
            },
            Attestation::Unknown {
                tag: t2,
                payload: p2,
            },
        ) => t1.cmp(t2).then_with(|| p1.cmp(p2)),
        _ => a.tag().cmp(&b.tag()),
    }
}

// -------------------- the tree --------------------

/// A timestamp node at message `msg`: its leaves and its outgoing ops, both
/// kept in canonical order.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Node {
    pub msg: Vec<u8>,
    pub attestations: Vec<Attestation>,
    pub ops: Vec<(Op, Node)>,
}

impl Node {
    fn new(msg: Vec<u8>) -> Self {
        Node {
            msg,
            attestations: Vec::new(),
            ops: Vec::new(),
        }
    }

    fn sort(&mut self) {
        self.attestations.sort_by(attestation_cmp);
        self.ops.sort_by(|a, b| op_cmp(&a.0, &b.0));
    }
}

/// A DetachedTimestampFile over a SHA-256 digest.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Proof {
    pub digest: [u8; 32],
    pub root: Node,
}

/// A leaf and the message it attests.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Leaf {
    pub attestation: Attestation,
    pub msg: Vec<u8>,
}

/// What a proof claims, derived from its leaves.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProofState {
    /// Only pending leaves (and possibly unknown ones).
    Pending,
    /// At least one Bitcoin leaf; `height` is the lowest.
    Bitcoin { height: u64 },
    /// Neither a pending nor a Bitcoin leaf.
    NoKnownAttestation,
}

impl ProofState {
    /// The `ots_anchors.attestation` literal; `None` is never written.
    pub fn column(&self) -> Option<&'static str> {
        match self {
            ProofState::Pending => Some(OTS_ATTESTATION_PENDING),
            ProofState::Bitcoin { .. } => Some(OTS_ATTESTATION_BITCOIN),
            ProofState::NoKnownAttestation => None,
        }
    }

    /// The cached lowest height, or `None`.
    pub fn height(&self) -> Option<u64> {
        match self {
            ProofState::Bitcoin { height } => Some(*height),
            _ => None,
        }
    }
}

// -------------------- reading --------------------

struct Reader<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    fn new(data: &'a [u8]) -> Self {
        Reader { data, pos: 0 }
    }

    fn byte(&mut self) -> Result<u8> {
        let b = *self
            .data
            .get(self.pos)
            .ok_or_else(|| anyhow!("truncated: unexpected end of data"))?;
        self.pos += 1;
        Ok(b)
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        if self.data.len() - self.pos < n {
            bail!("truncated: unexpected end of data");
        }
        let out = &self.data[self.pos..self.pos + n];
        self.pos += n;
        Ok(out)
    }

    /// LEB128, at most 9 bytes (63 bits): the reference's bound. The
    /// `opentimestamps` crate's `read_uint` has no byte bound.
    fn varuint(&mut self) -> Result<u64> {
        let mut value: u64 = 0;
        for i in 0..OTS_MAX_VARUINT_BYTES {
            let b = self.byte()?;
            value |= u64::from(b & 0x7f) << (7 * i);
            if b & 0x80 == 0 {
                return Ok(value);
            }
        }
        bail!("varuint exceeds {OTS_MAX_VARUINT_BYTES} bytes")
    }

    fn varbytes(&mut self, max: usize, min: usize, what: &str) -> Result<&'a [u8]> {
        let len = self.varuint()?;
        if len > max as u64 {
            bail!("{what} is {len} bytes (limit {max})");
        }
        if len < min as u64 {
            bail!("{what} is {len} bytes (minimum {min})");
        }
        self.take(len as usize)
    }

    fn at_eof(&self) -> bool {
        self.pos == self.data.len()
    }
}

fn write_varuint(out: &mut Vec<u8>, mut v: u64) {
    loop {
        let b = (v & 0x7f) as u8;
        v >>= 7;
        if v == 0 {
            out.push(b);
            return;
        }
        out.push(b | 0x80);
    }
}

fn write_varbytes(out: &mut Vec<u8>, bytes: &[u8]) {
    write_varuint(out, bytes.len() as u64);
    out.extend_from_slice(bytes);
}

fn uri_charset_ok(uri: &[u8]) -> bool {
    uri.iter()
        .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'/' | b':' | b'-'))
}

fn read_attestation(r: &mut Reader<'_>) -> Result<Attestation> {
    let tag: [u8; 8] = r.take(8)?.try_into().expect("8 bytes");
    let payload = r.varbytes(OTS_MAX_ATTESTATION_PAYLOAD, 0, "attestation payload")?;
    let mut p = Reader::new(payload);
    let att = if tag == TAG_BITCOIN {
        let height = p.varuint()?;
        if height > OTS_MAX_HEIGHT {
            bail!("Bitcoin attestation height {height} exceeds {OTS_MAX_HEIGHT}");
        }
        Attestation::Bitcoin { height }
    } else if tag == TAG_PENDING {
        let uri = p.varbytes(OTS_MAX_URI_LEN, 0, "pending attestation URI")?;
        if !uri_charset_ok(uri) {
            bail!("pending attestation URI contains a character outside A-Z a-z 0-9 . _ / : -");
        }
        Attestation::Pending {
            uri: String::from_utf8(uri.to_vec()).expect("ASCII charset checked"),
        }
    } else {
        return Ok(Attestation::Unknown {
            tag,
            payload: payload.to_vec(),
        });
    };
    if !p.at_eof() {
        bail!("trailing bytes inside an attestation payload");
    }
    Ok(att)
}

fn unsupported_op_name(tag: u8) -> Option<&'static str> {
    match tag {
        0x02 => Some("sha1"),
        0x03 => Some("ripemd160"),
        0x67 => Some("keccak256"),
        _ => None,
    }
}

fn read_op(r: &mut Reader<'_>, tag: u8) -> Result<Op> {
    Ok(match tag {
        0x08 => Op::Sha256,
        0xf0 => Op::Append(r.varbytes(OTS_MAX_OP_ARG, 1, "append argument")?.to_vec()),
        0xf1 => Op::Prepend(r.varbytes(OTS_MAX_OP_ARG, 1, "prepend argument")?.to_vec()),
        0xf2 => Op::Reverse,
        0xf3 => Op::Hexlify,
        t => match unsupported_op_name(t) {
            Some(name) => bail!(
                "op 0x{t:02x} ({name}) is not supported by this verifier; check the proof with the ots client"
            ),
            None => bail!("unknown op tag 0x{t:02x}"),
        },
    })
}

/// One node; `limit` is the reference's remaining recursion budget (the
/// root gets 256, so 255 nested ops parse and the 256th is refused, exactly
/// as `Timestamp.deserialize`).
fn read_node(r: &mut Reader<'_>, msg: Vec<u8>, limit: usize) -> Result<Node> {
    if limit == 0 {
        bail!("timestamp nests deeper than the recursion limit of {OTS_RECURSION_LIMIT}");
    }
    let mut node = Node::new(msg);
    loop {
        let first = r.byte()?;
        let (tag, more) = if first == 0xff {
            (r.byte()?, true)
        } else {
            (first, false)
        };
        if tag == 0x00 {
            let att = read_attestation(r)?;
            if node.attestations.contains(&att) {
                bail!("attestation repeated in one timestamp node");
            }
            node.attestations.push(att);
        } else {
            let op = read_op(r, tag)?;
            if node.ops.iter().any(|(o, _)| *o == op) {
                bail!("op {op} repeated in one timestamp node");
            }
            let result = op
                .apply(&node.msg)
                .map_err(|e| anyhow!("message invalid for op {op}: {e}"))?;
            let child = read_node(r, result, limit - 1)?;
            node.ops.push((op, child));
        }
        if !more {
            break;
        }
    }
    node.sort();
    Ok(node)
}

/// Parse a bare calendar body (no magic) as a timestamp over `msg`, then
/// require EOF. Bodies over `OTS_MAX_RESPONSE_BYTES` are refused.
pub fn parse_timestamp(bytes: &[u8], msg: &[u8]) -> Result<Node> {
    if bytes.len() > OTS_MAX_RESPONSE_BYTES {
        bail!("calendar response exceeded {OTS_MAX_RESPONSE_BYTES} bytes");
    }
    if msg.len() > OTS_MAX_MSG {
        bail!("message is {} bytes (limit {OTS_MAX_MSG})", msg.len());
    }
    let mut r = Reader::new(bytes);
    let node = read_node(&mut r, msg.to_vec(), OTS_RECURSION_LIMIT)?;
    if !r.at_eof() {
        bail!("trailing bytes after the timestamp");
    }
    Ok(node)
}

// -------------------- writing --------------------

fn write_attestation(out: &mut Vec<u8>, a: &Attestation) {
    out.extend_from_slice(&a.tag());
    write_varbytes(out, &a.payload());
}

fn write_op(out: &mut Vec<u8>, op: &Op) {
    out.push(op.tag());
    if let Op::Append(a) | Op::Prepend(a) = op {
        write_varbytes(out, a);
    }
}

fn write_node(out: &mut Vec<u8>, n: &Node) {
    debug_assert!(
        !n.attestations.is_empty() || !n.ops.is_empty(),
        "an empty timestamp cannot be serialized"
    );
    let mut atts: Vec<&Attestation> = n.attestations.iter().collect();
    atts.sort_by(|a, b| attestation_cmp(a, b));
    let mut ops: Vec<&(Op, Node)> = n.ops.iter().collect();
    ops.sort_by(|a, b| op_cmp(&a.0, &b.0));
    if let Some((last, rest)) = atts.split_last() {
        for a in rest {
            out.extend_from_slice(&[0xff, 0x00]);
            write_attestation(out, a);
        }
        if ops.is_empty() {
            out.push(0x00);
        } else {
            out.extend_from_slice(&[0xff, 0x00]);
        }
        write_attestation(out, last);
    }
    if let Some((last, rest)) = ops.split_last() {
        for (op, child) in rest {
            out.push(0xff);
            write_op(out, op);
            write_node(out, child);
        }
        write_op(out, &last.0);
        write_node(out, &last.1);
    }
}

/// Canonical serialization of a bare timestamp (Python's
/// `Timestamp.serialize`).
pub fn serialize_node(n: &Node) -> Vec<u8> {
    let mut out = Vec::new();
    write_node(&mut out, n);
    out
}

/// Union `from` into `into` (`Timestamp.merge`): attestations united, ops
/// united, recursing where an op exists on both sides; then canonical order.
/// The caller guarantees both nodes are over the same message.
pub fn merge(into: &mut Node, from: Node) {
    debug_assert_eq!(into.msg, from.msg);
    for a in from.attestations {
        if !into.attestations.contains(&a) {
            into.attestations.push(a);
        }
    }
    for (op, child) in from.ops {
        match into.ops.iter_mut().find(|(o, _)| *o == op) {
            Some((_, ours)) => merge(ours, child),
            None => into.ops.push((op, child)),
        }
    }
    into.sort();
}

fn collect_leaves(n: &Node, out: &mut Vec<Leaf>) {
    for a in &n.attestations {
        out.push(Leaf {
            attestation: a.clone(),
            msg: n.msg.clone(),
        });
    }
    for (_, child) in &n.ops {
        collect_leaves(child, out);
    }
}

fn find_pending_node<'n>(n: &'n mut Node, msg: &[u8], uri: &str) -> Option<&'n mut Node> {
    let holds = n.msg == msg
        && n.attestations
            .iter()
            .any(|a| matches!(a, Attestation::Pending { uri: u } if u == uri));
    if holds {
        return Some(n);
    }
    for (_, child) in n.ops.iter_mut() {
        if let Some(found) = find_pending_node(child, msg, uri) {
            return Some(found);
        }
    }
    None
}

impl Proof {
    /// Parse a complete `.ots` file with every bound.
    pub fn parse(bytes: &[u8]) -> Result<Proof> {
        if bytes.len() > OTS_MAX_FILE_BYTES {
            bail!(
                "proof is {} bytes (limit {OTS_MAX_FILE_BYTES})",
                bytes.len()
            );
        }
        let mut r = Reader::new(bytes);
        let magic = r
            .take(OTS_MAGIC.len())
            .map_err(|_| anyhow!("not an OpenTimestamps proof (bad magic)"))?;
        if magic != OTS_MAGIC {
            bail!("not an OpenTimestamps proof (bad magic)");
        }
        let version = r.byte()?;
        if u64::from(version) != OTS_VERSION {
            bail!("unsupported OpenTimestamps proof version {version}");
        }
        let tag = r.byte()?;
        if tag != OTS_DIGEST_TAG_SHA256 {
            match tag {
                0x02 | 0x03 | 0x67 => bail!(
                    "unsupported file digest 0x{tag:02x} (this verifier reads SHA-256 proofs only)"
                ),
                t => bail!("unknown file digest tag 0x{t:02x}"),
            }
        }
        let digest: [u8; 32] = r.take(32)?.try_into().expect("32 bytes");
        let root = read_node(&mut r, digest.to_vec(), OTS_RECURSION_LIMIT)?;
        if !r.at_eof() {
            bail!("trailing bytes after the proof");
        }
        Ok(Proof { digest, root })
    }

    /// Canonical `.ots` bytes.
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut out = OTS_MAGIC.to_vec();
        out.push(OTS_VERSION as u8);
        out.push(OTS_DIGEST_TAG_SHA256);
        out.extend_from_slice(&self.digest);
        write_node(&mut out, &self.root);
        out
    }

    /// `digest → append(nonce) → sha256 → response`: the shape the
    /// reference client's `ots stamp` writes. `response.msg` must be the
    /// commitment `SHA256(digest ‖ nonce)`.
    pub fn compose(
        digest: &[u8; 32],
        nonce: &[u8; OTS_NONCE_LEN],
        response: Node,
    ) -> Result<Proof> {
        let c = commitment(digest, nonce);
        if response.msg != c {
            bail!("calendar response is not over the commitment SHA256(digest ‖ nonce)");
        }
        let mut blinded = Node::new([digest.as_slice(), nonce.as_slice()].concat());
        blinded.ops.push((Op::Sha256, response));
        let mut root = Node::new(digest.to_vec());
        root.ops.push((Op::Append(nonce.to_vec()), blinded));
        Ok(Proof {
            digest: *digest,
            root,
        })
    }

    /// Every leaf, in serialization order.
    pub fn leaves(&self) -> Vec<Leaf> {
        let mut out = Vec::new();
        collect_leaves(&self.root, &mut out);
        out
    }

    /// Any Bitcoin leaf ⇒ the lowest height; else any pending leaf ⇒
    /// pending; else no known attestation.
    pub fn state(&self) -> ProofState {
        let leaves = self.leaves();
        let lowest = leaves
            .iter()
            .filter_map(|l| match l.attestation {
                Attestation::Bitcoin { height } => Some(height),
                _ => None,
            })
            .min();
        match lowest {
            Some(height) => ProofState::Bitcoin { height },
            None if leaves
                .iter()
                .any(|l| matches!(l.attestation, Attestation::Pending { .. })) =>
            {
                ProofState::Pending
            }
            None => ProofState::NoKnownAttestation,
        }
    }

    /// `(uri, msg)` of every pending leaf, in serialization order.
    pub fn pending_leaves(&self) -> Vec<(String, Vec<u8>)> {
        self.leaves()
            .into_iter()
            .filter_map(|l| match l.attestation {
                Attestation::Pending { uri } => Some((uri, l.msg)),
                _ => None,
            })
            .collect()
    }

    /// Merge a calendar's upgrade (a timestamp over `msg`) into the node at
    /// `msg` that holds `Pending{uri}`. The pending leaf stays beside the new
    /// path, as `ots upgrade` leaves it.
    pub fn merge_at(&mut self, msg: &[u8], uri: &str, upgrade: Node) -> Result<()> {
        if upgrade.msg != msg {
            bail!("upgrade is not over the pending message");
        }
        let node = find_pending_node(&mut self.root, msg, uri).ok_or_else(|| {
            anyhow!(
                "no node of the proof holds a pending attestation for {uri} at message {}",
                hex::encode(msg)
            )
        })?;
        merge(node, upgrade);
        Ok(())
    }

    /// Whether every leaf of `older`, with its message, is in this proof
    /// (and both cover the same digest): a merge only adds.
    pub fn contains(&self, older: &Proof) -> bool {
        if self.digest != older.digest {
            return false;
        }
        let mine = self.leaves();
        older.leaves().iter().all(|l| mine.contains(l))
    }
}

/// `SHA256(digest ‖ nonce)`: what a calendar receives.
pub fn commitment(digest: &[u8; 32], nonce: &[u8; OTS_NONCE_LEN]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(digest);
    h.update(nonce);
    h.finalize().into()
}

/// 16 fresh random bytes (one per subject, calendar and run).
pub fn random_nonce() -> [u8; OTS_NONCE_LEN] {
    let mut nonce = [0u8; OTS_NONCE_LEN];
    rand::fill(&mut nonce[..]);
    nonce
}

/// Byte-reversed lowercase hex: how `ots` and block explorers print a
/// merkle root or block hash.
pub fn display_hex(bytes: &[u8]) -> String {
    let rev: Vec<u8> = bytes.iter().rev().copied().collect();
    hex::encode(rev)
}

// -------------------- Bitcoin headers (verify only) --------------------

/// An 80-byte Bitcoin block header the verifier supplied.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BlockHeader(pub [u8; 80]);

impl BlockHeader {
    /// Bytes 36..68, in header (internal) byte order.
    pub fn merkle_root(&self) -> [u8; 32] {
        self.0[36..68].try_into().expect("32 bytes")
    }

    /// SHA256d of the header, internal byte order.
    pub fn block_hash(&self) -> [u8; 32] {
        Sha256::digest(Sha256::digest(self.0)).into()
    }

    pub fn block_hash_display(&self) -> String {
        display_hex(&self.block_hash())
    }

    /// nBits, bytes 72..76 little-endian.
    pub fn bits(&self) -> u32 {
        u32::from_le_bytes(self.0[72..76].try_into().expect("4 bytes"))
    }

    /// A SANITY check only: the header's hash is at or below the target its
    /// own nBits encodes, and that target is no easier than mainnet's
    /// minimum difficulty. It catches typos, a testnet or regtest header and
    /// the wrong file; it is not evidence that the header is on the real
    /// chain — the header source is the verifier's trust root.
    pub fn pow_sane(&self) -> bool {
        let (Some(target), Some(limit)) = (
            compact_target(self.bits()),
            compact_target(BITCOIN_POW_LIMIT_BITS),
        ) else {
            return false;
        };
        let mut hash_be = self.block_hash();
        hash_be.reverse();
        target <= limit && hash_be <= target
    }
}

/// Decode a compact target into a big-endian 256-bit number; `None` for a
/// negative compact (bit 0x00800000), zero, or overflow.
pub fn compact_target(bits: u32) -> Option<[u8; 32]> {
    if bits & 0x0080_0000 != 0 {
        return None;
    }
    let exp = (bits >> 24) as usize;
    let mantissa = bits & 0x007f_ffff;
    let mut out = [0u8; 32];
    if exp <= 3 {
        let v = mantissa >> (8 * (3 - exp));
        out[28..].copy_from_slice(&v.to_be_bytes());
    } else {
        // Byte k of the mantissa (k = 0 least significant) lands at
        // significance (exp - 3 + k) bytes.
        for k in 0..3usize {
            let b = ((mantissa >> (8 * k)) & 0xff) as u8;
            if b == 0 {
                continue;
            }
            let pos = exp - 3 + k;
            if pos >= 32 {
                return None;
            }
            out[31 - pos] = b;
        }
    }
    if out.iter().all(|&b| b == 0) {
        return None;
    }
    Some(out)
}

/// The operator's header file, by height.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HeaderFile {
    pub path: String,
    pub by_height: std::collections::BTreeMap<u64, BlockHeader>,
}

/// Load `<height> <160 hex>` lines (blank lines and `#` comments ignored),
/// each header passing [`BlockHeader::pow_sane`]. Errors read
/// `bitcoin headers {path}:{line}: …`.
pub fn load_headers(path: &std::path::Path) -> Result<HeaderFile> {
    let shown = path.display().to_string();
    let text = std::fs::read_to_string(path)
        .map_err(|e| anyhow!("bitcoin headers {shown}: cannot read ({e})"))?;
    let mut by_height = std::collections::BTreeMap::new();
    for (i, raw) in text.lines().enumerate() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let at = |msg: String| anyhow!("bitcoin headers {shown}:{}: {msg}", i + 1);
        let parts: Vec<&str> = line.split_whitespace().collect();
        let format_err = || at("expected \"<height> <160 hex characters>\"".to_string());
        let [h, hx] = parts.as_slice() else {
            return Err(format_err());
        };
        if !h.bytes().all(|b| b.is_ascii_digit()) {
            return Err(format_err());
        }
        let height: u64 = h.parse().map_err(|_| format_err())?;
        if height > OTS_MAX_HEIGHT {
            return Err(at(format!("height {height} exceeds {OTS_MAX_HEIGHT}")));
        }
        if hx.len() != 160 {
            return Err(format_err());
        }
        let bytes = hex::decode(hx).map_err(|_| format_err())?;
        let header = BlockHeader(bytes.try_into().expect("80 bytes"));
        if by_height.get(&height).is_some_and(|prev| *prev != header) {
            return Err(at(format!(
                "block {height} listed twice with different headers"
            )));
        }
        if !header.pow_sane() {
            return Err(at(format!(
                "block {height}: header fails the proof-of-work sanity check (hash above its own target, \
                 or a target easier than Bitcoin's minimum difficulty); not a Bitcoin mainnet header"
            )));
        }
        by_height.insert(height, header);
    }
    Ok(HeaderFile {
        path: shown,
        by_height,
    })
}

/// What one Bitcoin leaf says when compared with the supplied headers.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum BitcoinCheck {
    /// The leaf's message is the header's merkle root.
    Matches {
        height: u64,
        block_hash_display: String,
    },
    /// The supplied header's merkle root differs from the proof's.
    Mismatch {
        height: u64,
        proof_display: String,
        header_display: String,
    },
    /// No header for that height was supplied (or none at all).
    NoHeader { height: u64 },
    /// The leaf's message is not 32 bytes: it cannot be a merkle root.
    Malformed { height: u64, len: usize },
}

/// One entry per Bitcoin leaf, in serialization order; `None` ⇒ every
/// well-formed leaf is `NoHeader`.
pub fn check_bitcoin_leaves(proof: &Proof, headers: Option<&HeaderFile>) -> Vec<BitcoinCheck> {
    let mut out = Vec::new();
    for leaf in proof.leaves() {
        let Attestation::Bitcoin { height } = leaf.attestation else {
            continue;
        };
        if leaf.msg.len() != 32 {
            out.push(BitcoinCheck::Malformed {
                height,
                len: leaf.msg.len(),
            });
            continue;
        }
        match headers.and_then(|h| h.by_height.get(&height)) {
            None => out.push(BitcoinCheck::NoHeader { height }),
            Some(header) if header.merkle_root().as_slice() == leaf.msg.as_slice() => {
                out.push(BitcoinCheck::Matches {
                    height,
                    block_hash_display: header.block_hash_display(),
                })
            }
            Some(header) => out.push(BitcoinCheck::Mismatch {
                height,
                proof_display: display_hex(&leaf.msg),
                header_display: display_hex(&header.merkle_root()),
            }),
        }
    }
    out
}

/// The refusal text for a Bitcoin leaf that cannot be a merkle root.
fn malformed_text(height: u64, len: usize) -> String {
    format!(
        "Bitcoin attestation for block {height} is over a {len}-byte message; a merkle root is 32 bytes"
    )
}

fn refuse_malformed(proof: &Proof) -> Result<()> {
    for c in check_bitcoin_leaves(proof, None) {
        if let BitcoinCheck::Malformed { height, len } = c {
            bail!("{}", malformed_text(height, len));
        }
    }
    Ok(())
}

// -------------------- storage: the frozen ots_anchors table --------------------

/// The frozen `CREATE TABLE`. Newer columns go in [`OTS_EXTRA_COLUMNS`].
pub const OTS_ANCHORS_DDL: &str = "CREATE TABLE IF NOT EXISTS ots_anchors (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  created_bucket_start INTEGER NOT NULL,
  created_bucket_size INTEGER NOT NULL,
  subject TEXT NOT NULL,
  subject_hash BLOB NOT NULL,
  calendar_url TEXT NOT NULL,
  proof BLOB NOT NULL,
  attestation TEXT NOT NULL,
  bitcoin_height INTEGER,
  upgraded_bucket_start INTEGER,
  ledger_id INTEGER
);";

/// Nullable columns added after the frozen `CREATE TABLE` (none yet; the
/// `tsa_anchors` precedent, reserved from day one).
const OTS_EXTRA_COLUMNS: [(&str, &str); 0] = [];

/// The fixed columns, in select order.
const OTS_COLUMN_NAMES: [&str; 11] = [
    "id",
    "created_bucket_start",
    "created_bucket_size",
    "subject",
    "subject_hash",
    "calendar_url",
    "proof",
    "attestation",
    "bitcoin_height",
    "upgraded_bucket_start",
    "ledger_id",
];

/// A stored OpenTimestamps proof over a ledger head or a digest.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OtsAnchorRecord {
    pub id: i64,
    pub created_bucket: TimeBucket,
    /// An `AnchorSubject` literal (unknown text reads as a digest anchor).
    pub subject: String,
    pub subject_hash: [u8; 32],
    /// The calendar the commitment was submitted to (provenance only; the
    /// pending URIs are always re-derived from `proof`).
    pub calendar_url: String,
    /// The complete canonical `.ots` file over `subject_hash`.
    pub proof: Vec<u8>,
    /// Cache: `pending` | `bitcoin`, re-derived from `proof` by verifiers.
    pub attestation: String,
    /// Cache: the lowest Bitcoin height among the proof's leaves.
    pub bitcoin_height: Option<i64>,
    /// The 10-minute bucket of the run that stored the Bitcoin path.
    pub upgraded_bucket_start: Option<i64>,
    /// Ledger row id of the anchored head at insert time (diagnosis only).
    pub ledger_id: Option<i64>,
}

impl OtsAnchorRecord {
    pub fn subject_kind(&self) -> Option<AnchorSubject> {
        AnchorSubject::parse(&self.subject)
    }

    pub fn subject_kind_or_digest(&self) -> AnchorSubject {
        AnchorSubject::from_row(&self.subject)
    }
}

/// Create the table if missing. Only `insert_ots_anchor` calls this: a DB
/// that never stamps OpenTimestamps never grows the table.
pub fn ensure_ots_table(conn: &Connection) -> Result<()> {
    conn.execute_batch(OTS_ANCHORS_DDL)?;
    crate::storage::ensure_columns(conn, OTS_ANCHORS_TABLE, &OTS_EXTRA_COLUMNS)?;
    Ok(())
}

/// Whether the database has an `ots_anchors` table. Absence reads as "no
/// OpenTimestamps anchors"; readers never create it.
pub fn ots_table_exists(conn: &Connection) -> Result<bool> {
    Ok(conn
        .query_row(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1",
            [OTS_ANCHORS_TABLE],
            |_| Ok(true),
        )
        .optional()?
        .unwrap_or(false))
}

/// Store a proof over `hash`. The caches are derived from the proof; a
/// proof over another digest, with no pending or Bitcoin leaf, or with a
/// Bitcoin leaf that cannot be a merkle root is refused.
pub fn insert_ots_anchor(
    conn: &Connection,
    bucket: TimeBucket,
    subject: AnchorSubject,
    hash: &[u8; 32],
    calendar_url: &str,
    proof: &Proof,
) -> Result<i64> {
    if proof.digest != *hash {
        bail!("proof covers {} but the row would claim {}", hex::encode(proof.digest), hex::encode(hash));
    }
    let state = proof.state();
    let Some(column) = state.column() else {
        bail!("proof carries no Pending or Bitcoin attestation; not stored");
    };
    refuse_malformed(proof)?;
    let bytes = proof.to_bytes();
    if bytes.len() > OTS_MAX_FILE_BYTES {
        bail!("proof is {} bytes (limit {OTS_MAX_FILE_BYTES})", bytes.len());
    }
    ensure_ots_table(conn)?;
    let ledger_id = tsa::ledger_position(conn, subject, hash)?;
    let height = state.height().map(|h| h as i64);
    let upgraded = height.map(|_| bucket.start_epoch_s as i64);
    conn.execute(
        "INSERT INTO ots_anchors (created_bucket_start, created_bucket_size, subject,
            subject_hash, calendar_url, proof, attestation, bitcoin_height,
            upgraded_bucket_start, ledger_id) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)",
        rusqlite::params![
            bucket.start_epoch_s as i64,
            bucket.size_s as i64,
            subject.as_str(),
            hash.as_slice(),
            calendar_url,
            bytes,
            column,
            height,
            upgraded,
            ledger_id,
        ],
    )?;
    Ok(conn.last_insert_rowid())
}

/// The one UPDATE: a pending row gains its Bitcoin path. `new` must cover
/// the same digest, contain every leaf of `old`, and attest a Bitcoin block
/// over a 32-byte message. `Ok(false)` when the row is not pending (or
/// gone).
pub fn record_ots_upgrade(
    conn: &Connection,
    id: i64,
    bucket: TimeBucket,
    old: &Proof,
    new: &Proof,
) -> Result<bool> {
    if new.digest != old.digest {
        bail!("upgraded proof covers a different digest; not stored");
    }
    if !new.contains(old) {
        bail!("upgraded proof does not contain the stored proof; not stored");
    }
    let ProofState::Bitcoin { height } = new.state() else {
        bail!("upgraded proof attests no Bitcoin block; not stored");
    };
    refuse_malformed(new)?;
    let bytes = new.to_bytes();
    if bytes.len() > OTS_MAX_FILE_BYTES {
        bail!("proof is {} bytes (limit {OTS_MAX_FILE_BYTES})", bytes.len());
    }
    let changed = conn.execute(
        "UPDATE ots_anchors SET proof = ?2, attestation = 'bitcoin', bitcoin_height = ?3, upgraded_bucket_start = ?4 WHERE id = ?1 AND attestation = 'pending'",
        rusqlite::params![id, bytes, height as i64, bucket.start_epoch_s as i64],
    )?;
    Ok(changed == 1)
}

fn ots_select_sql(conn: &Connection) -> Result<String> {
    let mut stmt = conn.prepare("PRAGMA table_info(ots_anchors)")?;
    let present: std::collections::HashSet<String> = stmt
        .query_map([], |row| row.get::<_, String>(1))?
        .collect::<std::result::Result<_, _>>()?;
    let mut cols: Vec<String> = OTS_COLUMN_NAMES.iter().map(|c| c.to_string()).collect();
    for (name, _) in OTS_EXTRA_COLUMNS {
        if present.contains(name) {
            cols.push(name.to_string());
        } else {
            cols.push(format!("NULL AS {name}"));
        }
    }
    Ok(format!(
        "SELECT {} FROM ots_anchors ORDER BY id",
        cols.join(", ")
    ))
}

type RawOtsRow = Vec<rusqlite::types::Value>;

fn ots_i64(raw: &RawOtsRow, idx: usize) -> std::result::Result<i64, String> {
    use rusqlite::types::Value;
    let name = OTS_COLUMN_NAMES[idx];
    match &raw[idx] {
        Value::Integer(v) => Ok(*v),
        Value::Null => Err(format!("{name} is NULL")),
        _ => Err(format!("{name} is not an INTEGER")),
    }
}

fn ots_opt_i64(raw: &RawOtsRow, idx: usize) -> std::result::Result<Option<i64>, String> {
    match &raw[idx] {
        rusqlite::types::Value::Null => Ok(None),
        _ => ots_i64(raw, idx).map(Some),
    }
}

fn ots_text(raw: &RawOtsRow, idx: usize) -> std::result::Result<String, String> {
    use rusqlite::types::Value;
    let name = OTS_COLUMN_NAMES[idx];
    match &raw[idx] {
        Value::Text(v) => Ok(v.clone()),
        Value::Null => Err(format!("{name} is NULL")),
        _ => Err(format!("{name} is not TEXT")),
    }
}

fn ots_blob(raw: &RawOtsRow, idx: usize) -> std::result::Result<Vec<u8>, String> {
    use rusqlite::types::Value;
    let name = OTS_COLUMN_NAMES[idx];
    match &raw[idx] {
        Value::Blob(v) => Ok(v.clone()),
        Value::Null => Err(format!("{name} is NULL")),
        _ => Err(format!("{name} is not a BLOB")),
    }
}

fn decode_ots_row(raw: &RawOtsRow) -> std::result::Result<OtsAnchorRecord, String> {
    let id = ots_i64(raw, 0)?;
    let start = ots_i64(raw, 1)?;
    let size = ots_i64(raw, 2)?;
    let subject = ots_text(raw, 3)?;
    let hash = ots_blob(raw, 4)?;
    let calendar_url = ots_text(raw, 5)?;
    let proof = ots_blob(raw, 6)?;
    let attestation = ots_text(raw, 7)?;
    let bitcoin_height = ots_opt_i64(raw, 8)?;
    let upgraded_bucket_start = ots_opt_i64(raw, 9)?;
    let ledger_id = ots_opt_i64(raw, 10)?;
    let subject_hash: [u8; 32] = hash.try_into().map_err(|_| "subject_hash size")?;
    Ok(OtsAnchorRecord {
        id,
        created_bucket: TimeBucket {
            start_epoch_s: start as u64,
            size_s: size as u32,
        },
        subject,
        subject_hash,
        calendar_url,
        proof,
        attestation,
        bitcoin_height,
        upgraded_bucket_start,
        ledger_id,
    })
}

/// Every row in id order, a malformed one surfaced as `(id, Err(reason))`
/// (`corrupt ots anchor {id}: {reason}`) instead of aborting the read.
/// `Ok(vec![])` when the table is absent.
pub fn list_ots_anchors_lenient(conn: &Connection) -> Result<Vec<(i64, Result<OtsAnchorRecord>)>> {
    use rusqlite::types::Value;
    if !ots_table_exists(conn)? {
        return Ok(Vec::new());
    }
    let sql = ots_select_sql(conn)?;
    let mut stmt = conn.prepare(&sql)?;
    let width = OTS_COLUMN_NAMES.len() + OTS_EXTRA_COLUMNS.len();
    let rows = stmt.query_map([], |row| {
        let mut raw: RawOtsRow = Vec::with_capacity(width);
        for i in 0..width {
            raw.push(row.get_ref(i)?.into());
        }
        Ok(raw)
    })?;
    let mut out = Vec::new();
    for row in rows {
        let raw = row?;
        let id = match &raw[0] {
            Value::Integer(id) => *id,
            _ => -1,
        };
        out.push((
            id,
            decode_ots_row(&raw).map_err(|reason| anyhow!("corrupt ots anchor {id}: {reason}")),
        ));
    }
    Ok(out)
}

/// Every row in id order; a malformed row is a hard error.
pub fn list_ots_anchors(conn: &Connection) -> Result<Vec<OtsAnchorRecord>> {
    list_ots_anchors_lenient(conn)?
        .into_iter()
        .map(|(_, row)| row)
        .collect()
}

/// Every well-formed row whose `attestation` is `pending`, in id order.
pub fn pending_ots_anchors(conn: &Connection) -> Result<Vec<OtsAnchorRecord>> {
    Ok(list_ots_anchors_lenient(conn)?
        .into_iter()
        .filter_map(|(_, row)| row.ok())
        .filter(|r| r.attestation == OTS_ATTESTATION_PENDING)
        .collect())
}

/// `relabel --ots`: set a row's subject to `digest` (the proof untouched).
pub fn relabel_ots_anchor_to_digest(conn: &Connection, id: i64) -> Result<()> {
    let n = conn.execute(
        "UPDATE ots_anchors SET subject = 'digest' WHERE id = ?1",
        [id],
    )?;
    if n != 1 {
        bail!("no ots anchor #{id}");
    }
    Ok(())
}

// -------------------- offline files --------------------

/// `{subject}.{calendar}.ots-request.json`: one nonced submission. It
/// names no subject — import re-derives it from the ledgers.
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct OtsRequestFile {
    pub format: String,
    pub subject_hash: String,
    pub nonce: String,
    pub calendar: String,
    pub calendar_url: String,
}

/// `ots-{id}.ots-upgrade.json`: one upgrade fetch for a pending row.
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct OtsUpgradeFile {
    pub format: String,
    pub anchor_id: i64,
    pub uri: String,
    pub message: String,
}

impl OtsRequestFile {
    /// Check the format literal and hex lengths; `(hash, nonce)`.
    pub fn decode(&self) -> Result<([u8; 32], [u8; OTS_NONCE_LEN])> {
        if self.format != OTS_REQUEST_FORMAT {
            bail!(
                "format must be \"{OTS_REQUEST_FORMAT}\" (got \"{}\")",
                self.format
            );
        }
        let hash: [u8; 32] = hex::decode(&self.subject_hash)
            .ok()
            .and_then(|b| b.try_into().ok())
            .ok_or_else(|| anyhow!("subject_hash must be 64 hex characters"))?;
        let nonce: [u8; OTS_NONCE_LEN] = hex::decode(&self.nonce)
            .ok()
            .and_then(|b| b.try_into().ok())
            .ok_or_else(|| anyhow!("nonce must be 32 hex characters"))?;
        Ok((hash, nonce))
    }
}

impl OtsUpgradeFile {
    /// Check the format literal; the pending message bytes.
    pub fn decode(&self) -> Result<Vec<u8>> {
        if self.format != OTS_UPGRADE_FORMAT {
            bail!(
                "format must be \"{OTS_UPGRADE_FORMAT}\" (got \"{}\")",
                self.format
            );
        }
        let msg = hex::decode(&self.message).map_err(|_| anyhow!("message must be hex"))?;
        if msg.is_empty() || msg.len() > OTS_MAX_MSG {
            bail!("message must be 1..={OTS_MAX_MSG} bytes");
        }
        Ok(msg)
    }
}

/// The pending leaf the upgrade pass polls for a row: the first (canonical
/// order) whose URI equals the row's `calendar_url`.
pub fn pollable_leaf(proof: &Proof, calendar_url: &str) -> Option<(String, Vec<u8>)> {
    proof
        .pending_leaves()
        .into_iter()
        .find(|(uri, _)| uri == calendar_url)
}

/// The stamp acceptance rule: the response must hold `Pending{calendar_url}`
/// or a Bitcoin leaf; anything else came from a relay we did not declare.
pub fn accept_response(node: &Node, calendar_name: &str, calendar_url: &str) -> Result<()> {
    let mut leaves = Vec::new();
    collect_leaves(node, &mut leaves);
    let ok = leaves.iter().any(|l| match &l.attestation {
        Attestation::Pending { uri } => uri == calendar_url,
        Attestation::Bitcoin { .. } => true,
        Attestation::Unknown { .. } => false,
    });
    if !ok {
        bail!(
            "calendar {calendar_name} answered with no pending attestation for {calendar_url} and no Bitcoin attestation (declare calendar URLs, not aggregator pools)"
        );
    }
    Ok(())
}

// -------------------- the calendar leg --------------------

/// The two calendar calls, behind a trait so the leg is tested offline.
pub trait CalendarTransport {
    /// POST {calendar_url}/digest; Ok(body) on 200.
    fn submit(&self, calendar_url: &str, commitment: &[u8; 32], allow_http: bool) -> Result<Vec<u8>>;
    /// GET {uri}/timestamp/{hex(msg)}; Ok(None) on 404.
    fn fetch(&self, uri: &str, msg: &[u8], allow_http: bool) -> Result<Option<Vec<u8>>>;
}

/// The real transport: ureq, 30 s, no redirects, ureq's default User-Agent,
/// `Accept: application/vnd.opentimestamps.v1`, 10 000-byte bodies.
#[cfg(feature = "ots")]
pub struct UreqCalendar;

#[cfg(feature = "ots")]
impl UreqCalendar {
    fn agent() -> ureq::Agent {
        ureq::Agent::config_builder()
            .timeout_global(Some(std::time::Duration::from_secs(30)))
            .max_redirects(0)
            .build()
            .into()
    }

    fn scheme_ok(url: &str, allow_http: bool) -> Result<()> {
        if url.starts_with("https://") || (allow_http && url.starts_with("http://")) {
            Ok(())
        } else {
            bail!("calendar URL must be https:// (set \"allow_http\": true to override)")
        }
    }

    fn read_body(mut response: ureq::http::Response<ureq::Body>, url: &str) -> Result<Vec<u8>> {
        let status = response.status().as_u16();
        if status != 200 {
            bail!("calendar {url} answered HTTP {status} (redirects are not followed)");
        }
        let body = match response
            .body_mut()
            .with_config()
            .limit(OTS_MAX_RESPONSE_BYTES as u64 + 1)
            .read_to_vec()
        {
            Ok(b) => b,
            Err(ureq::Error::BodyExceedsLimit(_)) => {
                bail!("calendar response exceeded {OTS_MAX_RESPONSE_BYTES} bytes")
            }
            Err(e) => bail!("reading the calendar response failed: {e}"),
        };
        if body.len() > OTS_MAX_RESPONSE_BYTES {
            bail!("calendar response exceeded {OTS_MAX_RESPONSE_BYTES} bytes");
        }
        Ok(body)
    }
}

#[cfg(feature = "ots")]
impl CalendarTransport for UreqCalendar {
    fn submit(&self, calendar_url: &str, commitment: &[u8; 32], allow_http: bool) -> Result<Vec<u8>> {
        Self::scheme_ok(calendar_url, allow_http)?;
        let url = format!("{calendar_url}/digest");
        let response = Self::agent()
            .post(&url)
            .header("Accept", OTS_ACCEPT)
            .send(commitment.as_slice())
            .map_err(|e| anyhow!("calendar request to {url} failed: {e}"))?;
        Self::read_body(response, &url)
    }

    fn fetch(&self, uri: &str, msg: &[u8], allow_http: bool) -> Result<Option<Vec<u8>>> {
        Self::scheme_ok(uri, allow_http)?;
        let url = format!("{uri}/timestamp/{}", hex::encode(msg));
        match Self::agent().get(&url).header("Accept", OTS_ACCEPT).call() {
            Ok(response) => Self::read_body(response, &url).map(Some),
            Err(ureq::Error::StatusCode(404)) => Ok(None),
            Err(e) => bail!("calendar request to {url} failed: {e}"),
        }
    }
}

/// One subject the leg stamps (the ledger head, or a sentinel stored as a
/// digest).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PlannedOts {
    pub stored_as: AnchorSubject,
    pub hash: [u8; 32],
    pub sentinel: bool,
}

/// A stored stamp.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OtsStored {
    pub id: i64,
    pub stored_as: AnchorSubject,
    pub hash: [u8; 32],
    pub sentinel: bool,
    pub calendar: String,
    pub state: ProofState,
    pub first_pending_uri: Option<String>,
}

/// A failed submission.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OtsSubmitFailure {
    pub stored_as: AnchorSubject,
    pub hash: [u8; 32],
    pub calendar: String,
    pub url: String,
    pub error: String,
}

/// Everything the leg did, in order, for the binary to print.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum OtsLegEvent {
    /// Before each upgrade GET.
    Upgrading { id: i64, uri: String },
    Upgraded { id: i64, subject: String, height: u64 },
    StillPending { id: i64, uri: String },
    AnsweredWithoutBitcoin { id: i64, uri: String },
    UpgradeFailed { id: i64, uri: String, error: String },
    BeyondHorizon { id: i64 },
    NotPolledUndeclared { id: i64, uri: String },
    /// Before each submission (index into `planned`, calendar name and url).
    Stamping { planned: usize, calendar: String, url: String },
    /// Index into `OtsLegReport.stored`.
    Stored(usize),
    /// Index into `OtsLegReport.submit_failures`.
    SubmitFailed(usize),
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct OtsLegReport {
    pub submissions: usize,
    pub fetches: usize,
    pub stored: Vec<OtsStored>,
    pub submit_failures: Vec<OtsSubmitFailure>,
    /// (id, subject, lowest height)
    pub upgraded: Vec<(i64, String, u64)>,
    /// (id, uri)
    pub still_pending: Vec<(i64, String)>,
    pub answered_without_bitcoin: Vec<(i64, String)>,
    /// (id, uri, error)
    pub upgrade_errors: Vec<(i64, String, String)>,
    pub beyond_horizon: Vec<i64>,
    pub not_polled_undeclared: Vec<(i64, String)>,
    /// The same outcomes, in the order they happened.
    pub events: Vec<OtsLegEvent>,
}

/// Merge an upgrade body into a stored proof and record it. `Ok(Some(h))`
/// when the row now attests Bitcoin block `h`; `Ok(None)` when the answer
/// carried no Bitcoin path (nothing stored).
pub fn apply_upgrade(
    conn: &Connection,
    row: &OtsAnchorRecord,
    now: TimeBucket,
    uri: &str,
    msg: &[u8],
    body: &[u8],
) -> Result<Option<u64>> {
    let old = Proof::parse(&row.proof)?;
    let upgrade = parse_timestamp(body, msg)?;
    let mut merged = old.clone();
    merged.merge_at(msg, uri, upgrade)?;
    let ProofState::Bitcoin { height } = merged.state() else {
        return Ok(None);
    };
    if !record_ots_upgrade(conn, row.id, now, &old, &merged)? {
        bail!("ots anchor #{} is no longer pending; not stored", row.id);
    }
    Ok(Some(height))
}

/// The OpenTimestamps leg of `anchor-all` (and `request --policy`):
/// (1) if `upgrade`, at most one GET per pending row created within
/// `OTS_UPGRADE_HORIZON_S` — rows stamped in this run are never polled in
/// it; (2) every planned subject × every declared calendar, a fresh nonce
/// each. Upgrade misses and errors are not run failures; every calendar is
/// always asked.
pub fn run_ots_leg(
    conn: &Connection,
    policy: &OtsPolicy,
    planned: &[PlannedOts],
    transport: &dyn CalendarTransport,
    now: TimeBucket,
    upgrade: bool,
    nonce: &mut dyn FnMut() -> [u8; OTS_NONCE_LEN],
) -> Result<OtsLegReport> {
    let mut rep = OtsLegReport::default();
    if upgrade {
        for row in pending_ots_anchors(conn)? {
            let id = row.id;
            if now
                .start_epoch_s
                .saturating_sub(row.created_bucket.start_epoch_s)
                > OTS_UPGRADE_HORIZON_S
            {
                rep.beyond_horizon.push(id);
                rep.events.push(OtsLegEvent::BeyondHorizon { id });
                continue;
            }
            let proof = match Proof::parse(&row.proof) {
                Ok(p) => p,
                Err(e) => {
                    let error = format!("stored proof does not parse: {e:#}");
                    rep.upgrade_errors
                        .push((id, row.calendar_url.clone(), error.clone()));
                    rep.events.push(OtsLegEvent::UpgradeFailed {
                        id,
                        uri: row.calendar_url.clone(),
                        error,
                    });
                    continue;
                }
            };
            let leaf = pollable_leaf(&proof, &row.calendar_url);
            let entry = policy.calendar_for_url(&row.calendar_url);
            let (Some((uri, msg)), Some(entry)) = (leaf, entry) else {
                let uri = proof
                    .pending_leaves()
                    .into_iter()
                    .map(|(u, _)| u)
                    .next()
                    .unwrap_or_else(|| row.calendar_url.clone());
                rep.not_polled_undeclared.push((id, uri.clone()));
                rep.events
                    .push(OtsLegEvent::NotPolledUndeclared { id, uri });
                continue;
            };
            rep.fetches += 1;
            rep.events.push(OtsLegEvent::Upgrading {
                id,
                uri: uri.clone(),
            });
            let outcome = transport
                .fetch(&uri, &msg, entry.allow_http)
                .and_then(|body| match body {
                    None => Ok(None),
                    Some(body) => apply_upgrade(conn, &row, now, &uri, &msg, &body).map(Some),
                });
            match outcome {
                Ok(None) => {
                    rep.still_pending.push((id, uri.clone()));
                    rep.events.push(OtsLegEvent::StillPending { id, uri });
                }
                Ok(Some(Some(height))) => {
                    rep.upgraded.push((id, row.subject.clone(), height));
                    rep.events.push(OtsLegEvent::Upgraded {
                        id,
                        subject: row.subject.clone(),
                        height,
                    });
                }
                Ok(Some(None)) => {
                    rep.answered_without_bitcoin.push((id, uri.clone()));
                    rep.events
                        .push(OtsLegEvent::AnsweredWithoutBitcoin { id, uri });
                }
                Err(e) => {
                    let error = format!("{e:#}");
                    rep.upgrade_errors.push((id, uri.clone(), error.clone()));
                    rep.events
                        .push(OtsLegEvent::UpgradeFailed { id, uri, error });
                }
            }
        }
    }
    for (i, p) in planned.iter().enumerate() {
        for cal in &policy.calendars {
            rep.submissions += 1;
            rep.events.push(OtsLegEvent::Stamping {
                planned: i,
                calendar: cal.name.clone(),
                url: cal.url.clone(),
            });
            let n = nonce();
            let c = commitment(&p.hash, &n);
            let outcome = transport
                .submit(&cal.url, &c, cal.allow_http)
                .and_then(|body| parse_timestamp(&body, &c))
                .and_then(|node| {
                    accept_response(&node, &cal.name, &cal.url)?;
                    let proof = Proof::compose(&p.hash, &n, node)?;
                    let id = insert_ots_anchor(conn, now, p.stored_as, &p.hash, &cal.url, &proof)?;
                    Ok((id, proof))
                });
            match outcome {
                Ok((id, proof)) => {
                    let pend = proof.pending_leaves();
                    let first_pending_uri = pend
                        .iter()
                        .find(|(u, _)| *u == cal.url)
                        .or(pend.first())
                        .map(|(u, _)| u.clone());
                    rep.stored.push(OtsStored {
                        id,
                        stored_as: p.stored_as,
                        hash: p.hash,
                        sentinel: p.sentinel,
                        calendar: cal.name.clone(),
                        state: proof.state(),
                        first_pending_uri,
                    });
                    rep.events.push(OtsLegEvent::Stored(rep.stored.len() - 1));
                }
                Err(e) => {
                    rep.submit_failures.push(OtsSubmitFailure {
                        stored_as: p.stored_as,
                        hash: p.hash,
                        calendar: cal.name.clone(),
                        url: cal.url.clone(),
                        error: format!("{e:#}"),
                    });
                    rep.events
                        .push(OtsLegEvent::SubmitFailed(rep.submit_failures.len() - 1));
                }
            }
        }
    }
    Ok(rep)
}

#[cfg(test)]
mod tests;
