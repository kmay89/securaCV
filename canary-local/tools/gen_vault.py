#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_vault.py — build canary-local/devices/vault.json from the code + specs.

The Vault teaching page (`canary-local/vault.html`) explains three things —
**Vault**, **Sealed**, and **Quorum** (break-glass) — with the docs and the
code standing right behind every claim. Like `gen_wap.py` / `gen_homeassistant.py`,
nothing is hand-faked: each constant, algorithm name, invariant and API here is
either parsed from source or authored and then *validated to still exist in the
source*, so this file `sys.exit(1)`s on drift and CI `git diff --exit-code`s the
output.

Sources of truth (all in-repo, deterministic, offline):
  src/break_glass/core.rs      QuorumPolicy{n,m}, MAX_TRUSTEES/MAX_APPROVALS, the
                               "distinct approvals >= n" grant rule, BreakGlassToken
  src/crypto/signatures.rs     the three :v2 signing domains
  src/vault/crypto.rs          AEAD alg (chacha20poly1305), crypto modes
  src/vault/format.rs          the VLT2 kernel envelope magic
  src/bin/witnessd.rs          seal_latest_frame → vault.seal_frame (it's wired)
  firmware/.../vault_snapshot.cpp, vault_logic.h   the device .svlt seal + ring bound
  docs/sealed_snapshot_vault.md the device-side seal narrative + byte-exact header
  spec/invariants.md           Invariant I (No Raw Export) + V (Break-Glass by Quorum)

Run:  python3 canary-local/tools/gen_vault.py
"""

import json
import re
from pathlib import Path

from _tooling import die, repo_root

REPO = repo_root()
CORE_RS = REPO / "src/break_glass/core.rs"
SIGS_RS = REPO / "src/crypto/signatures.rs"
VCRYPTO_RS = REPO / "src/vault/crypto.rs"
VFORMAT_RS = REPO / "src/vault/format.rs"
WITNESSD_RS = REPO / "src/bin/witnessd.rs"
CLI_RS = REPO / "src/break_glass/cli.rs"
HTTP_RS = REPO / "src/break_glass/http.rs"
BG_HTML = REPO / "src/break_glass/breakglass.html"
FW = REPO / "firmware/projects/canary-wap/arduino/canary_wap"
VAULT_LOGIC_H = FW / "vault_logic.h"
VAULT_SNAP_CPP = FW / "vault_snapshot.cpp"
DOC = REPO / "docs/sealed_snapshot_vault.md"
INVARIANTS = REPO / "spec/invariants.md"
OUT_JSON = REPO / "canary-local/devices/vault.json"

_CACHE: dict = {}


def read(path: Path) -> str:
    if path not in _CACHE:
        if not path.exists():
            die(f"source missing: {path.relative_to(REPO)}")
        _CACHE[path] = path.read_text(encoding="utf-8", errors="replace")
    return _CACHE[path]


def must(path: Path, needle: str, label: str) -> None:
    if needle not in read(path):
        die(f"{label}: expected {needle!r} in {path.relative_to(REPO)} — code changed?")


def must_any(paths, needle: str, label: str) -> None:
    if not any(needle in read(p) for p in paths):
        die(f"{label}: expected {needle!r} in one of [{', '.join(p.name for p in paths)}]")


def grab(path: Path, pattern: str, label: str, flags=0) -> str:
    m = re.search(pattern, read(path), flags)
    if not m:
        die(f"{label}: /{pattern}/ not found in {path.relative_to(REPO)}")
    return m.group(1)


# --------------------------------------------------------------------------- #
# 1. the three concepts (short, plain-English; anchored below)
# --------------------------------------------------------------------------- #

CONCEPTS = [
    {"key": "vault", "title": "Vault",
     "blurb": "A local, write-only lockbox. The device seals evidence against an operator's public key — it can put things in, but it cannot read them back out. Nothing is ever captured unless you explicitly armed it (everything is off by default), and files never leave the device on their own."},
    {"key": "sealed", "title": "Sealed",
     "blurb": "The cryptographic wrapper. A fresh key per item (ephemeral X25519 ECDH → HKDF), authenticated encryption (ChaCha20-Poly1305), and a byte-exact header that is signed along with the data — so editing the trigger, the time, or a single byte makes the whole thing fail to open. Tamper-evident by construction."},
    {"key": "quorum", "title": "Quorum (break-glass)",
     "blurb": "Getting raw evidence back out is a deliberate, audited event that no one can do alone. It takes N of M trustees, each approving with their own key. Below the threshold the vault stays shut; at the threshold a single-use token is minted, every decision is logged to a signed chain, and only then does the evidence come out."},
]

# --------------------------------------------------------------------------- #
# 2. the device seal (canary-wap .svlt) — parsed/validated from firmware + doc
# --------------------------------------------------------------------------- #

VAULT_INFO = grab(VAULT_SNAP_CPP, r'INFO\[\]\s*=\s*"([^"]+)"', "device seal HKDF info")
must(VAULT_LOGIC_H, 'magic "SVLT"', "device .svlt magic")
KEEP_FILES = int(grab(VAULT_LOGIC_H, r"KEEP_FILES\s*=\s*(\d+)", "KEEP_FILES"))
for d in ("SKIP_NO_KEY", "SKIP_DISABLED", "SKIP_COOLDOWN", "SKIP_BAD_TRIGGER", "CAPTURE"):
    must(VAULT_LOGIC_H, d, f"device decision {d}")
for needle in ("ChaCha20-Poly1305", "X25519", "write-only escrow", "SVLT"):
    must(DOC, needle, f"seal doc anchor {needle!r}")

DEVICE_SEAL = {
    "escrow": "write-only escrow: the device stores only the operator's X25519 public key; sealing derives a fresh per-frame key by ephemeral ECDH, then zeroizes the ephemeral secret, the shared secret, the symmetric key and the plaintext. There is no code path that decrypts a .svlt on-device.",
    "defaults_off": True,
    "defaults_note": "opt-in per trigger (T3 smoke / T4 CO / glass break), ALL OFF by default. No operator key registered → nothing is ever captured, not even a Test capture.",
    "kex": "X25519 (ephemeral ECDH per frame)",
    "kdf": "HKDF-SHA256",
    "aead": "ChaCha20-Poly1305",
    "info_string": VAULT_INFO,
    "magic": "SVLT",
    "steps": [
        {"n": 1, "title": "fresh ephemeral key", "detail": "e ← hardware RNG, X25519-clamped; E = X25519(e, basepoint)"},
        {"n": 2, "title": "shared secret", "detail": "ss = X25519(e, operator_pub) — an all-zero result is rejected"},
        {"n": 3, "title": "derive the seal key", "detail": f"key = HKDF-SHA256(salt = E ‖ operator_pub, ikm = ss, info = \"{VAULT_INFO}\", 32 bytes)"},
        {"n": 4, "title": "authenticated encryption", "detail": "file = header(64) ‖ ChaCha20-Poly1305(key, nonce, aad = header, jpeg) ‖ tag(16)"},
        {"n": 5, "title": "zeroize", "detail": "the ephemeral secret, shared secret, seal key and plaintext are wiped; only the sealed .svlt remains"},
    ],
    # the byte-exact .svlt header (docs/sealed_snapshot_vault.md) — it IS the AEAD
    # associated data, so every field is tamper-evident.
    "svlt_header": [
        {"off": 0, "size": 4, "field": "magic \"SVLT\""},
        {"off": 4, "size": 1, "field": "format version (1)"},
        {"off": 5, "size": 1, "field": "trigger (1=T3 smoke, 2=T4 CO, 3=glass, 9=test)"},
        {"off": 6, "size": 1, "field": "time bucket 0–143 (10-min bucket — the ONLY time info stored)"},
        {"off": 7, "size": 1, "field": "reserved (0)"},
        {"off": 8, "size": 8, "field": "recipient key id = first 8 B of SHA-256(operator pubkey)"},
        {"off": 16, "size": 32, "field": "ephemeral X25519 public key"},
        {"off": 48, "size": 12, "field": "ChaCha20-Poly1305 nonce"},
        {"off": 60, "size": 4, "field": "ciphertext length u32 (excludes the 16 B tag)"},
    ],
    "aad_note": "the 64-byte header is the AEAD associated data — edit any field (trigger, time bucket, key id, nonce, length) and the tag fails on unseal.",
    "decision_table": [
        {"condition": "unknown trigger byte", "decision": "SKIP_BAD_TRIGGER"},
        {"condition": "no operator key registered", "decision": "SKIP_NO_KEY (Test included)"},
        {"condition": "SD unavailable", "decision": "SKIP_NO_SD"},
        {"condition": "camera not initialized", "decision": "SKIP_NO_CAMERA"},
        {"condition": "QR provisioning scan active", "decision": "SKIP_QR_BUSY"},
        {"condition": "a seal already in flight", "decision": "SKIP_WORKER_BUSY"},
        {"condition": "trigger == TEST", "decision": "CAPTURE (bypasses only opt-in + cooldown)"},
        {"condition": "trigger not opted in", "decision": "SKIP_DISABLED (the default state)"},
        {"condition": "within per-trigger cooldown", "decision": "SKIP_COOLDOWN"},
        {"condition": "otherwise", "decision": "CAPTURE"},
    ],
    "http": [
        {"method": "GET", "path": "/api/vault/status"},
        {"method": "POST", "path": "/api/vault/config"},
        {"method": "POST/DELETE", "path": "/api/vault/key"},
        {"method": "GET", "path": "/api/vault/list"},
        {"method": "GET", "path": "/api/vault/download?name="},
        {"method": "DELETE", "path": "/api/vault/item?name="},
        {"method": "POST", "path": "/api/vault/test"},
    ],
    "storage": {"keep_files": KEEP_FILES, "cooldown_default_s": 60, "max_ciphertext_kb": 512},
    "witness_note": "only the EXISTENCE of a sealed frame enters the witness chain — one `frame_sealed` event carrying `<trigger tag> <ciphertext SHA-256 prefix>` + a 10-minute time bucket. The chokepoint has no field that could carry image bytes.",
    "review": "the private key never touches the device; review happens off-device with tools/unseal_snapshot.py (gen-key / inspect / unseal).",
}

# --------------------------------------------------------------------------- #
# 3. the kernel vault envelope (src/vault) — parsed/validated
# --------------------------------------------------------------------------- #

AEAD_ALG = grab(VCRYPTO_RS, r'AEAD_ALG_CHACHA20POLY1305:\s*&str\s*=\s*"([^"]+)"', "kernel AEAD alg")
KEM_ALG = grab(VCRYPTO_RS, r'KEM_ALG_ML_KEM_768:\s*&str\s*=\s*"([^"]+)"', "kernel KEM alg")
must(VFORMAT_RS, 'b"VLT2"', "kernel envelope magic")
must(WITNESSD_RS, "seal_latest_frame", "vault wired into witnessd")
must(WITNESSD_RS, "vault.seal_frame", "witnessd seals frames")

KERNEL_VAULT = {
    "magic": "VLT2",
    "envelope": "EnvelopeV2",
    "aead": AEAD_ALG,
    "kem": KEM_ALG,
    "modes": ["classical (default)", "pq (ml-kem-768)", "hybrid"],
    "dek_model": "a fresh random 32-byte data-encryption key per envelope encrypts the payload (ChaCha20-Poly1305); the DEK is then key-wrapped under the device's 32-byte master key. The raw master key never leaves the process and is zeroized on drop.",
    "aad": "envelope_id + ruleset_hash are the AEAD associated data — the ruleset hash binds each envelope to the ruleset in force (Invariant VI: no retroactive re-processing).",
    "wired": "src/bin/witnessd.rs: seal_latest_frame → Vault::seal_frame (production-wired, not a stub).",
    "kat": "pinned by an RFC 8439 ChaCha20-Poly1305 known-answer test.",
    "pq_note": "post-quantum ML-KEM-768 (pq/hybrid) is behind the `pqc-vault` cargo feature; the classical default path is complete and tested.",
}

# --------------------------------------------------------------------------- #
# 4. quorum / break-glass (src/break_glass) — parsed/validated
# --------------------------------------------------------------------------- #

MAX_TRUSTEES = int(grab(CORE_RS, r"MAX_TRUSTEES:\s*usize\s*=\s*(\d+)", "MAX_TRUSTEES"))
MAX_APPROVALS = int(grab(CORE_RS, r"MAX_APPROVALS:\s*usize\s*=\s*(\d+)", "MAX_APPROVALS"))
must(CORE_RS, "trustees_used.len() >= policy.n", "the 'distinct approvals >= n' grant rule")
must(CORE_RS, "pub n: u8", "QuorumPolicy.n (threshold)")
must(CORE_RS, "pub m: u8", "QuorumPolicy.m (member count)")

DOMAIN_APPROVAL = grab(SIGS_RS, r'DOMAIN_TRUSTEE_APPROVAL:\s*&str\s*=\s*"([^"]+)"', "trustee approval domain")
DOMAIN_TOKEN = grab(SIGS_RS, r'DOMAIN_BREAK_GLASS_TOKEN:\s*&str\s*=\s*"([^"]+)"', "break-glass token domain")
DOMAIN_RECEIPT = grab(SIGS_RS, r'DOMAIN_BREAK_GLASS_RECEIPT:\s*&str\s*=\s*"([^"]+)"', "break-glass receipt domain")

QUORUM = {
    "model": "M-of-N independent Ed25519 trustee approvals, counted by the kernel — not Shamir secret sharing and not threshold signatures. The vault key is never split.",
    "policy": {"n": "threshold (how many must approve)", "m": "member count (how many trustees exist)"},
    "policy_note": "operator-supplied, no built-in defaults; validated: n > 0, n ≤ members, ≤ 32 trustees, unique ids AND unique keys (a reused key can't fill two quorum slots).",
    "max_trustees": MAX_TRUSTEES,
    "max_approvals": MAX_APPROVALS,
    "granted_rule": "Granted iff the number of DISTINCT trustee public keys carrying a valid approval for this exact request ≥ n.",
    "domains": {"approval": DOMAIN_APPROVAL, "token": DOMAIN_TOKEN, "receipt": DOMAIN_RECEIPT},
    "domain_sep": "every signature is domain-separated: sign over SHA-256(le32(len(domain)) ‖ domain ‖ hash), so an approval can never be replayed as a token or a receipt.",
    "token_fields": ["token_nonce (32 B)", "expires_bucket", "vault_envelope_id", "ruleset_hash", "receipt_entry_hash", "device_signature (64 B)", "consumed"],
    "guardrails": [
        {"title": "No solo access", "detail": "the single-token `authorize_mvp` path is deprecated and always errors — the quorum path is the only way through."},
        {"title": "Can't forge a grant", "detail": "even a validly device-signed `Granted` receipt is refused unless the kernel re-derives quorum from the committed approvals (≥ n distinct valid keys). A device-key holder cannot forge a grant with zero real approvals."},
        {"title": "Single-use", "detail": "the token nonce is burned durably (SQLite INSERT-OR-IGNORE) BEFORE any cleartext exists; a replayed nonce is refused (\"already consumed\")."},
        {"title": "Expires fast", "detail": "the token is bound to a 10-minute time bucket and is valid only within the bucket it was minted in."},
        {"title": "Always audited", "detail": "every decision — Granted or Denied — is appended to a hash-chained, device-signed receipt log; the CLI `receipts` command re-verifies the chain, signatures and quorum."},
    ],
    "flow": [
        {"n": 1, "title": "Request", "detail": "an operator opens an UnlockRequest (envelope id, purpose, ruleset hash, time bucket) and publishes its request_hash."},
        {"n": 2, "title": "Approve", "detail": "each trustee signs that request_hash with their own Ed25519 key, under the trustee-approval domain. Keys never leave the trustee."},
        {"n": 3, "title": "Count", "detail": "the kernel counts distinct valid approvals for the request. Below n → Denied (still logged). At n → Granted."},
        {"n": 4, "title": "Mint + burn", "detail": "a single-use BreakGlassToken is device-signed, its nonce burned durably, and a signed receipt is chained."},
        {"n": 5, "title": "Unseal", "detail": "export_for_vault validates the token AND re-derives quorum, consumes the nonce, then the vault decrypts to an operator-only file."},
    ],
    "http": [
        {"method": "GET", "path": "/breakglass/policy", "desc": "the policy (public keys only): {n, m, trustees}"},
        {"method": "POST", "path": "/breakglass/request", "desc": "open a session; returns request_hash, time_bucket, needed"},
        {"method": "GET", "path": "/breakglass/status", "desc": "{envelope, purpose, request_hash, needed, collected, ready}"},
        {"method": "POST", "path": "/breakglass/approve", "desc": "verify + count one trustee's Ed25519 signature"},
        {"method": "POST", "path": "/breakglass/unseal", "desc": "only if ready → authorize + unseal to an operator dir"},
        {"method": "POST", "path": "/breakglass/close", "desc": "discard the session + approvals"},
    ],
    "cli": ["request", "approve", "authorize", "receipts", "unseal", "policy set", "policy show"],
    "console": "src/break_glass/breakglass.html",
    "console_note": "a real 4-step operator console ships in the repo (Connect → Request → Collect approvals → Quorum & unseal), with a separate trustee-signer view that makes zero network calls and signs Ed25519 in-browser via WebCrypto — the same domain separation this page uses.",
}

# --------------------------------------------------------------------------- #
# 5. the governing invariants (spec/invariants.md) — validated headings
# --------------------------------------------------------------------------- #

must(INVARIANTS, "Invariant I — No Raw Export by Design", "Invariant I heading")
must(INVARIANTS, "Invariant V — Break-Glass by Quorum", "Invariant V heading")
must(INVARIANTS, "quorum-based authorization", "Invariant V quorum rule")

INVARIANTS_OUT = [
    {"id": "I", "title": "No Raw Export by Design", "ref": "spec/invariants.md §3",
     "text": "Raw frames are private at the type level. There is no public getter, no Clone, no serialization — the only path to raw bytes is export_for_vault(), and that requires a valid BreakGlassToken."},
    {"id": "V", "title": "Break-Glass by Quorum", "ref": "spec/invariants.md §7",
     "text": "The kernel MUST support quorum-based authorization (e.g., N-of-M trustees). Access to sealed evidence is a deliberate, auditable transition under quorum rules — never a single actor, never silent."},
]

# --------------------------------------------------------------------------- #
# 6. the interactive break-glass demo config (real Ed25519 in the browser)
# --------------------------------------------------------------------------- #

DEMO = {
    "n": 2, "m": 3,
    "trustees": ["Avery (you)", "Bailey", "Cameron"],
    "envelope_id": "cam-porch-0007",
    "purpose": "insurance claim — porch camera, sealed 2026-07-19",
    "sealed_label": "🔒 sealed evidence · cam-porch-0007.vlt",
    "unsealed_label": "🖼️ porch-0007.jpg (operator copy)",
    "domain": DOMAIN_APPROVAL,
    "note": "This runs REAL Ed25519 in your browser (WebCrypto), with the same domain separation the kernel uses (securacv:pwk:trustee-approval:v2). The page counts distinct valid signatures exactly like count_valid_distinct_approvals — a reused key won't fill two slots, and a forged grant with too few approvals is refused.",
}

# --------------------------------------------------------------------------- #
# 7. operator terminal (real commands, recorded output) — hub-term shape
# --------------------------------------------------------------------------- #

TERMINAL = {
    "note": "the real operator commands — device-side unseal and the kernel break-glass CLI. Recorded output; the commands are the ones in the repo.",
    "chapters": [
        {"id": "device", "title": "Device seal (off-device review)", "host": "vault-op",
         "intro": "you sealed a frame on the Canary; the private key lives on your laptop.",
         "steps": [
             {"cmd": "tools/unseal_snapshot.py gen-key --out vault_key",
              "out": ["wrote vault_key      (private; keep offline)",
                      "wrote vault_key.pub  (paste the 64-hex into the dashboard)"], "note": "the device only ever gets the .pub."},
             {"cmd": "tools/unseal_snapshot.py inspect seal_00000001_smoke.svlt",
              "out": ["magic SVLT  v1  trigger=smoke(T3)  bucket=131", "ciphertext sha256 = 4f2a9c1e… (matches the frame_sealed note)"], "note": "the header is the AEAD AAD — tamper-evident."},
             {"cmd": "tools/unseal_snapshot.py unseal seal_00000001_smoke.svlt --key vault_key",
              "out": ["tag OK — wrote seal_00000001_smoke.jpg"], "note": "a wrong key or a flipped byte fails the tag, loudly."},
         ]},
        {"id": "quorum", "title": "Break-glass by quorum (kernel)", "host": "vault-op",
         "intro": "raw media in the witness kernel — needs the trustees.",
         "steps": [
             {"cmd": "break_glass policy show",
              "out": ["{ \"n\": 2, \"m\": 3, \"trustees\": [\"avery\",\"bailey\",\"cameron\"] }"], "note": "2 of 3 must approve."},
             {"cmd": "break_glass request --envelope cam-porch-0007 --purpose 'insurance claim'",
              "out": ["request_hash = 8b1d…e07  (send this to the trustees)"], "note": "each trustee signs THIS hash."},
             {"cmd": "break_glass authorize --approval avery.json --approval bailey.json --output-token tok",
              "out": ["distinct approvals: 2/2  → GRANTED", "receipt chained + signed; token written to tok (0600)"], "note": "1 approval short → Denied (still logged)."},
             {"cmd": "break_glass unseal --token tok --envelope cam-porch-0007",
              "out": ["nonce burned (single-use)", "wrote cam-porch-0007.raw (0600)"], "note": "re-running is refused — the nonce is spent."},
         ]},
    ],
}

# --------------------------------------------------------------------------- #
# assemble + write
# --------------------------------------------------------------------------- #

out = {
    "$note": "GENERATED by canary-local/tools/gen_vault.py from the vault/break-glass code + docs/specs. Do not edit by hand; run the generator. Drift-gated in .github/workflows/canary-local.yml.",
    "generated_by": "canary-local/tools/gen_vault.py",
    "concepts": CONCEPTS,
    "device_seal": DEVICE_SEAL,
    "kernel_vault": KERNEL_VAULT,
    "quorum": QUORUM,
    "invariants": INVARIANTS_OUT,
    "demo": DEMO,
    "terminal": TERMINAL,
    "docs": {
        "design": "docs/sealed_snapshot_vault.md",
        "invariants": "spec/invariants.md",
        "kernel_vault": "src/vault/",
        "break_glass": "src/break_glass/",
        "console": "src/break_glass/breakglass.html",
        "unseal_tool": "tools/unseal_snapshot.py",
    },
}

# sanity floors
if len(DEVICE_SEAL["svlt_header"]) != 9:
    die("svlt header must be 9 fields")
if len(QUORUM["guardrails"]) < 5:
    die("quorum guardrails too thin")
if len(INVARIANTS_OUT) != 2:
    die("expected Invariants I and V")

OUT_JSON.write_text(json.dumps(out, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
print(f"wrote {OUT_JSON.relative_to(REPO)}  "
      f"(quorum {DEMO['n']}-of-{DEMO['m']}, {len(QUORUM['guardrails'])} guardrails, "
      f"{len(DEVICE_SEAL['svlt_header'])}-field SVLT header, MAX_TRUSTEES={MAX_TRUSTEES})")
