#!/usr/bin/env bash
#
# setup_release_key.sh — one-time OTA release signing key ceremony.
#
# ┌─────────────────────────────────────────────────────────────────────────┐
# │  RUN THIS ON YOUR OWN MACHINE. NEVER in CI, never in a shared/cloud      │
# │  shell, never inside an AI agent's environment.                          │
# │                                                                          │
# │  It generates the Ed25519 *private* release key (releaser.pem). That     │
# │  key is the master key that signs every firmware image a Canary will     │
# │  accept over the air. Anyone who has it can push firmware to every       │
# │  device in the field. Treat it like a code-signing certificate:          │
# │    • It stays OFFLINE, on your machine (or a hardware token / password    │
# │      manager). Back it up somewhere private.                             │
# │    • It is NEVER committed. `.gitignore` already ignores `*.pem`; this    │
# │      script also refuses to write it inside the repo working tree.       │
# │    • Only its PUBLIC half (a C header) goes into the repo, and a copy of  │
# │      the PEM goes into the OTA_SIGNING_KEY_PEM GitHub Actions secret.     │
# └─────────────────────────────────────────────────────────────────────────┘
#
# What it does (idempotent — safe to re-run):
#   1. Verifies Python + the `cryptography` module are available.
#   2. Generates releaser.pem if it doesn't exist (refuses to clobber an
#      existing key unless you pass --force).
#   3. Writes the PUBLIC key header to the canonical location and syncs the
#      two committed Arduino copies, so check_ota_sync.sh stays green.
#   4. Prints the exact next steps: set the secret, commit the public header,
#      cut the release.
#
# Usage:
#   firmware/scripts/setup_release_key.sh                 # key -> ./releaser.pem
#   firmware/scripts/setup_release_key.sh --key ~/keys/securacv-releaser.pem
#   firmware/scripts/setup_release_key.sh --force         # regenerate (ROTATES the key)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Defaults: key next to your CWD, not inside the repo.
KEY_PATH="releaser.pem"
FORCE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --key)   KEY_PATH="$2"; shift 2 ;;
        --key=*) KEY_PATH="${1#*=}"; shift ;;
        --force) FORCE="--force"; shift ;;
        -h|--help)
            sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
warn() { printf '\033[33m%s\033[0m\n' "$1"; }
ok()   { printf '\033[32m%s\033[0m\n' "$1"; }

# ── Guard: never let the private key land inside the repo working tree ──────
KEY_ABS="$(cd "$(dirname "$KEY_PATH")" 2>/dev/null && pwd)/$(basename "$KEY_PATH")" || KEY_ABS="$KEY_PATH"
case "$KEY_ABS" in
    "$REPO_ROOT"/*)
        echo "::refusing:: $KEY_PATH is inside the repository ($REPO_ROOT)." >&2
        echo "The private key must live OUTSIDE the repo so it can never be committed." >&2
        echo "Re-run with --key pointing somewhere outside, e.g.:" >&2
        echo "    firmware/scripts/setup_release_key.sh --key ~/securacv-releaser.pem" >&2
        exit 2 ;;
esac

# ── Step 1: prerequisites ───────────────────────────────────────────────────
PY="$(command -v python3 || command -v python || true)"
if [ -z "$PY" ]; then
    echo "Python 3 not found. Install Python 3, then re-run." >&2
    exit 1
fi
if ! "$PY" -c "import cryptography" 2>/dev/null; then
    warn "The 'cryptography' Python module is required and not installed."
    echo "Install it, then re-run this script:"
    echo "    $PY -m pip install cryptography"
    exit 1
fi

OTA_TOOL="${REPO_ROOT}/firmware/scripts/ota_release.py"
CANONICAL_HEADER="${REPO_ROOT}/firmware/common/ota/src/ota_release_key.h"
STAGED_HEADERS=(
    "${REPO_ROOT}/firmware/projects/canary-wap/arduino/canary_wap/ota_release_key.h"
    "${REPO_ROOT}/firmware/projects/canary-display/arduino/canary_display/ota_release_key.h"
)

# ── Step 2: generate the private key (idempotent) ──────────────────────────
bold "1/3  Release signing key"
if [ -f "$KEY_PATH" ] && [ -z "$FORCE" ]; then
    ok  "  Using existing key: $KEY_PATH (pass --force to rotate/regenerate)"
else
    if [ -f "$KEY_PATH" ]; then
        warn "  --force given: ROTATING the key. Old firmware signed with the"
        warn "  previous key will no longer be trusted by NEW devices."
    fi
    "$PY" "$OTA_TOOL" keygen --private-key "$KEY_PATH" $FORCE
    chmod 600 "$KEY_PATH" 2>/dev/null || true
    ok  "  Wrote private key: $KEY_PATH"
fi

# ── Step 3: derive + sync the PUBLIC header ────────────────────────────────
bold "2/3  Public key header (safe to commit)"
"$PY" "$OTA_TOOL" pubkey-header --private-key "$KEY_PATH" --out "$CANONICAL_HEADER"
for dst in "${STAGED_HEADERS[@]}"; do
    cp "$CANONICAL_HEADER" "$dst"
    echo "  synced -> ${dst#"${REPO_ROOT}/"}"
done
if [ -x "${REPO_ROOT}/firmware/scripts/check_ota_sync.sh" ]; then
    if "${REPO_ROOT}/firmware/scripts/check_ota_sync.sh" >/dev/null 2>&1; then
        ok "  check_ota_sync.sh: all copies byte-identical ✓"
    else
        warn "  check_ota_sync.sh reported drift — inspect before committing."
    fi
fi

# The pubkey header embeds the key id in a comment ("Key id ...: <hex>").
KEY_ID="$(grep -i 'Key id' "$CANONICAL_HEADER" 2>/dev/null | head -1 | sed 's/^[[:space:]]*\*[[:space:]]*//' || true)"

# ── Next steps ──────────────────────────────────────────────────────────────
bold "3/3  Next steps (do these yourself — the script stops here on purpose)"
cat <<EOF

  a) Add the PRIVATE key as a GitHub Actions secret so CI can sign releases:
       GitHub → your repo → Settings → Secrets and variables → Actions
       → New repository secret
         Name:  OTA_SIGNING_KEY_PEM
         Value: the FULL contents of $KEY_PATH
                (everything from '-----BEGIN PRIVATE KEY-----' to
                 '-----END PRIVATE KEY-----', inclusive)

     Copy it to your clipboard with:
EOF
if command -v pbcopy >/dev/null 2>&1; then
    echo "         pbcopy < $KEY_PATH        # macOS"
elif command -v xclip >/dev/null 2>&1; then
    echo "         xclip -sel clip < $KEY_PATH   # Linux"
else
    echo "         cat $KEY_PATH             # then copy the output"
fi
cat <<EOF

  b) Commit ONLY the public header (never $KEY_PATH):
       git add firmware/common/ota/src/ota_release_key.h \\
               firmware/projects/canary-wap/arduino/canary_wap/ota_release_key.h \\
               firmware/projects/canary-display/arduino/canary_display/ota_release_key.h
       git commit -m "Embed OTA release public key (ceremony)"
       git push

  c) Cut the release — Actions → "Firmware Release — if changed" → Run workflow
     (or the one-click launcher). It will sign with OTA_SIGNING_KEY_PEM.

EOF
warn "REMINDER: $KEY_PATH is your master signing key. Keep it offline, back it"
warn "up privately, and never commit or paste it anywhere but the GitHub secret."
[ -n "$KEY_ID" ] && echo "  $KEY_ID"
