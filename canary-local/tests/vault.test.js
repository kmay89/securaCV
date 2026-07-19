// canary-local/tests/vault.test.js — the Vault page's honesty gate.
//
// Two jobs, like tests/wap.test.js:
//  1. Cross-check devices/vault.json against its sources (src/vault,
//     src/break_glass, the firmware seal, spec/invariants.md) so a hand-edit
//     or code drift is caught here — every constant, algorithm, signing domain
//     and invariant the page shows must still exist in the source it claims.
//  2. Exercise the DOM-free cores the page ships (vault-ui.js) — including a
//     REAL Ed25519 approval round-trip through Node's WebCrypto — so the demo's
//     "this is the kernel's actual quorum math" claim can't rot.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const { createHash, webcrypto } = require("node:crypto");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const read = (p) => readFileSync(p, "utf8");

const data = JSON.parse(read(join(ROOT, "devices/vault.json")));
const coreRs = read(join(REPO, "src/break_glass/core.rs"));
const sigsRs = read(join(REPO, "src/crypto/signatures.rs"));
const vcryptoRs = read(join(REPO, "src/vault/crypto.rs"));
const vformatRs = read(join(REPO, "src/vault/format.rs"));
const witnessdRs = read(join(REPO, "src/bin/witnessd.rs"));
const invariants = read(join(REPO, "spec/invariants.md"));
const FW = join(REPO, "firmware/projects/canary-wap/arduino/canary_wap");
const vaultLogicH = read(join(FW, "vault_logic.h"));
const vaultSnapCpp = read(join(FW, "vault_snapshot.cpp"));
const doc = read(join(REPO, "docs/sealed_snapshot_vault.md"));

// ── 1. shape + sanity floors ───────────────────────────────────────────────
test("vault.json has every section the page needs", () => {
  for (const k of ["concepts", "device_seal", "kernel_vault", "quorum", "invariants", "demo", "terminal", "docs"])
    assert.ok(data[k], "missing section: " + k);
  assert.strictEqual(data.concepts.length, 3);
  assert.strictEqual(data.device_seal.svlt_header.length, 9);
  assert.strictEqual(data.invariants.length, 2);
  assert.ok(data.quorum.guardrails.length >= 5, "quorum guardrails too thin");
});

// ── 2. quorum constants + grant rule trace to src/break_glass/core.rs ──────
test("quorum bounds + the grant rule are the kernel's own", () => {
  assert.ok(coreRs.includes("MAX_TRUSTEES: usize = " + data.quorum.max_trustees));
  assert.ok(coreRs.includes("MAX_APPROVALS: usize = " + data.quorum.max_approvals));
  assert.ok(coreRs.includes("trustees_used.len() >= policy.n"), "grant rule drifted");
  assert.ok(coreRs.includes("pub n: u8") && coreRs.includes("pub m: u8"), "QuorumPolicy{n,m} drifted");
});

// ── 3. the three signing domains trace to src/crypto/signatures.rs ─────────
test("the break-glass signing domains are the kernel's own", () => {
  assert.ok(sigsRs.includes('DOMAIN_TRUSTEE_APPROVAL: &str = "' + data.quorum.domains.approval + '"'));
  assert.ok(sigsRs.includes('DOMAIN_BREAK_GLASS_TOKEN: &str = "' + data.quorum.domains.token + '"'));
  assert.ok(sigsRs.includes('DOMAIN_BREAK_GLASS_RECEIPT: &str = "' + data.quorum.domains.receipt + '"'));
  // the interactive demo signs under the real approval domain
  assert.strictEqual(data.demo.domain, data.quorum.domains.approval);
});

// ── 4. the kernel vault envelope traces to src/vault ───────────────────────
test("kernel vault AEAD + magic are real", () => {
  assert.ok(vcryptoRs.includes('AEAD_ALG_CHACHA20POLY1305: &str = "' + data.kernel_vault.aead + '"'));
  assert.ok(vformatRs.includes('b"' + data.kernel_vault.magic + '"'), "VLT2 magic drifted");
  assert.ok(witnessdRs.includes("seal_latest_frame") && witnessdRs.includes("vault.seal_frame"),
    "vault not wired into witnessd");
});

// ── 5. the device seal traces to the firmware + design doc ─────────────────
test("device .svlt seal facts are the firmware's own", () => {
  assert.ok(vaultSnapCpp.includes('"' + data.device_seal.info_string + '"'), "HKDF info string drifted");
  assert.ok(vaultLogicH.includes('magic "' + data.device_seal.magic + '"'), "SVLT magic drifted");
  assert.ok(vaultLogicH.includes("KEEP_FILES      = " + data.device_seal.storage.keep_files));
  for (const d of ["SKIP_NO_KEY", "SKIP_DISABLED", "CAPTURE"])
    assert.ok(vaultLogicH.includes(d), "decision " + d + " drifted");
  for (const anchor of ["ChaCha20-Poly1305", "X25519", "write-only escrow"])
    assert.ok(doc.includes(anchor), "seal doc anchor drifted: " + anchor);
});

// ── 6. the invariants trace to spec/invariants.md ──────────────────────────
test("Invariants I and V are the spec's own", () => {
  const ids = data.invariants.map((i) => i.id).sort();
  assert.deepStrictEqual(ids, ["I", "V"]);
  assert.ok(invariants.includes("Invariant I — No Raw Export by Design"));
  assert.ok(invariants.includes("Invariant V — Break-Glass by Quorum"));
  assert.ok(invariants.includes("quorum-based authorization"));
});

// ── 7. DOM-free cores (vault-ui.js) ────────────────────────────────────────
test("le32 is little-endian", async () => {
  const { le32 } = await import("../assets/vault-ui.js");
  assert.deepStrictEqual([...le32(1)], [1, 0, 0, 0]);
  assert.deepStrictEqual([...le32(258)], [2, 1, 0, 0]);
});

test("countDistinctApprovals dedups on key and ignores invalid", async () => {
  const { countDistinctApprovals, quorumOutcome } = await import("../assets/vault-ui.js");
  const approvals = [
    { keyHex: "aa", valid: true }, { keyHex: "aa", valid: true }, // same key twice → 1
    { keyHex: "bb", valid: true }, { keyHex: "cc", valid: false }, // invalid ignored
  ];
  assert.strictEqual(countDistinctApprovals(approvals), 2);
  assert.strictEqual(quorumOutcome(2, 2), "granted");
  assert.strictEqual(quorumOutcome(1, 2), "denied");
});

test("domainSepHash matches the kernel's SHA-256(le32(len)||domain||hash)", async () => {
  const { domainSepHash } = await import("../assets/vault-ui.js");
  const domain = data.quorum.domains.approval;
  const reqHash = new Uint8Array(32).fill(7);
  const got = await domainSepHash(domain, reqHash);
  // independent reference (node:crypto), byte-for-byte
  const len = Buffer.from([domain.length & 255, (domain.length >>> 8) & 255, (domain.length >>> 16) & 255, (domain.length >>> 24) & 255]);
  const expect = createHash("sha256").update(len).update(Buffer.from(domain, "utf8")).update(Buffer.from(reqHash)).digest();
  assert.strictEqual(Buffer.from(got).toString("hex"), expect.toString("hex"));
});

test("a REAL Ed25519 approval verifies and counts — the demo isn't theater", async () => {
  const { domainSepHash, countDistinctApprovals, hex } = await import("../assets/vault-ui.js");
  const subtle = webcrypto.subtle;
  const domain = data.quorum.domains.approval;
  const reqHash = new Uint8Array(await subtle.digest("SHA-256", new TextEncoder().encode("cam-porch-0007|purpose")));
  const msg = await domainSepHash(domain, reqHash);

  const kp = await subtle.generateKey({ name: "Ed25519" }, true, ["sign", "verify"]);
  const sig = new Uint8Array(await subtle.sign({ name: "Ed25519" }, kp.privateKey, msg));
  const pub = new Uint8Array(await subtle.exportKey("raw", kp.publicKey));
  const ok = await subtle.verify({ name: "Ed25519" }, kp.publicKey, sig, msg);
  assert.strictEqual(ok, true, "a genuine approval must verify");

  // one trustee, signed twice → still one distinct approval (can't fill two slots)
  const approvals = [{ keyHex: hex(pub), valid: ok }, { keyHex: hex(pub), valid: ok }];
  assert.strictEqual(countDistinctApprovals(approvals), 1);
  // tampering the message breaks verification (tamper-evidence)
  const bad = await subtle.verify({ name: "Ed25519" }, kp.publicKey, sig, new Uint8Array(32));
  assert.strictEqual(bad, false, "a signature over a different request must not verify");
});
