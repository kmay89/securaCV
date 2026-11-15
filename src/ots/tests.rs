//! Codec, header, storage and leg tests for `src/ots.rs`. All offline, on
//! the checked-in fixtures in `tests/fixtures/ots/` (see its README): the
//! `opentimestamps` 0.2.0 crate's two real proofs, the files derived from
//! them by `write_derived_ots_fixtures`, a synthetic genesis proof and the
//! real Bitcoin genesis header.

use super::*;
use std::cell::RefCell;
use std::path::PathBuf;

fn fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/ots")
}

fn fx(name: &str) -> Vec<u8> {
    std::fs::read(fixture_dir().join(name)).unwrap_or_else(|e| panic!("fixture {name}: {e}"))
}

fn h32(s: &str) -> [u8; 32] {
    hex::decode(s).unwrap().try_into().unwrap()
}

const PENDING_DIGEST: &str = "a70dfe69c5a0d62816781abb6e1777854718624a0d194231adb14c32ee5438a4";
// The client nonce inside the real pending.ots (tests/fixtures/ots/README.md);
// pending-bob.ots is byte-pinned to it, so it is the one literal nonce these
// tests keep: a test vector, not a deployed secret.
// codeql[rust/hard-coded-cryptographic-value]
const PENDING_NONCE: &str = "7a4605de0a5b37cb211759c6812bfe2e";
const PENDING_COMMITMENT: &str = "e1ae951b5744348aaf577a341e834464680d1c0afdea639715f148a0379d6593";
const BOB: &str = "https://bob.btc.calendar.opentimestamps.org";
const ALICE: &str = "https://alice.btc.calendar.opentimestamps.org";
const BOB_MSG_PENDING: &str =
    "58a4c2578a67a452937f8837da37dafa442aa308ced90d6b7381149c52f2802858337adaa1a92c61d5417206";
const ALICE_MSG_PENDING: &str =
    "58a4c25629bc8b28b9168fe368536eb6b370f2cf844b991792ee868ce897a4c88d91677038f2c7f4baf4bcd7";
const BITCOIN_DIGEST: &str = "6fd9c1c4f096b77e6d4457bac1c7f51010d318db483f2868d3795843f098d378";
const BOB_MSG_BITCOIN: &str =
    "5883f171aa6795c4b2d50c11a9261949a1eedfdcf8254d5adab80b95d36584163a25cfcd6d4580fc64dfa979";
const ALICE_MSG_BITCOIN: &str =
    "5883f17120599b7b1ba5dd17ee43289504b3d17636dba6009f57ddc961b104c16b0caf4b5deb8967362e06b6";
const ROOT_449399: &str = "1a1da26714e8ef3b140c5f461ee52ea6fb5d8ca8e02d73370b5b416265eb1f5e";
const ROOT_449399_DISPLAY: &str =
    "5e1feb6562415b0b37732de0a88c5dfba62ee51e465f0c143befe81467a21d1a";
const ROOT_449397: &str = "7c17a8a0d6bc1da3604ecd442fc38869983290dcbc4876bdf0fcf133791ff218";
const ROOT_449397_DISPLAY: &str =
    "18f21f7933f1fcf0bd7648bcdc9032986988c32f44cd4e60a31dbcd6a0a8177c";
const GENESIS_HEADER: &str = "0100000000000000000000000000000000000000000000000000000000000000000000003ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a29ab5f49ffff001d1dac2b7c";
const GENESIS_ROOT: &str = "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a";
const GENESIS_HASH_DISPLAY: &str =
    "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";

fn header(hex80: &str) -> BlockHeader {
    BlockHeader(hex::decode(hex80).unwrap().try_into().unwrap())
}

/// The DetachedTimestampFile prefix: MAGIC ‖ 01 ‖ 08 ‖ digest.
fn file_prefix(digest: &[u8; 32]) -> Vec<u8> {
    let mut out = OTS_MAGIC.to_vec();
    out.extend_from_slice(&[0x01, 0x08]);
    out.extend_from_slice(digest);
    out
}

fn err_text(r: Result<impl std::fmt::Debug>) -> String {
    format!("{:#}", r.unwrap_err())
}

// -------------------- derivation of the derived fixtures --------------------

/// The node of `n` holding `Pending{uri}`.
fn pending_node<'n>(n: &'n Node, uri: &str) -> Option<&'n Node> {
    if n.attestations
        .iter()
        .any(|a| matches!(a, Attestation::Pending { uri: u } if u == uri))
    {
        return Some(n);
    }
    n.ops.iter().find_map(|(_, c)| pending_node(c, uri))
}

/// The commitment node of a stamped proof: digest → append(nonce) → sha256.
fn commitment_node(p: &Proof) -> &Node {
    let (op, blinded) = &p.root.ops[0];
    assert!(matches!(op, Op::Append(n) if n.len() == OTS_NONCE_LEN));
    let (op, c) = &blinded.ops[0];
    assert_eq!(*op, Op::Sha256);
    c
}

/// The single branch of a commitment node that leads to `uri`'s pending leaf.
fn calendar_branch(c: &Node, uri: &str) -> Node {
    let (op, sub) = c
        .ops
        .iter()
        .find(|(_, sub)| pending_node(sub, uri).is_some())
        .expect("branch");
    Node {
        msg: c.msg.clone(),
        attestations: Vec::new(),
        ops: vec![(op.clone(), sub.clone())],
    }
}

fn strip_below_pending(n: &mut Node) {
    if n.attestations
        .iter()
        .any(|a| matches!(a, Attestation::Pending { .. }))
    {
        n.ops.clear();
        return;
    }
    for (_, c) in n.ops.iter_mut() {
        strip_below_pending(c);
    }
}

fn upgrade_at(p: &Proof, uri: &str) -> Node {
    let mut n = pending_node(&p.root, uri).expect("pending node").clone();
    n.attestations.clear();
    n
}

fn genesis_proof() -> Proof {
    Proof {
        digest: h32(GENESIS_ROOT),
        root: Node {
            msg: h32(GENESIS_ROOT).to_vec(),
            attestations: vec![Attestation::Bitcoin { height: 0 }],
            ops: Vec::new(),
        },
    }
}

/// Every derived fixture, computed from the two real proofs with the codec.
fn derived_fixtures() -> Vec<(&'static str, Vec<u8>)> {
    let pending = Proof::parse(&fx("pending.ots")).unwrap();
    let bitcoin = Proof::parse(&fx("bitcoin.ots")).unwrap();
    let c = commitment_node(&pending);
    let response_bob = calendar_branch(c, BOB);
    let response_alice = calendar_branch(c, ALICE);
    let nonce: [u8; 16] = hex::decode(PENDING_NONCE).unwrap().try_into().unwrap();
    let pending_bob = Proof::compose(&pending.digest, &nonce, response_bob.clone()).unwrap();
    let mut stripped = bitcoin.clone();
    strip_below_pending(&mut stripped.root);
    vec![
        ("response-bob.otsr", serialize_node(&response_bob)),
        ("response-alice.otsr", serialize_node(&response_alice)),
        ("pending-bob.ots", pending_bob.to_bytes()),
        ("bitcoin-stripped.ots", stripped.to_bytes()),
        (
            "upgrade-bob.otsr",
            serialize_node(&upgrade_at(&bitcoin, BOB)),
        ),
        (
            "upgrade-alice.otsr",
            serialize_node(&upgrade_at(&bitcoin, ALICE)),
        ),
        ("genesis.ots", genesis_proof().to_bytes()),
    ]
}

/// Writes the derived fixtures. Run once after a codec change that is
/// meant to move them: `cargo test --lib write_derived_ots_fixtures -- --ignored`.
#[test]
#[ignore]
fn write_derived_ots_fixtures() {
    for (name, bytes) in derived_fixtures() {
        std::fs::write(fixture_dir().join(name), bytes).unwrap();
    }
}

fn contains_sub(hay: &[u8], needle: &[u8]) -> bool {
    hay.windows(needle.len()).any(|w| w == needle)
}

#[test]
fn derived_ots_fixtures_are_current() {
    let want_sha = [
        (
            "response-bob.otsr",
            "367429bad7fd7a68372da640dd1449aa46dbc49da06b29f099fd8c9b1b6e9637",
        ),
        (
            "response-alice.otsr",
            "22fa7c6b5331002da3b9edfd1e21924436522b9eb285a61d5184a1bfa87c3288",
        ),
        (
            "pending-bob.ots",
            "3ed9cced375f9b29473b6d6ca95328391c635a1b8df68a82ad42b37869ea4c19",
        ),
        (
            "bitcoin-stripped.ots",
            "abf10428508436b038ddbc5de2a4858155f782a6dfff8a171ee1c515d12d7e4f",
        ),
        (
            "upgrade-bob.otsr",
            "17d3951afc464437f3e06e8157db4d4a407b9de9f115ffb1076d18688e0aeaec",
        ),
        (
            "upgrade-alice.otsr",
            "575f37f114d294d3c3cb83a05037098a1497ed8d6c2dc396fc526630658deab1",
        ),
        (
            "genesis.ots",
            "3ff23d220784c3499c21b5d7d1c1d28e7157d17d988353a10a5daac0975a74df",
        ),
    ];
    for ((name, bytes), (n2, sha)) in derived_fixtures().into_iter().zip(want_sha) {
        assert_eq!(name, n2);
        assert_eq!(
            fx(name),
            bytes,
            "{name} is stale: run write_derived_ots_fixtures"
        );
        assert_eq!(hex::encode(Sha256::digest(&bytes)), sha, "{name}");
    }
    // Independent of our serializer: every calendar body is a byte substring
    // of the real file it came from.
    let pending = fx("pending.ots");
    let bitcoin = fx("bitcoin.ots");
    assert!(contains_sub(&pending, &fx("response-bob.otsr")));
    assert!(contains_sub(&pending, &fx("response-alice.otsr")));
    assert!(contains_sub(&bitcoin, &fx("upgrade-bob.otsr")));
    assert!(contains_sub(&bitcoin, &fx("upgrade-alice.otsr")));
    assert_eq!(
        hex::encode(fx("genesis.ots")),
        "004f70656e54696d657374616d7073000050726f6f6600bf89e2e884e8929401083ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a000588960d73d719010100"
    );
}

// -------------------- 4.2 codec --------------------

#[test]
fn pending_fixture_round_trips_bytes() {
    let bytes = fx("pending.ots");
    assert_eq!(
        hex::encode(Sha256::digest(&bytes)),
        "a9f3facdfe0c71a761b679f436b3da50cee2848777cc1e6874c1b837b78d7363"
    );
    assert_eq!(&bytes[..31], &OTS_MAGIC);
    assert_eq!(bytes[31], 0x01);
    assert_eq!(bytes[32], OTS_DIGEST_TAG_SHA256);
    let p = Proof::parse(&bytes).unwrap();
    assert_eq!(p.digest, h32(PENDING_DIGEST));
    assert_eq!(p.to_bytes(), bytes);
}

#[test]
fn bitcoin_fixture_round_trips_bytes() {
    let bytes = fx("bitcoin.ots");
    assert_eq!(
        hex::encode(Sha256::digest(&bytes)),
        "9b192789b96e8626284401b323e08567306697e17934d3c64eba343371f0e54d"
    );
    let p = Proof::parse(&bytes).unwrap();
    assert_eq!(p.digest, h32(BITCOIN_DIGEST));
    assert_eq!(p.to_bytes(), bytes);
}

#[test]
fn pending_fixture_leaves() {
    let p = Proof::parse(&fx("pending.ots")).unwrap();
    let leaves = p.pending_leaves();
    assert_eq!(
        leaves,
        vec![
            (BOB.to_string(), hex::decode(BOB_MSG_PENDING).unwrap()),
            (ALICE.to_string(), hex::decode(ALICE_MSG_PENDING).unwrap()),
        ]
    );
    assert!(leaves.iter().all(|(_, m)| m.len() == 44));
    assert_eq!(p.leaves().len(), 2);
    assert_eq!(p.state(), ProofState::Pending);
    assert_eq!(p.state().column(), Some("pending"));
    assert_eq!(commitment_node(&p).msg, h32(PENDING_COMMITMENT).to_vec());
}

#[test]
fn bitcoin_fixture_leaves_pin_heights_and_roots() {
    let p = Proof::parse(&fx("bitcoin.ots")).unwrap();
    let leaves = p.leaves();
    assert_eq!(leaves.len(), 4);
    assert_eq!(
        leaves[0],
        Leaf {
            attestation: Attestation::Pending { uri: BOB.into() },
            msg: hex::decode(BOB_MSG_BITCOIN).unwrap()
        }
    );
    assert_eq!(
        leaves[1],
        Leaf {
            attestation: Attestation::Bitcoin { height: 449399 },
            msg: hex::decode(ROOT_449399).unwrap()
        }
    );
    assert_eq!(
        leaves[2],
        Leaf {
            attestation: Attestation::Pending { uri: ALICE.into() },
            msg: hex::decode(ALICE_MSG_BITCOIN).unwrap()
        }
    );
    assert_eq!(
        leaves[3],
        Leaf {
            attestation: Attestation::Bitcoin { height: 449397 },
            msg: hex::decode(ROOT_449397).unwrap()
        }
    );
    assert_eq!(display_hex(&leaves[1].msg), ROOT_449399_DISPLAY);
    assert_eq!(display_hex(&leaves[3].msg), ROOT_449397_DISPLAY);
    assert_eq!(p.state(), ProofState::Bitcoin { height: 449397 });
    assert_eq!(p.state().column(), Some("bitcoin"));
}

#[test]
fn varuint_edges() {
    // The reference caps a varuint at 9 bytes (63 bits). The `opentimestamps`
    // crate's `read_uint` (ser.rs:186-202) has no byte bound: `shift += 7`
    // grows until the shift overflows. 2^64-1 needs 10 bytes, so the
    // largest round trip is 2^63-1.
    for v in [0u64, 0x7f, 0x80, 300, (1u64 << 63) - 1] {
        let mut buf = Vec::new();
        write_varuint(&mut buf, v);
        let mut r = Reader::new(&buf);
        assert_eq!(r.varuint().unwrap(), v);
        assert!(r.at_eof());
    }
    let mut ten = Vec::new();
    write_varuint(&mut ten, 1u64 << 63);
    assert_eq!(ten.len(), 10);
    assert_eq!(
        Reader::new(&ten).varuint().unwrap_err().to_string(),
        "varuint exceeds 9 bytes"
    );
    let mut max = Vec::new();
    write_varuint(&mut max, u64::MAX);
    assert!(Reader::new(&max).varuint().is_err());
}

#[test]
fn header_refusals() {
    let good = fx("genesis.ots");
    let mut bad_magic = good.clone();
    bad_magic[1] = b'X';
    assert_eq!(
        err_text(Proof::parse(&bad_magic)),
        "not an OpenTimestamps proof (bad magic)"
    );
    let mut v2 = good.clone();
    v2[31] = 2;
    assert_eq!(
        err_text(Proof::parse(&v2)),
        "unsupported OpenTimestamps proof version 2"
    );
    let mut sha1 = good.clone();
    sha1[32] = 0x02;
    assert_eq!(
        err_text(Proof::parse(&sha1)),
        "unsupported file digest 0x02 (this verifier reads SHA-256 proofs only)"
    );
    let mut trailing = good.clone();
    trailing.push(0);
    assert_eq!(
        err_text(Proof::parse(&trailing)),
        "trailing bytes after the proof"
    );
    let big = vec![0u8; OTS_MAX_FILE_BYTES + 1];
    assert_eq!(
        err_text(Proof::parse(&big)),
        "proof is 65537 bytes (limit 65536)"
    );
    assert!(Proof::parse(&good[..good.len() - 1]).is_err());
}

/// A file over `digest` whose root carries the given raw timestamp bytes.
fn file_with(digest: &[u8; 32], body: &[u8]) -> Vec<u8> {
    let mut f = file_prefix(digest);
    f.extend_from_slice(body);
    f
}

/// Bytes of a Bitcoin{0} leaf (`00 ‖ tag ‖ 01 ‖ 00`).
fn btc_leaf(height: u64) -> Vec<u8> {
    let mut out = vec![0x00];
    write_attestation(&mut out, &Attestation::Bitcoin { height });
    out
}

#[test]
fn op_refusals() {
    let d = [7u8; 32];
    let refuse = |body: Vec<u8>| err_text(Proof::parse(&file_with(&d, &body)));
    let mut b = vec![0x03];
    b.extend(btc_leaf(0));
    assert_eq!(
        refuse(b),
        "op 0x03 (ripemd160) is not supported by this verifier; check the proof with the ots client"
    );
    let mut b = vec![0x67];
    b.extend(btc_leaf(0));
    assert_eq!(
        refuse(b),
        "op 0x67 (keccak256) is not supported by this verifier; check the proof with the ots client"
    );
    let mut b = vec![0x02];
    b.extend(btc_leaf(0));
    assert!(refuse(b).starts_with("op 0x02 (sha1) is not supported"));
    let mut b = vec![0x42];
    b.extend(btc_leaf(0));
    assert_eq!(refuse(b), "unknown op tag 0x42");
    let mut b = vec![0xf0, 0x00];
    b.extend(btc_leaf(0));
    assert_eq!(refuse(b), "append argument is 0 bytes (minimum 1)");
    let mut b = vec![0xf0];
    write_varbytes(&mut b, &vec![1u8; 4097]);
    b.extend(btc_leaf(0));
    assert_eq!(refuse(b), "append argument is 4097 bytes (limit 4096)");
    // The same op twice in one node.
    let mut b = vec![0xff, 0x08];
    b.extend(btc_leaf(1));
    b.push(0x08);
    b.extend(btc_leaf(2));
    assert_eq!(refuse(b), "op sha256 repeated in one timestamp node");
    // hexlify of a 2049-byte message: append 2017 bytes to the 32-byte digest.
    let mut b = vec![0xf0];
    write_varbytes(&mut b, &vec![1u8; 2017]);
    b.push(0xf3);
    b.extend(btc_leaf(0));
    assert_eq!(
        refuse(b),
        "message invalid for op hexlify: hexlify over a 2049-byte message (limit 2048)"
    );
    // 2048 bytes are fine.
    let mut b = vec![0xf0];
    write_varbytes(&mut b, &vec![1u8; 2016]);
    b.push(0xf3);
    b.extend(btc_leaf(0));
    assert!(Proof::parse(&file_with(&d, &b)).is_ok());
}

#[test]
fn attestation_bounds() {
    let d = [7u8; 32];
    let refuse = |body: Vec<u8>| err_text(Proof::parse(&file_with(&d, &body)));
    // An 8193-byte payload.
    let mut b = vec![0x00];
    b.extend_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8]);
    write_varbytes(&mut b, &vec![0u8; 8193]);
    assert_eq!(refuse(b), "attestation payload is 8193 bytes (limit 8192)");
    // Trailing bytes inside a Bitcoin payload (the crate ignores the length,
    // attestation.rs:63-92).
    let mut b = vec![0x00];
    b.extend_from_slice(&TAG_BITCOIN);
    write_varbytes(&mut b, &[0x05, 0x00]);
    assert_eq!(refuse(b), "trailing bytes inside an attestation payload");
    // Height 2^32.
    let mut b = vec![0x00];
    b.extend_from_slice(&TAG_BITCOIN);
    let mut payload = Vec::new();
    write_varuint(&mut payload, 1u64 << 32);
    write_varbytes(&mut b, &payload);
    assert_eq!(
        refuse(b),
        "Bitcoin attestation height 4294967296 exceeds 4294967295"
    );
    // A URI with '?'.
    let pend = |uri: &[u8]| {
        let mut b = vec![0x00];
        b.extend_from_slice(&TAG_PENDING);
        let mut payload = Vec::new();
        write_varbytes(&mut payload, uri);
        write_varbytes(&mut b, &payload);
        b
    };
    assert_eq!(
        refuse(pend(b"https://a.example/?x")),
        "pending attestation URI contains a character outside A-Z a-z 0-9 . _ / : -"
    );
    assert_eq!(
        refuse(pend(&vec![b'a'; 1001])),
        "pending attestation URI is 1001 bytes (limit 1000)"
    );
    assert!(Proof::parse(&file_with(&d, &pend(&vec![b'a'; 1000]))).is_ok());
    // The same attestation twice in one node.
    let mut b = vec![0xff];
    b.extend(btc_leaf(5));
    b.extend(btc_leaf(5));
    assert_eq!(refuse(b), "attestation repeated in one timestamp node");
}

#[test]
fn recursion_limit() {
    // Timestamp.deserialize starts at 256 and refuses at 0: 255 nested ops
    // parse, the 256th is refused (the reference's exact bound).
    let chain = |n: usize| {
        let mut b = vec![0x08; n];
        b.extend(btc_leaf(0));
        file_with(&[1u8; 32], &b)
    };
    assert!(Proof::parse(&chain(255)).is_ok());
    assert_eq!(
        err_text(Proof::parse(&chain(256))),
        "timestamp nests deeper than the recursion limit of 256"
    );
}

#[test]
fn unknown_attestation_round_trips() {
    // A Litecoin attestation is kept verbatim and never read as Bitcoin.
    let mut b = vec![0x00];
    b.extend_from_slice(&hex::decode("06869a0d73d71b45").unwrap());
    write_varbytes(&mut b, &[0x81, 0x01]);
    let f = file_with(&[3u8; 32], &b);
    let p = Proof::parse(&f).unwrap();
    assert_eq!(p.to_bytes(), f);
    assert_eq!(p.state(), ProofState::NoKnownAttestation);
    assert_eq!(p.state().column(), None);
    assert!(matches!(
        p.leaves()[0].attestation,
        Attestation::Unknown { .. }
    ));
}

#[test]
fn canonical_order_is_python_order() {
    assert_eq!(
        attestation_cmp(
            &Attestation::Bitcoin { height: 1 },
            &Attestation::Pending { uri: String::new() }
        ),
        Ordering::Less
    );
    assert_eq!(
        attestation_cmp(
            &Attestation::Bitcoin { height: 1_000_000 },
            &Attestation::Pending { uri: String::new() }
        ),
        Ordering::Less
    );
    assert_eq!(
        op_cmp(&Op::Append(vec![1]), &Op::Append(vec![1, 0])),
        Ordering::Less
    );
    assert_eq!(op_cmp(&Op::Sha256, &Op::Append(vec![0])), Ordering::Less);
    assert_eq!(
        op_cmp(&Op::Prepend(vec![0]), &Op::Append(vec![9])),
        Ordering::Greater
    );
    assert_eq!(
        attestation_cmp(
            &Attestation::Pending {
                uri: "https://a".into()
            },
            &Attestation::Pending {
                uri: "https://b".into()
            }
        ),
        Ordering::Less
    );
    // Merging two nodes built in opposite orders serializes identically.
    let leaf = |a: Attestation| Node {
        msg: Vec::new(),
        attestations: vec![a],
        ops: Vec::new(),
    };
    let msg = vec![9u8; 32];
    let mut child_a = leaf(Attestation::Pending {
        uri: "https://a".into(),
    });
    child_a.msg = Op::Sha256.apply(&msg).unwrap();
    let mut child_b = leaf(Attestation::Bitcoin { height: 3 });
    child_b.msg = Op::Append(vec![1]).apply(&msg).unwrap();
    let one = Node {
        msg: msg.clone(),
        attestations: vec![Attestation::Pending {
            uri: "https://z".into(),
        }],
        ops: vec![(Op::Sha256, child_a.clone())],
    };
    let two = Node {
        msg: msg.clone(),
        attestations: vec![Attestation::Bitcoin { height: 9 }],
        ops: vec![(Op::Append(vec![1]), child_b.clone())],
    };
    let mut x = one.clone();
    merge(&mut x, two.clone());
    let mut y = two;
    merge(&mut y, one);
    assert_eq!(serialize_node(&x), serialize_node(&y));
    assert_eq!(x, y);
    // And it re-parses to the same tree.
    assert_eq!(parse_timestamp(&serialize_node(&x), &msg).unwrap(), x);
}

#[test]
fn commitment_and_compose() {
    let digest = h32(PENDING_DIGEST);
    let nonce: [u8; 16] = hex::decode(PENDING_NONCE).unwrap().try_into().unwrap();
    let c = commitment(&digest, &nonce);
    assert_eq!(hex::encode(c), PENDING_COMMITMENT);
    let response = parse_timestamp(&fx("response-bob.otsr"), &c).unwrap();
    let p = Proof::compose(&digest, &nonce, response.clone()).unwrap();
    assert_eq!(p.to_bytes(), fx("pending-bob.ots"));
    // A response over another message is refused. Any other nonce does it;
    // a fresh one differs from the fixture's with overwhelming probability.
    let wrong = random_nonce();
    assert_ne!(wrong, nonce);
    assert!(Proof::compose(&digest, &wrong, response).is_err());
}

#[test]
fn merge_responses_reproduces_pending_ots() {
    let mut p = Proof::parse(&fx("pending-bob.ots")).unwrap();
    let c = h32(PENDING_COMMITMENT);
    let alice = parse_timestamp(&fx("response-alice.otsr"), &c).unwrap();
    // The commitment node holds no pending leaf; merge there directly.
    let node = {
        let (_, blinded) = &mut p.root.ops[0];
        &mut blinded.ops[0].1
    };
    merge(node, alice);
    assert_eq!(p.to_bytes(), fx("pending.ots"));
}

#[test]
fn merge_upgrades_reproduces_bitcoin_ots() {
    let old = Proof::parse(&fx("bitcoin-stripped.ots")).unwrap();
    assert_eq!(old.state(), ProofState::Pending);
    let mut p = old.clone();
    for (uri, msg, file) in [
        (BOB, BOB_MSG_BITCOIN, "upgrade-bob.otsr"),
        (ALICE, ALICE_MSG_BITCOIN, "upgrade-alice.otsr"),
    ] {
        let msg = hex::decode(msg).unwrap();
        let up = parse_timestamp(&fx(file), &msg).unwrap();
        p.merge_at(&msg, uri, up).unwrap();
    }
    assert_eq!(p.to_bytes(), fx("bitcoin.ots"));
    assert!(p.contains(&old));
    assert!(!old.contains(&p));
}

#[test]
fn merge_at_refuses_unknown_message_or_uri() {
    let mut p = Proof::parse(&fx("bitcoin-stripped.ots")).unwrap();
    let msg = hex::decode(BOB_MSG_BITCOIN).unwrap();
    let up = parse_timestamp(&fx("upgrade-bob.otsr"), &msg).unwrap();
    let e = err_text(p.merge_at(&msg, ALICE, up.clone()));
    assert!(
        e.starts_with("no node of the proof holds a pending attestation for https://alice"),
        "{e}"
    );
    let other = hex::decode(ALICE_MSG_BITCOIN).unwrap();
    assert_eq!(
        err_text(p.merge_at(&other, BOB, up)),
        "upgrade is not over the pending message"
    );
    assert_eq!(p.to_bytes(), fx("bitcoin-stripped.ots"));
}

#[test]
fn genesis_header_pow_and_root() {
    let h = header(GENESIS_HEADER);
    assert!(h.pow_sane());
    assert_eq!(hex::encode(h.merkle_root()), GENESIS_ROOT);
    assert_eq!(h.block_hash_display(), GENESIS_HASH_DISPLAY);
    assert_eq!(h.bits(), 0x1d00ffff);
    let mut flipped = h;
    flipped.0[76] ^= 0x01;
    assert!(
        flipped.block_hash_display().starts_with("8cdc343d"),
        "{}",
        flipped.block_hash_display()
    );
    assert!(!flipped.pow_sane());
    let mut easy = h;
    easy.0[72..76].copy_from_slice(&0x207f_ffffu32.to_le_bytes());
    assert!(!easy.pow_sane(), "a regtest-easy target is above powLimit");
}

#[test]
fn compact_target_edges() {
    assert_eq!(compact_target(0x1d80_ffff), None, "negative");
    assert_eq!(compact_target(0x1d00_0000), None, "zero mantissa");
    assert_eq!(compact_target(0xff00_ffff), None, "overflow");
    assert_eq!(compact_target(0x0300_0000), None);
    let t = compact_target(0x1d00_ffff).unwrap();
    assert_eq!(
        hex::encode(t),
        "00000000ffff0000000000000000000000000000000000000000000000000000"
    );
    let small = compact_target(0x0212_3456).unwrap();
    assert_eq!(hex::encode(&small[28..]), "00001234");
}

fn headers_file(dir: &std::path::Path, text: &str) -> PathBuf {
    let p = dir.join("headers.txt");
    std::fs::write(&p, text).unwrap();
    p
}

fn genesis_headers() -> HeaderFile {
    load_headers(&fixture_dir().join("headers.txt")).unwrap()
}

#[test]
fn check_bitcoin_leaves_tiers() {
    let headers = genesis_headers();
    let g = Proof::parse(&fx("genesis.ots")).unwrap();
    assert_eq!(
        check_bitcoin_leaves(&g, Some(&headers)),
        vec![BitcoinCheck::Matches {
            height: 0,
            block_hash_display: GENESIS_HASH_DISPLAY.into()
        }]
    );
    assert_eq!(
        check_bitcoin_leaves(&g, None),
        vec![BitcoinCheck::NoHeader { height: 0 }]
    );
    let mut flipped = g.clone();
    flipped.digest[0] ^= 1;
    flipped.root.msg[0] ^= 1;
    match &check_bitcoin_leaves(&flipped, Some(&headers))[..] {
        [BitcoinCheck::Mismatch {
            height: 0,
            proof_display,
            header_display,
        }] => {
            assert_eq!(header_display, &display_hex(&h32(GENESIS_ROOT)));
            assert_ne!(proof_display, header_display);
        }
        other => panic!("{other:?}"),
    }
    let p = Proof::parse(&fx("pending.ots")).unwrap();
    let mut forty_four = p.clone();
    let node = pending_node(&p.root, BOB).unwrap().msg.clone();
    // A Bitcoin leaf over the 44-byte pending message cannot be a merkle root.
    fn add_btc(n: &mut Node, msg: &[u8]) {
        if n.msg == msg {
            n.attestations.push(Attestation::Bitcoin { height: 7 });
            n.sort();
            return;
        }
        for (_, c) in n.ops.iter_mut() {
            add_btc(c, msg);
        }
    }
    add_btc(&mut forty_four.root, &node);
    assert_eq!(
        check_bitcoin_leaves(&forty_four, Some(&headers)),
        vec![BitcoinCheck::Malformed { height: 7, len: 44 }]
    );
}

#[test]
fn load_headers_rules() {
    let dir = tempfile::tempdir().unwrap();
    let ok = format!(
        "# comment\n\n0 {}\n0 {}\n",
        GENESIS_HEADER,
        GENESIS_HEADER.to_uppercase()
    );
    let hf = load_headers(&headers_file(dir.path(), &ok)).unwrap();
    assert_eq!(hf.by_height.len(), 1);
    let path = headers_file(dir.path(), &format!("0 {}\n", &GENESIS_HEADER[..158]));
    assert_eq!(
        err_text(load_headers(&path)),
        format!(
            "bitcoin headers {}:1: expected \"<height> <160 hex characters>\"",
            path.display()
        )
    );
    let path = headers_file(dir.path(), &format!("4294967296 {GENESIS_HEADER}\n"));
    assert!(err_text(load_headers(&path)).ends_with(":1: height 4294967296 exceeds 4294967295"));
    // Same height, different bytes: refused before the PoW check runs.
    let mut other = header(GENESIS_HEADER);
    other.0[76] ^= 1;
    let path = headers_file(
        dir.path(),
        &format!("0 {GENESIS_HEADER}\n0 {}\n", hex::encode(other.0)),
    );
    assert!(
        err_text(load_headers(&path)).ends_with(":2: block 0 listed twice with different headers")
    );
    let path = headers_file(
        dir.path(),
        &format!("0 {GENESIS_HEADER}\n1 {GENESIS_HEADER}\n"),
    );
    assert_eq!(load_headers(&path).unwrap().by_height.len(), 2);
    let mut easy = header(GENESIS_HEADER);
    easy.0[72..76].copy_from_slice(&0x207f_ffffu32.to_le_bytes());
    let path = headers_file(dir.path(), &format!("3 {}\n", hex::encode(easy.0)));
    assert_eq!(
        err_text(load_headers(&path)),
        format!(
            "bitcoin headers {}:1: block 3: header fails the proof-of-work sanity check (hash above its own target, \
             or a target easier than Bitcoin's minimum difficulty); not a Bitcoin mainnet header",
            path.display()
        )
    );
}

#[test]
fn display_hex_reverses() {
    assert_eq!(display_hex(&[0x01, 0x02, 0xab]), "ab0201");
    assert_eq!(display_hex(&h32(ROOT_449397)), ROOT_449397_DISPLAY);
}

#[test]
fn random_nonce_differs() {
    assert_ne!(random_nonce(), random_nonce());
}

#[test]
fn request_and_upgrade_files_round_trip_and_deny_unknown_fields() {
    let r = OtsRequestFile {
        format: OTS_REQUEST_FORMAT.into(),
        subject_hash: PENDING_DIGEST.into(),
        nonce: PENDING_NONCE.into(),
        calendar: "bob".into(),
        calendar_url: BOB.into(),
    };
    let text = serde_json::to_string(&r).unwrap();
    assert!(!text.contains("subject\""), "no subject field: {text}");
    let back: OtsRequestFile = serde_json::from_str(&text).unwrap();
    assert_eq!(back, r);
    let (h, n) = back.decode().unwrap();
    assert_eq!(hex::encode(commitment(&h, &n)), PENDING_COMMITMENT);
    let extra = text.replace("{", "{\"subject\":\"chain_head\",");
    assert!(serde_json::from_str::<OtsRequestFile>(&extra)
        .unwrap_err()
        .to_string()
        .contains("unknown field `subject`"));
    let mut bad = r.clone();
    bad.format = "x".into();
    assert!(bad.decode().is_err());
    let u = OtsUpgradeFile {
        format: OTS_UPGRADE_FORMAT.into(),
        anchor_id: 7,
        uri: BOB.into(),
        message: BOB_MSG_BITCOIN.into(),
    };
    let text = serde_json::to_string(&u).unwrap();
    let back: OtsUpgradeFile = serde_json::from_str(&text).unwrap();
    assert_eq!(back, u);
    assert_eq!(
        back.decode().unwrap(),
        hex::decode(BOB_MSG_BITCOIN).unwrap()
    );
    let extra = text.replace("{", "{\"proof\":\"00\",");
    assert!(serde_json::from_str::<OtsUpgradeFile>(&extra).is_err());
}

// -------------------- 4.3 storage and the leg --------------------

fn bucket(start: u64) -> TimeBucket {
    TimeBucket {
        start_epoch_s: start,
        size_s: 600,
    }
}

fn mem() -> Connection {
    Connection::open_in_memory().unwrap()
}

fn master(conn: &Connection) -> Vec<String> {
    let mut stmt = conn
        .prepare(
            "SELECT type || ':' || name || ':' || ifnull(sql,'') FROM sqlite_master ORDER BY name",
        )
        .unwrap();
    stmt.query_map([], |r| r.get(0))
        .unwrap()
        .collect::<std::result::Result<_, _>>()
        .unwrap()
}

#[test]
fn ddl_literal_pinned_and_extra_columns_empty() {
    assert_eq!(
        OTS_ANCHORS_DDL,
        "CREATE TABLE IF NOT EXISTS ots_anchors (\n  id INTEGER PRIMARY KEY AUTOINCREMENT,\n  created_bucket_start INTEGER NOT NULL,\n  created_bucket_size INTEGER NOT NULL,\n  subject TEXT NOT NULL,\n  subject_hash BLOB NOT NULL,\n  calendar_url TEXT NOT NULL,\n  proof BLOB NOT NULL,\n  attestation TEXT NOT NULL,\n  bitcoin_height INTEGER,\n  upgraded_bucket_start INTEGER,\n  ledger_id INTEGER\n);"
    );
    assert!(OTS_EXTRA_COLUMNS.is_empty());
    let conn = mem();
    assert!(!ots_table_exists(&conn).unwrap());
    ensure_ots_table(&conn).unwrap();
    let once = master(&conn);
    ensure_ots_table(&conn).unwrap();
    assert_eq!(master(&conn), once);
    assert!(ots_table_exists(&conn).unwrap());
}

fn chain_db() -> Connection {
    let conn = mem();
    conn.execute_batch(
        "CREATE TABLE sealed_events (id INTEGER PRIMARY KEY AUTOINCREMENT, entry_hash BLOB NOT NULL);
         CREATE TABLE checkpoints (id INTEGER PRIMARY KEY AUTOINCREMENT, chain_head_hash BLOB NOT NULL);",
    )
    .unwrap();
    conn
}

#[test]
fn insert_list_round_trip_caches_derived_from_proof() {
    let conn = chain_db();
    let pend = Proof::parse(&fx("pending-bob.ots")).unwrap();
    let btc = Proof::parse(&fx("bitcoin.ots")).unwrap();
    // The bitcoin proof's digest is a chain head here: ledger_id recorded.
    conn.execute(
        "INSERT INTO sealed_events (entry_hash) VALUES (?1), (?2)",
        rusqlite::params![vec![0u8; 32], btc.digest.to_vec()],
    )
    .unwrap();
    let a = insert_ots_anchor(
        &conn,
        bucket(600),
        AnchorSubject::Digest,
        &pend.digest,
        BOB,
        &pend,
    )
    .unwrap();
    let b = insert_ots_anchor(
        &conn,
        bucket(1200),
        AnchorSubject::ChainHead,
        &btc.digest,
        ALICE,
        &btc,
    )
    .unwrap();
    let rows = list_ots_anchors(&conn).unwrap();
    assert_eq!(rows.len(), 2);
    assert_eq!(rows[0].id, a);
    assert_eq!(rows[0].attestation, "pending");
    assert_eq!(rows[0].bitcoin_height, None);
    assert_eq!(rows[0].upgraded_bucket_start, None);
    assert_eq!(rows[0].ledger_id, None);
    assert_eq!(rows[0].proof, fx("pending-bob.ots"));
    assert_eq!(rows[0].subject_kind(), Some(AnchorSubject::Digest));
    assert_eq!(rows[1].id, b);
    assert_eq!(rows[1].attestation, "bitcoin");
    assert_eq!(rows[1].bitcoin_height, Some(449397));
    assert_eq!(rows[1].upgraded_bucket_start, Some(1200));
    assert_eq!(rows[1].ledger_id, Some(2));
    assert_eq!(rows[1].created_bucket, bucket(1200));
    assert_eq!(pending_ots_anchors(&conn).unwrap().len(), 1);
    // A proof over another digest is refused.
    assert!(insert_ots_anchor(
        &conn,
        bucket(0),
        AnchorSubject::Digest,
        &[1u8; 32],
        BOB,
        &pend
    )
    .is_err());
}

#[test]
fn lenient_reader_reasons() {
    let conn = chain_db();
    let pend = Proof::parse(&fx("pending-bob.ots")).unwrap();
    let id = insert_ots_anchor(
        &conn,
        bucket(600),
        AnchorSubject::Digest,
        &pend.digest,
        BOB,
        &pend,
    )
    .unwrap();
    conn.execute("UPDATE ots_anchors SET proof = 'x' WHERE id = ?1", [id])
        .unwrap();
    let id2 = insert_ots_anchor(
        &conn,
        bucket(600),
        AnchorSubject::Digest,
        &pend.digest,
        BOB,
        &pend,
    )
    .unwrap();
    conn.execute(
        "UPDATE ots_anchors SET subject_hash = ?2 WHERE id = ?1",
        rusqlite::params![id2, vec![0u8; 31]],
    )
    .unwrap();
    let rows = list_ots_anchors_lenient(&conn).unwrap();
    assert_eq!(
        rows[0].1.as_ref().unwrap_err().to_string(),
        format!("corrupt ots anchor {id}: proof is not a BLOB")
    );
    assert_eq!(
        rows[1].1.as_ref().unwrap_err().to_string(),
        format!("corrupt ots anchor {id2}: subject_hash size")
    );
    assert!(list_ots_anchors(&conn).is_err());
}

#[test]
fn record_upgrade_once_and_refuses_non_superset() {
    let conn = chain_db();
    let old = Proof::parse(&fx("bitcoin-stripped.ots")).unwrap();
    let new = Proof::parse(&fx("bitcoin.ots")).unwrap();
    let id = insert_ots_anchor(
        &conn,
        bucket(600),
        AnchorSubject::Digest,
        &old.digest,
        BOB,
        &old,
    )
    .unwrap();
    // A "new" proof that drops evidence is refused.
    let mut dropped = new.clone();
    strip_below_pending(&mut dropped.root);
    dropped.root.ops.truncate(1);
    let e = err_text(record_ots_upgrade(
        &conn,
        id,
        bucket(1200),
        &old,
        &Proof::parse(&fx("genesis.ots")).unwrap(),
    ));
    assert_eq!(e, "upgraded proof covers a different digest; not stored");
    let mut lossy = new.clone();
    // Remove bob's pending leaf: no longer a superset.
    fn drop_pending(n: &mut Node, uri: &str) {
        n.attestations
            .retain(|a| !matches!(a, Attestation::Pending { uri: u } if u == uri));
        for (_, c) in n.ops.iter_mut() {
            drop_pending(c, uri);
        }
    }
    drop_pending(&mut lossy.root, BOB);
    assert_eq!(
        err_text(record_ots_upgrade(&conn, id, bucket(1200), &old, &lossy)),
        "upgraded proof does not contain the stored proof; not stored"
    );
    assert!(record_ots_upgrade(&conn, id, bucket(1200), &old, &new).unwrap());
    assert!(!record_ots_upgrade(&conn, id, bucket(1800), &old, &new).unwrap());
    let row = &list_ots_anchors(&conn).unwrap()[0];
    assert_eq!(row.attestation, "bitcoin");
    assert_eq!(row.bitcoin_height, Some(449397));
    assert_eq!(row.upgraded_bucket_start, Some(1200));
    assert_eq!(row.proof, fx("bitcoin.ots"));
}

#[test]
fn readers_never_create_the_table() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("w.db");
    Connection::open(&path)
        .unwrap()
        .execute_batch("CREATE TABLE sealed_events (id INTEGER PRIMARY KEY, entry_hash BLOB)")
        .unwrap();
    let ro =
        Connection::open_with_flags(&path, rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY).unwrap();
    let before = master(&ro);
    assert!(!ots_table_exists(&ro).unwrap());
    assert!(list_ots_anchors_lenient(&ro).unwrap().is_empty());
    assert!(list_ots_anchors(&ro).unwrap().is_empty());
    assert!(pending_ots_anchors(&ro).unwrap().is_empty());
    assert_eq!(master(&ro), before);
}

/// A calendar that answers the way a real one does: `append(16) sha256
/// prepend(4) append(8) → Pending{its url}` over the commitment, and serves
/// queued upgrade bodies.
struct FakeCalendar {
    received: RefCell<Vec<(String, [u8; 32])>>,
    fetched: RefCell<Vec<(String, Vec<u8>)>>,
    upgrades: RefCell<Vec<Option<Vec<u8>>>>,
    /// Calendar url ⇒ pending URI to answer with (pool behavior).
    answer_uri: Option<(String, String)>,
    fail_url: Option<String>,
}

impl FakeCalendar {
    fn new() -> Self {
        FakeCalendar {
            received: RefCell::new(Vec::new()),
            fetched: RefCell::new(Vec::new()),
            upgrades: RefCell::new(Vec::new()),
            answer_uri: None,
            fail_url: None,
        }
    }
}

fn calendar_body(commitment: &[u8; 32], uri: &str) -> Vec<u8> {
    let salt: [u8; 16] = commitment[..16].try_into().unwrap();
    let ops = [
        Op::Append(salt.to_vec()),
        Op::Sha256,
        Op::Prepend(vec![0x58, 0xa4, 0xc2, 0x57]),
        Op::Append(vec![0xa1, 0xa9, 0x2c, 0x61, 0xd5, 0x41, 0x72, 0x06]),
    ];
    let mut msgs = vec![commitment.to_vec()];
    for op in &ops {
        let next = op.apply(msgs.last().unwrap()).unwrap();
        msgs.push(next);
    }
    let mut node = Node {
        msg: msgs[4].clone(),
        attestations: vec![Attestation::Pending { uri: uri.into() }],
        ops: Vec::new(),
    };
    for i in (0..4).rev() {
        node = Node {
            msg: msgs[i].clone(),
            attestations: Vec::new(),
            ops: vec![(ops[i].clone(), node)],
        };
    }
    serialize_node(&node)
}

impl CalendarTransport for FakeCalendar {
    fn submit(&self, url: &str, c: &[u8; 32], _allow_http: bool) -> Result<Vec<u8>> {
        self.received.borrow_mut().push((url.to_string(), *c));
        if self.fail_url.as_deref() == Some(url) {
            bail!("connection refused");
        }
        let uri = match &self.answer_uri {
            Some((u, pool)) if u == url => pool.clone(),
            _ => url.to_string(),
        };
        Ok(calendar_body(c, &uri))
    }

    fn fetch(&self, uri: &str, msg: &[u8], _allow_http: bool) -> Result<Option<Vec<u8>>> {
        self.fetched
            .borrow_mut()
            .push((uri.to_string(), msg.to_vec()));
        let mut q = self.upgrades.borrow_mut();
        Ok(if q.is_empty() { None } else { q.remove(0) })
    }
}

fn two_calendars() -> OtsPolicy {
    serde_json::from_str(&format!(
        r#"{{"calendars":[{{"name":"alice","url":"{ALICE}"}},{{"name":"bob","url":"{BOB}"}}]}}"#
    ))
    .unwrap()
}

fn plan_all_heads(conn: &Connection) -> Vec<PlannedOts> {
    AnchorSubject::HEADS
        .iter()
        .map(|k| match tsa::ledger_head(conn, *k).unwrap() {
            Some(hash) => PlannedOts {
                stored_as: *k,
                hash,
                sentinel: false,
            },
            None => PlannedOts {
                stored_as: AnchorSubject::Digest,
                hash: k.empty_ledger_sentinel().unwrap(),
                sentinel: true,
            },
        })
        .collect()
}

#[test]
fn ots_leg_constant_count_fresh_nonces() {
    let conn = chain_db();
    conn.execute(
        "INSERT INTO sealed_events (entry_hash) VALUES (?1)",
        [vec![0x11u8; 32]],
    )
    .unwrap();
    let planned = plan_all_heads(&conn);
    assert_eq!(planned.len(), AnchorSubject::HEADS.len());
    assert!(planned.iter().filter(|p| p.sentinel).count() >= 1);
    let fake = FakeCalendar::new();
    let policy = two_calendars();
    let mut nonce = random_nonce;
    for run in 0..2u64 {
        let rep = run_ots_leg(
            &conn,
            &policy,
            &planned,
            &fake,
            bucket(600 * (run + 1)),
            true,
            &mut nonce,
        )
        .unwrap();
        assert_eq!(rep.submissions, planned.len() * 2);
        assert_eq!(rep.stored.len(), rep.submissions);
        assert!(rep.submit_failures.is_empty());
        // Every row is pending at its own calendar: nothing to fetch yet in
        // run 0; in run 1 the rows of run 0 are polled (and still pending).
        assert_eq!(rep.fetches, if run == 0 { 0 } else { planned.len() * 2 });
    }
    let received = fake.received.borrow();
    assert_eq!(received.len(), 2 * planned.len() * 2);
    let distinct: std::collections::HashSet<_> = received.iter().map(|(_, c)| *c).collect();
    assert_eq!(distinct.len(), received.len(), "every commitment is fresh");
    for p in &planned {
        assert!(
            !distinct.contains(&p.hash),
            "a calendar never sees a head or sentinel"
        );
    }
    assert_eq!(list_ots_anchors(&conn).unwrap().len(), received.len());
    // Each stored proof is a valid nonced commitment over its subject.
    for row in list_ots_anchors(&conn).unwrap() {
        let p = Proof::parse(&row.proof).unwrap();
        assert_eq!(p.digest, row.subject_hash);
        assert_eq!(pollable_leaf(&p, &row.calendar_url).unwrap().1.len(), 44);
    }
}

const BASE: u64 = 10 * 86_400;

#[test]
fn ots_leg_upgrade_bound_and_order() {
    let conn = chain_db();
    let stripped = Proof::parse(&fx("bitcoin-stripped.ots")).unwrap();
    let seeded = insert_ots_anchor(
        &conn,
        bucket(BASE),
        AnchorSubject::Digest,
        &stripped.digest,
        BOB,
        &stripped,
    )
    .unwrap();
    let old = insert_ots_anchor(
        &conn,
        bucket(BASE),
        AnchorSubject::Digest,
        &stripped.digest,
        BOB,
        &stripped,
    )
    .unwrap();
    let undeclared = insert_ots_anchor(
        &conn,
        bucket(BASE),
        AnchorSubject::Digest,
        &stripped.digest,
        "https://carol.example",
        &stripped,
    )
    .unwrap();
    let now = BASE + 6 * 86_400;
    // Moved 8 days back from now: beyond the horizon.
    conn.execute(
        "UPDATE ots_anchors SET created_bucket_start = ?2 WHERE id = ?1",
        rusqlite::params![old, (now - 8 * 86_400) as i64],
    )
    .unwrap();
    let planned = vec![PlannedOts {
        stored_as: AnchorSubject::Digest,
        hash: [5u8; 32],
        sentinel: false,
    }];
    let fake = FakeCalendar::new();
    fake.upgrades.borrow_mut().push(None);
    fake.upgrades
        .borrow_mut()
        .push(Some(fx("upgrade-bob.otsr")));
    let policy = two_calendars();
    let mut nonce = random_nonce;
    let rep = run_ots_leg(
        &conn,
        &policy,
        &planned,
        &fake,
        bucket(now),
        true,
        &mut nonce,
    )
    .unwrap();
    assert_eq!(rep.fetches, 1, "one GET: the seeded row only");
    assert_eq!(rep.still_pending, vec![(seeded, BOB.to_string())]);
    assert_eq!(rep.beyond_horizon, vec![old]);
    assert_eq!(rep.not_polled_undeclared.len(), 1);
    assert_eq!(rep.not_polled_undeclared[0].0, undeclared);
    assert_eq!(
        fake.fetched.borrow()[0],
        (BOB.to_string(), hex::decode(BOB_MSG_BITCOIN).unwrap())
    );
    // Upgrades come before stamps in the event log.
    let first_stamp = rep
        .events
        .iter()
        .position(|e| matches!(e, OtsLegEvent::Stamping { .. }))
        .unwrap();
    assert!(rep.events[..first_stamp]
        .iter()
        .all(|e| !matches!(e, OtsLegEvent::Stamping { .. } | OtsLegEvent::Stored(_))));
    // Run 2: the rows stamped in run 1 are pending at alice/bob too; the
    // seeded row gets the real upgrade body.
    let rep = run_ots_leg(
        &conn,
        &policy,
        &planned,
        &fake,
        bucket(now + 600),
        true,
        &mut nonce,
    )
    .unwrap();
    assert_eq!(rep.upgraded, vec![(seeded, "digest".to_string(), 449399)]);
    assert_eq!(
        rep.fetches,
        1 + 2,
        "seeded row + the two rows stamped in run 1"
    );
    let row = list_ots_anchors(&conn)
        .unwrap()
        .into_iter()
        .find(|r| r.id == seeded)
        .unwrap();
    assert_eq!(row.attestation, "bitcoin");
    assert_eq!(row.bitcoin_height, Some(449399));
    assert_eq!(row.upgraded_bucket_start, Some((now + 600) as i64));
}

#[test]
fn ots_leg_refuses_pool_style_response() {
    let conn = chain_db();
    let planned = vec![PlannedOts {
        stored_as: AnchorSubject::Digest,
        hash: [5u8; 32],
        sentinel: false,
    }];
    let mut fake = FakeCalendar::new();
    fake.answer_uri = Some((
        ALICE.to_string(),
        "https://a.pool.opentimestamps.org".to_string(),
    ));
    let policy = two_calendars();
    let mut nonce = random_nonce;
    let rep = run_ots_leg(
        &conn,
        &policy,
        &planned,
        &fake,
        bucket(600),
        false,
        &mut nonce,
    )
    .unwrap();
    assert_eq!(rep.submit_failures.len(), 1);
    assert_eq!(
        rep.submit_failures[0].error,
        format!("calendar alice answered with no pending attestation for {ALICE} and no Bitcoin attestation (declare calendar URLs, not aggregator pools)")
    );
    assert_eq!(rep.stored.len(), 1, "bob was still asked and stored");
    // A transport error is a failure and the other calendar is still asked.
    let mut fake = FakeCalendar::new();
    fake.fail_url = Some(ALICE.to_string());
    let rep = run_ots_leg(
        &conn,
        &policy,
        &planned,
        &fake,
        bucket(1200),
        false,
        &mut nonce,
    )
    .unwrap();
    assert_eq!(rep.submit_failures.len(), 1);
    assert_eq!(rep.submit_failures[0].error, "connection refused");
    assert_eq!(fake.received.borrow().len(), 2);
    assert_eq!(rep.stored.len(), 1);
    assert_eq!(rep.fetches, 0, "request --policy never upgrades");
}

/// `00 ‖ Pending{uri}`: a pending leaf's bytes.
fn pending_leaf_bytes(uri: &str) -> Vec<u8> {
    let mut out = vec![0x00];
    write_attestation(&mut out, &Attestation::Pending { uri: uri.into() });
    out
}

/// A calendar whose `POST /digest` body nests `depth` sha256 ops above its
/// pending leaf: within the recursion limit on its own, and `compose` adds
/// two more levels above it.
struct DeepCalendar {
    depth: usize,
}

impl CalendarTransport for DeepCalendar {
    fn submit(&self, url: &str, _c: &[u8; 32], _allow_http: bool) -> Result<Vec<u8>> {
        let mut body = vec![0x08; self.depth];
        body.extend(pending_leaf_bytes(url));
        Ok(body)
    }

    fn fetch(&self, _uri: &str, _msg: &[u8], _allow_http: bool) -> Result<Option<Vec<u8>>> {
        bail!("not polled in this test")
    }
}

#[test]
fn deep_stamp_response_refused_before_storing() {
    let conn = mem();
    let policy: OtsPolicy = serde_json::from_str(&format!(
        r#"{{"calendars":[{{"name":"bob","url":"{BOB}"}}]}}"#
    ))
    .unwrap();
    let planned = vec![PlannedOts {
        stored_as: AnchorSubject::Digest,
        hash: [5u8; 32],
        sentinel: false,
    }];
    let mut nonce = random_nonce;
    // 254 nested ops parse as a body (the limit allows 255), but the stored
    // file would nest 256 deep from the digest: refused, nothing stored.
    let rep = run_ots_leg(
        &conn,
        &policy,
        &planned,
        &DeepCalendar { depth: 254 },
        bucket(600),
        false,
        &mut nonce,
    )
    .unwrap();
    assert_eq!(rep.submissions, 1);
    assert!(rep.stored.is_empty());
    assert_eq!(rep.submit_failures.len(), 1);
    assert_eq!(
        rep.submit_failures[0].error,
        "proof would not read back (timestamp nests deeper than the recursion limit of 256); \
         not stored"
    );
    assert!(
        !ots_table_exists(&conn).unwrap(),
        "refused before the table is created"
    );
    // One level shallower is exactly the deepest file the reader accepts.
    let rep = run_ots_leg(
        &conn,
        &policy,
        &planned,
        &DeepCalendar { depth: 253 },
        bucket(600),
        false,
        &mut nonce,
    )
    .unwrap();
    assert!(rep.submit_failures.is_empty(), "{:?}", rep.submit_failures);
    let rows = list_ots_anchors(&conn).unwrap();
    assert_eq!(rows.len(), 1);
    Proof::parse(&rows[0].proof).unwrap();
}

#[test]
fn deep_upgrade_body_leaves_the_pending_row_unchanged() {
    let conn = chain_db();
    let pending = Proof::parse(&fx("pending-bob.ots")).unwrap();
    let id = insert_ots_anchor(
        &conn,
        bucket(BASE),
        AnchorSubject::Digest,
        &pending.digest,
        BOB,
        &pending,
    )
    .unwrap();
    let before = list_ots_anchors(&conn).unwrap().remove(0);
    // bob's pending node sits 6 ops below the digest. A body of 250 nested
    // sha256 ops ending in a Bitcoin leaf over a 32-byte message parses on
    // its own and passes refuse_malformed, but merged it nests 256 deep.
    let deep = |n: usize| {
        let mut b = vec![0x08; n];
        b.extend(btc_leaf(1));
        b
    };
    let fake = FakeCalendar::new();
    fake.upgrades.borrow_mut().push(Some(deep(250)));
    let policy = two_calendars();
    let mut nonce = random_nonce;
    let rep = run_ots_leg(&conn, &policy, &[], &fake, bucket(BASE), true, &mut nonce).unwrap();
    assert_eq!(rep.fetches, 1);
    assert!(rep.upgraded.is_empty());
    assert_eq!(
        rep.upgrade_errors,
        vec![(
            id,
            BOB.to_string(),
            "proof would not read back (timestamp nests deeper than the recursion limit of \
             256); not stored"
                .to_string()
        )]
    );
    let after = list_ots_anchors(&conn).unwrap().remove(0);
    assert_eq!(after.proof, before.proof, "the pending proof is kept");
    assert_eq!(after.attestation, "pending");
    assert_eq!(after.bitcoin_height, None);
    assert_eq!(after.upgraded_bucket_start, None);
    // One level shallower merges to the deepest readable file and upgrades.
    fake.upgrades.borrow_mut().push(Some(deep(249)));
    let rep = run_ots_leg(
        &conn,
        &policy,
        &[],
        &fake,
        bucket(BASE + 600),
        true,
        &mut nonce,
    )
    .unwrap();
    assert!(rep.upgrade_errors.is_empty(), "{:?}", rep.upgrade_errors);
    assert_eq!(rep.upgraded, vec![(id, "digest".to_string(), 1)]);
    let row = list_ots_anchors(&conn).unwrap().remove(0);
    assert!(Proof::parse(&row.proof).unwrap().contains(&pending));
}

#[test]
fn upgrade_horizon_is_two_sided() {
    let now = bucket(BASE);
    let at = |s: u64| bucket(s);
    assert!(within_upgrade_horizon(now, at(BASE)));
    assert!(within_upgrade_horizon(
        now,
        at(BASE - OTS_UPGRADE_HORIZON_S)
    ));
    assert!(within_upgrade_horizon(
        now,
        at(BASE + OTS_UPGRADE_HORIZON_S)
    ));
    assert!(!within_upgrade_horizon(
        now,
        at(BASE - OTS_UPGRADE_HORIZON_S - 600)
    ));
    assert!(!within_upgrade_horizon(
        now,
        at(BASE + OTS_UPGRADE_HORIZON_S + 600)
    ));

    // In the leg: a row dated 8 days after `now` and one 8 days before it are
    // both beyond the horizon and never fetched; a 6-day skew either way is
    // still polled.
    let conn = chain_db();
    let stripped = Proof::parse(&fx("bitcoin-stripped.ots")).unwrap();
    let row = |start: u64| {
        insert_ots_anchor(
            &conn,
            bucket(start),
            AnchorSubject::Digest,
            &stripped.digest,
            BOB,
            &stripped,
        )
        .unwrap()
    };
    let now_s = 100 * 86_400;
    let future = row(now_s + 8 * 86_400);
    let past = row(now_s - 8 * 86_400);
    let ahead = row(now_s + 6 * 86_400);
    let behind = row(now_s - 6 * 86_400);
    let fake = FakeCalendar::new();
    let policy = two_calendars();
    let mut nonce = random_nonce;
    let rep = run_ots_leg(&conn, &policy, &[], &fake, bucket(now_s), true, &mut nonce).unwrap();
    assert_eq!(rep.beyond_horizon, vec![future, past]);
    assert_eq!(rep.fetches, 2);
    assert_eq!(
        rep.still_pending,
        vec![(ahead, BOB.to_string()), (behind, BOB.to_string())]
    );
    assert_eq!(fake.fetched.borrow().len(), 2);
}
