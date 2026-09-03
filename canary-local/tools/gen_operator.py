#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_operator.py — build canary-local/devices/operator.json from the break-glass
operator CLI.

The Operator's Bench page (`canary-local/operator.html`) teaches the *setup and
day-2 lifecycle* of a break-glass vault — the four commands that just shipped:

    init  →  trustee enroll  →  drill  →  doctor

vault.html already explains what a vault / sealed / quorum ARE and lets you break
the glass yourself; this page is the companion that shows how you *stand one up*
and keep it healthy. Like every other canary-local generator, nothing here is
hand-faked: each command name, flag, guard and message is validated to still exist
in `src/break_glass/cli.rs`, so this file `sys.exit(1)`s on drift and CI
`git diff --exit-code`s the output. Recorded terminal output is authored to match
the real binary, and the message strings it hinges on are drift-anchored below.

Sources of truth (all in-repo, deterministic, offline):
  src/break_glass/cli.rs   the Init / Trustee(Enroll) / Drill / Doctor commands,
                           their flags, the draft-state store, and the messages
  src/break_glass/core.rs  QuorumPolicy{n,m}, MAX_TRUSTEES, validate() guards
  docs/design/vault_operator_ux_v1_1.md  the design (draft-separate-from-policy)
  docs/operator_guide.md   the operator-facing command reference

Run:  python3 canary-local/tools/gen_operator.py
"""

import json
import re
from pathlib import Path

from _tooling import die, repo_root

REPO = repo_root()
CLI_RS = REPO / "src/break_glass/cli.rs"
CORE_RS = REPO / "src/break_glass/core.rs"
RFC = REPO / "docs/design/vault_operator_ux_v1_1.md"
OPGUIDE = REPO / "docs/operator_guide.md"
OUT_JSON = REPO / "canary-local/devices/operator.json"

_CACHE: dict = {}


def read(path: Path) -> str:
    if path not in _CACHE:
        if not path.exists():
            die(f"source missing: {path.relative_to(REPO)}")
        _CACHE[path] = path.read_text(encoding="utf-8", errors="replace")
    return _CACHE[path]


def must(path: Path, needle: str, label: str) -> None:
    if needle not in read(path):
        die(f"{label}: expected {needle!r} in {path.relative_to(REPO)} — CLI changed?")


def grab(path: Path, pattern: str, label: str, flags=0) -> str:
    m = re.search(pattern, read(path), flags)
    if not m:
        die(f"{label}: /{pattern}/ not found in {path.relative_to(REPO)}")
    return m.group(1)


# --------------------------------------------------------------------------- #
# 1. drift anchors — the CLI surface this page depends on must still exist.
# --------------------------------------------------------------------------- #

# the four commands + the enroll subcommand
must(CLI_RS, "Init {", "Init command")
must(CLI_RS, "TrusteeCommand", "Trustee subcommand group")
must(CLI_RS, "Enroll {", "trustee Enroll command")
must(CLI_RS, "Drill {", "Drill command")
must(CLI_RS, "Doctor {", "Doctor command")
for fn in ("fn cmd_init", "fn cmd_trustee_enroll", "fn cmd_drill", "fn cmd_doctor"):
    must(CLI_RS, fn, f"handler {fn}")

# the draft-state store (kept SEPARATE from the committed policy)
must(CLI_RS, "SetupDraft", "draft-state struct")
must(CLI_RS, "setup-draft.json", "draft file naming")

# enroll modes + guards
must(CLI_RS, "--public-key", "enroll --public-key (import)")
must(CLI_RS, "--generate", "enroll --generate (mint)")
must(CLI_RS, "minted a signing key", "generate mode message")
must(CLI_RS, "already enrolled", "duplicate-id guard")
must(CLI_RS, "quorum independence requires distinct keys", "duplicate-key guard")
must(CLI_RS, "no setup in progress", "enroll-before-init guard")

# lifecycle messages the ceremony records
must(CLI_RS, "device identity pinned", "init pins identity")
must(CLI_RS, "started a", "init starts a draft")
must(CLI_RS, "quorum policy is live", "commit-on-valid message")
must(CLI_RS, "DRILL PASSED", "drill success message")
must(CLI_RS, "Result: HEALTHY", "doctor healthy message")
must(CLI_RS, "device public key pinned", "doctor identity check")
must(CLI_RS, "the master key is plaintext on disk", "doctor honesty warning")

# drill runs in a throwaway sandbox and burns its token single-use
must(CLI_RS, "Nothing real is touched", "drill sandbox promise")
must(CLI_RS, "consume_break_glass_token_durably", "drill burns the token single-use")

# the committed policy always goes through validate() (Invariant V)
must(CLI_RS, "set_break_glass_policy", "commit path")
must(CORE_RS, "quorum must include at least one trustee", "validate rejects empty roster")
must(CORE_RS, "trustees_used.len() >= policy.n", "distinct-approvals grant rule")
MAX_TRUSTEES = int(grab(CORE_RS, r"MAX_TRUSTEES:\s*usize\s*=\s*(\d+)", "MAX_TRUSTEES"))

# the drill defaults, straight from the arg definitions
DRILL_N = int(grab(CLI_RS, r"threshold to rehearse.*?default_value_t\s*=\s*(\d+)",
                   "drill --threshold default", re.DOTALL))
DRILL_M = int(grab(CLI_RS, r"trustees in the rehearsed quorum.*?default_value_t\s*=\s*(\d+)",
                   "drill --trustees default", re.DOTALL))

# the design promise this whole page rests on
must(RFC, "kept", "RFC draft-separation note")
must(OPGUIDE, "Guided setup", "operator-guide guided-setup section")


# --------------------------------------------------------------------------- #
# 2. the four commands (authored copy; anchored to the CLI above)
# --------------------------------------------------------------------------- #

COMMANDS = [
    {
        "name": "init",
        "one_line": "Open a guided n-of-m setup.",
        "usage": "break_glass init --threshold <n> --trustees <m> --db witness.db",
        "what": "Pins the device identity in the (encrypted) kernel database and opens an "
                "n-of-m setup draft. Writes no quorum policy yet — an empty roster is not a "
                "valid gate. Idempotent: re-running reports where you are, it never clobbers.",
        "flags": [
            {"flag": "--threshold n", "desc": "how many trustees must approve to break the glass"},
            {"flag": "--trustees m", "desc": "how many trustees you plan to enroll in total"},
            {"flag": "--device-key-seed", "desc": "the device seed (also DEVICE_KEY_SEED) — opens the encrypted DB"},
        ],
    },
    {
        "name": "trustee enroll",
        "one_line": "Add one trustee — import a key, or mint one.",
        "usage": "break_glass trustee enroll --id <name> (--public-key <HEX> | --generate --output <file>)",
        "what": "Adds one trustee to the draft. Import a public key a trustee generated "
                "themselves (--public-key), or mint a fresh Ed25519 keypair for them "
                "(--generate) — the signing key is written at mode 0600 to hand over. Rejects "
                "duplicate ids and reused keys up front. The real quorum policy commits — and "
                "then strengthens — automatically the moment the draft is a valid quorum.",
        "flags": [
            {"flag": "--id name", "desc": "a short, unique label for this trustee"},
            {"flag": "--public-key HEX", "desc": "import an existing 32-byte Ed25519 public key"},
            {"flag": "--generate --output FILE", "desc": "mint a keypair; write the signing key (0600) to hand over"},
            {"flag": "--device-key-seed", "desc": "the device seed (also DEVICE_KEY_SEED) — required, opens the encrypted DB"},
        ],
    },
    {
        "name": "drill",
        "one_line": "Rehearse the whole break-glass, risk-free.",
        "usage": f"break_glass drill --threshold {DRILL_N} --trustees {DRILL_M}",
        "what": "Runs the entire request → approve → authorize → seal → unseal path in a "
                "throwaway sandbox — a temp database, a temp vault and ephemeral trustee keys, "
                "all discarded afterward. It touches nothing real. Proves break-glass actually "
                "works on this build/host before you need it at 3 a.m., and lets trustees "
                "practice with zero risk. Exits non-zero if any step fails.",
        "flags": [
            {"flag": f"--threshold n (default {DRILL_N})", "desc": "quorum threshold to rehearse"},
            {"flag": f"--trustees m (default {DRILL_M})", "desc": "trustees in the rehearsed quorum"},
        ],
    },
    {
        "name": "doctor",
        "one_line": "Is this vault set up correctly? Gate a deploy on it.",
        "usage": "break_glass doctor --db witness.db",
        "what": "A read-only health check. Reports the quorum policy (n-of-m, every trustee key "
                "well-formed), whether the device identity is pinned AND matches the supplied "
                "seed, and the vault master key's state — including a loud reminder that the "
                "master key is plaintext on disk (the honest state until hardware-backed keys "
                "land). Exits non-zero on any problem, so it can gate a deploy in a script or CI.",
        "flags": [
            {"flag": "--db", "desc": "the kernel database to inspect"},
            {"flag": "--vault-path", "desc": "where master.key lives (default vault/envelopes)"},
            {"flag": "--device-key-seed", "desc": "verifies the seed derives the pinned identity"},
        ],
    },
]

# --------------------------------------------------------------------------- #
# 3. the concepts this page makes concrete
# --------------------------------------------------------------------------- #

CONCEPTS = [
    {"title": "A draft, kept apart from the gate",
     "blurb": "Enrollment fills a draft roster — a plain file next to the database "
              "(<db>.setup-draft.json), holding only public keys and counts, no secrets. "
              "It is deliberately separate from the committed quorum policy, because the kernel "
              "rejects an empty or partial roster by design (Invariant V). A half-finished setup "
              "can never be mistaken for a live gate."},
    {"title": "It goes live the moment it's valid",
     "blurb": "The real policy commits automatically once the draft is a genuine quorum — the "
              "instant the n-th trustee is enrolled — and every further enrollment strengthens "
              "it (n-of-n → n-of-(n+1) → …). Every commit runs the kernel's own validate(), so "
              "the gate is never partial and reused keys are refused."},
    {"title": "Mint or import — your call",
     "blurb": "A trustee can generate their own key and send you only the public half (import), "
              "or you can mint one for them. A minted signing key is written at mode 0600 to "
              "hand over — it is the one secret in the whole flow; the draft never holds it."},
    {"title": "Rehearse before you trust it",
     "blurb": "drill runs the full break-glass in a throwaway sandbox — real crypto, real "
              "quorum counting, real single-use token burn — and proves recovery works before "
              "an incident, with zero risk to your vault. Then doctor gates the deploy."},
    {"title": "Honest about what isn't hardened yet",
     "blurb": "doctor says out loud that the master key is plaintext on disk — the honest state "
              "until hardware-backed keys land (v1.1) — and fails hard if that key is readable "
              "beyond its owner or if the supplied seed doesn't match the pinned identity."},
]

# --------------------------------------------------------------------------- #
# 4. the ceremony — recorded output, matching the real binary (anchored above)
# --------------------------------------------------------------------------- #

CEREMONY = {
    "threshold": 2,
    "target": 3,
    "note": "the exact commands, in order, with the real binary's output. Each command that opens "
            "the encrypted kernel database carries DEVICE_KEY_SEED=… inline (init, every enroll, "
            "doctor) — export it once instead if you prefer, or pass --device-key-seed. drill needs "
            "no seed: it runs entirely in a throwaway sandbox. Watch the roster fill and the policy "
            "go live the instant it becomes a valid 2-of-3.",
    "steps": [
        {"cmd": "DEVICE_KEY_SEED=devkey:your-seed break_glass init --threshold 2 --trustees 3 --db witness.db",
         "out": [
             "=== Break-glass setup — init ===",
             "  ✓ device identity pinned: b5c3d676657d54cd",
             "  ✓ started a 2-of-3 setup draft (witness.db.setup-draft.json)",
             "    the quorum policy commits automatically once 2 trustee(s) are enrolled",
         ],
         "roster": [], "policy": None,
         "note": "Identity pinned; a 2-of-3 draft opened. No policy yet — an empty roster isn't a gate."},
        {"cmd": "DEVICE_KEY_SEED=devkey:your-seed break_glass trustee enroll --id alice --public-key 0123… --db witness.db",
         "out": ["  ✓ enrolled trustee 'alice' (1/3)"],
         "roster": ["alice"], "policy": None,
         "note": "One of three. Still below the threshold of 2, so no policy commits."},
        {"cmd": "DEVICE_KEY_SEED=devkey:your-seed break_glass trustee enroll --id bob --public-key 4567… --db witness.db",
         "out": ["  ✓ enrolled trustee 'bob' (2/3)",
                 "  ✓ quorum policy is live: 2-of-2"],
         "roster": ["alice", "bob"], "policy": "2-of-2",
         "note": "Threshold reached — the policy goes live as a valid 2-of-2 and the vault can now open under quorum."},
        {"cmd": "DEVICE_KEY_SEED=devkey:your-seed break_glass trustee enroll --id carol --generate --output carol.key --db witness.db",
         "out": ["  ✓ minted a signing key for 'carol' → carol.key (secret, mode 0600 — hand it to the trustee)",
                 "  ✓ enrolled trustee 'carol' (3/3)",
                 "  ✓ quorum policy is live: 2-of-3"],
         "roster": ["alice", "bob", "carol"], "policy": "2-of-3",
         "note": "Minted carol a key (written 0600 to hand over) and reached the target — the gate strengthens to 2-of-3."},
        {"cmd": "break_glass drill --threshold 2 --trustees 3",
         "out": [
             "=== Break-glass drill ===",
             "  ✓ set up a 2-of-3 sandbox quorum",
             "  ✓ 2 trustees approved; two break-glass tokens minted (one to seal, one to unseal)",
             "  ✓ sealed a throwaway envelope",
             "  ✓ unsealed it with a fresh quorum token",
             "  ✓ recovered payload matches the original, byte for byte",
             "Result: DRILL PASSED — break-glass works end to end on this build.",
         ],
         "roster": ["alice", "bob", "carol"], "policy": "2-of-3",
         "note": "A full rehearsal in a throwaway sandbox — real crypto, discarded after. Nothing real touched."},
        {"cmd": "DEVICE_KEY_SEED=devkey:your-seed break_glass doctor --db witness.db",
         "out": [
             "=== Break-glass vault doctor ===",
             "[ quorum policy ]",
             "  ✓ quorum policy configured: 2-of-3",
             "  ✓ 3 trustee key(s), all well-formed",
             "[ device identity ]",
             "  ✓ device public key pinned, matches the supplied seed: b5c3d676657d54cd",
             "[ vault master key ]",
             "  ⚠ the master key is plaintext on disk — no hardware backing yet (file backend)",
             "Result: HEALTHY (1 warning(s))",
         ],
         "roster": ["alice", "bob", "carol"], "policy": "2-of-3",
         "note": "Read-only health check: policy, identity match, key state — HEALTHY, exit 0. The plaintext-key warning is the honest state until v1.1 hardware backing."},
    ],
}

# --------------------------------------------------------------------------- #
# 5. what doctor actually checks + what drill actually does
# --------------------------------------------------------------------------- #

DOCTOR = {
    "read_only": True,
    "gates_deploy": "exits non-zero on any problem, so a script or CI step can gate on it.",
    "checks": [
        {"section": "quorum policy", "ok": "configured n-of-m; every trustee public key well-formed",
         "fail": "no policy, an invalid stored policy, or a malformed trustee key"},
        {"section": "device identity", "ok": "pinned in the DB AND derived by the supplied seed",
         "fail": "no pinned key, or the seed doesn't match — authorize/witnessd would reject it"},
        {"section": "vault master key", "ok": "present, 32 bytes, owner-only (0600)",
         "fail": "wrong size, OR readable beyond its owner (a local user could decrypt without quorum)"},
        {"section": "honesty", "ok": "—", "warn": "the master key is plaintext on disk (no hardware backing yet, v1.1)"},
    ],
}

DRILL = {
    "sandbox": "a temp kernel database, a temp vault, and ephemeral trustee keys — all discarded "
               "when it finishes. It touches nothing real.",
    "exercises": [
        "stands up an n-of-m quorum with fresh ephemeral trustees",
        "the first n approve; a break-glass token is minted and a receipt logged",
        "seals a dummy payload, then mints a SECOND quorum token",
        "burns that token durably (single-use), then unseals",
        "confirms the recovered bytes match the sealed bytes, byte for byte",
    ],
    "why": "proves break-glass executes on this build/host before a real incident, and lets "
           "trustees practice the flow with zero risk. A green drill is meaningful because it "
           "runs the same code paths a real break-glass uses — including the single-use burn.",
}

# --------------------------------------------------------------------------- #
# assemble + write
# --------------------------------------------------------------------------- #

out = {
    "$note": "GENERATED by canary-local/tools/gen_operator.py from the break-glass operator CLI "
             "(src/break_glass/cli.rs) + core + the v1.1 design. Do not edit by hand; run the "
             "generator. Drift-gated in .github/workflows/canary-local.yml.",
    "generated_by": "canary-local/tools/gen_operator.py",
    "lede": "You have a vault that seals evidence no one can open alone. This is how you stand it "
            "up — and prove it works — in four commands: init, enroll, drill, doctor.",
    "commands": COMMANDS,
    "concepts": CONCEPTS,
    "ceremony": CEREMONY,
    "doctor": DOCTOR,
    "drill": DRILL,
    "max_trustees": MAX_TRUSTEES,
    "docs": {
        "cli": "src/break_glass/cli.rs",
        "design": "docs/design/vault_operator_ux_v1_1.md",
        "operator_guide": "docs/operator_guide.md",
        "concepts_page": "vault.html",
    },
}

# sanity floors
if len(COMMANDS) != 4:
    die("expected exactly the four operator commands")
if len(CEREMONY["steps"]) != 6:
    die("ceremony must be the 6-step init→enroll×3→drill→doctor walk")
if len(CONCEPTS) < 5:
    die("concepts too thin")

OUT_JSON.write_text(json.dumps(out, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
print(f"wrote {OUT_JSON.relative_to(REPO)}  "
      f"({len(COMMANDS)} commands, {len(CEREMONY['steps'])}-step ceremony, "
      f"{len(CONCEPTS)} concepts, drill {DRILL_N}-of-{DRILL_M}, MAX_TRUSTEES={MAX_TRUSTEES})")
