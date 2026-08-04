#!/bin/bash
# Put a *single-identity* Developer ID Application .p12 into the repo secrets
# the desktop apps (Flasher, Lab) sign with. Run it on the Mac that holds the
# signing key:
#
#     bash desktop/scripts/set-desktop-signing-secrets.sh
#
# Why this exists instead of a Keychain Access click-path (desktop/SIGNING.md
# used to say "export it by hand"): every hand export so far went wrong in a
# way that only surfaced in CI.
#
#   * `security export -t identities` writes EVERY identity in the keychain.
#     Tauri validates the LAST certificate in the .p12, so an iOS cert riding
#     along makes it abort with "certificate ... does not match provided
#     identity" — even though the right cert is in there. (Three dry runs.)
#   * Keychain Access saves wherever it last saved, so `base64 -i <path>` hit a
#     file that wasn't there, printed nothing, and `gh secret set` cheerfully
#     stored an EMPTY secret. (One more dry run.)
#   * A copy-pasted `VAR=path   # comment` line makes zsh run `#` as a command,
#     leaving VAR unset and every later check reading zero. (One more.)
#
# So: no paths to fill in, no GUI, and nothing is written to GitHub unless the
# .p12 this builds contains exactly one certificate, exactly one matching
# private key, an unexpired cert, and a common name identical to the identity
# the workflow asks for. Those are the same four things the release workflow's
# preflight checks — see "Verify the macOS signing certificate matches the
# identity" in .github/workflows/desktop-flasher-release.yml.
#
# Expect one macOS dialog asking to let `security` export the key. That is the
# keychain guarding your private key; enter your login password and Allow.

set -euo pipefail

REPO="${REPO:-kmay89/securaCV}"
KEEP_DIR="${KEEP_DIR:-$HOME/securacv-signing}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Common name of a PEM certificate. /usr/bin/openssl on macOS is LibreSSL, not
# OpenSSL 3, and the two print subjects differently ("/CN=x" vs "CN = x"), so
# fall back to parsing the one-line form rather than returning empty — an empty
# CN here would report "your identity isn't in the export" for a keychain that
# has it, which is the worst possible lie to tell at this step.
cn_of() {
  local cn
  cn="$(openssl x509 -in "$1" -noout -subject -nameopt multiline 2>/dev/null \
        | awk '/^ *commonName/{sub(/^ *commonName *= */, ""); print; exit}')"
  if [ -z "$cn" ]; then
    # Both separators map to newline deliberately, so string2 repeats \n: BSD tr
    # and GNU tr disagree about padding a short string2, and a one-character
    # string2 would behave differently on the Mac this runs on.
    # shellcheck disable=SC2020
    cn="$(openssl x509 -in "$1" -noout -subject 2>/dev/null \
          | awk '{sub(/^subject=[[:space:]]*/, "")} 1' \
          | tr ',/' '\n\n' \
          | awk -F'[[:space:]]*=[[:space:]]*' '/^[[:space:]]*CN[[:space:]]*=/{print $2; exit}')"
  fi
  printf '%s' "$cn"
}

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
ok()   { printf '  ok   %s\n' "$*"; }
die()  { printf '\n\033[31mSTOPPED\033[0m %s\n\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- preconditions
say "Checking tools"
[ "$(uname -s)" = "Darwin" ] || die "This has to run on the Mac that holds the signing key."
command -v security >/dev/null || die "No /usr/bin/security."
command -v openssl  >/dev/null || die "No openssl."
command -v gh       >/dev/null || die "GitHub CLI missing. Install it: brew install gh"
gh auth status >/dev/null 2>&1 || die "gh is not signed in. Run: gh auth login"
ok "security, openssl, gh (signed in)"

# ------------------------------------------------------------------- identity
# find-identity -p codesigning lists only codesigning-valid identities, which
# is the right filter for "what can actually sign" — but note it hides some
# certs that `security export` will still dump, which is the whole reason this
# script repacks rather than trusting the export.
say "Finding the Developer ID Application identity"
security find-identity -v -p codesigning > "$WORK/ids.txt" 2>/dev/null || true
IDENT="$(awk -F'"' '/Developer ID Application:/ {print $2}' "$WORK/ids.txt" | sort -u)"
N_IDENT="$(printf '%s\n' "$IDENT" | awk 'NF' | wc -l | tr -d ' ')"
if [ "$N_IDENT" = "0" ]; then
  die "No 'Developer ID Application' identity in this keychain.

That is the certificate a DMG downloaded outside the App Store signs with —
NOT 'Apple Distribution' (App Store) and NOT 'Apple Development'. Create one at
developer.apple.com/account/resources/certificates → + → Developer ID
Application, download it, double-click to install, then re-run this.

What this keychain does have:
$(sed 's/^/  /' "$WORK/ids.txt")"
fi
[ "$N_IDENT" = "1" ] || die "More than one Developer ID Application identity here; I won't guess which:
$(printf '%s\n' "$IDENT" | sed 's/^/  - /')"
ok "$IDENT"

# --------------------------------------------------------------------- export
# The keychain will only hand over a private key with the user's consent, so a
# GUI prompt here is expected, not a failure. The password is a throwaway for a
# file inside $WORK that this script deletes on exit.
say "Exporting identities from the keychain (expect a macOS Allow prompt)"
TMPPW="$(openssl rand -hex 16)"; export TMPPW
security export -t identities -f pkcs12 -P "$TMPPW" -o "$WORK/all.p12" \
  || die "The keychain export was canceled or denied. Re-run and click Allow."
ok "exported"

# OpenSSL 3 needs -legacy for the RC2-encrypted PKCS#12 macOS writes; LibreSSL
# (what /usr/bin/openssl usually is) has no -legacy flag at all. Try both.
if ! openssl pkcs12 -in "$WORK/all.p12" -nodes -passin env:TMPPW -legacy \
        -out "$WORK/all.pem" 2>/dev/null; then
  openssl pkcs12 -in "$WORK/all.p12" -nodes -passin env:TMPPW \
        -out "$WORK/all.pem" 2>/dev/null \
    || die "Could not read the keychain's own export. Send me the output of: openssl version"
fi

# ---------------------------------------------------------------------- split
awk '/-----BEGIN CERTIFICATE-----/{c++; f=d "/cert." c ".pem"; p=1}
     p{print > f}
     /-----END CERTIFICATE-----/{p=0}' d="$WORK" "$WORK/all.pem"
awk '/-----BEGIN .*PRIVATE KEY-----/{k++; f=d "/key." k ".pem"; p=1}
     p{print > f}
     /-----END .*PRIVATE KEY-----/{p=0}' d="$WORK" "$WORK/all.pem"
rm -f "$WORK/all.pem" "$WORK/all.p12"

say "Picking out that one certificate and its private key"
CERT=""
for f in "$WORK"/cert.*.pem; do
  [ -e "$f" ] || continue
  [ "$(cn_of "$f")" = "$IDENT" ] && { CERT="$f"; break; }
done
[ -n "$CERT" ] || die "The keychain export didn't contain '$IDENT'. If the key shows as
non-exportable, the certificate was installed without its private key and has
to be re-created at developer.apple.com."

# Match key to certificate by public key, not by position: the export's
# ordering is not guaranteed and pairing the wrong key produces a .p12 that
# imports fine and then fails to sign.
CPUB="$(openssl x509 -in "$CERT" -noout -pubkey | openssl dgst -sha256 | awk '{print $NF}')"
KEY=""
for f in "$WORK"/key.*.pem; do
  [ -e "$f" ] || continue
  kpub="$(openssl pkey -in "$f" -pubout 2>/dev/null | openssl dgst -sha256 | awk '{print $NF}')" || continue
  [ "$kpub" = "$CPUB" ] && { KEY="$f"; break; }
done
[ -n "$KEY" ] || die "Found the certificate but not its private key. Signing needs both."
ok "certificate and matching private key"

openssl x509 -in "$CERT" -checkend 0 >/dev/null 2>&1 \
  || die "That certificate has already expired ($(openssl x509 -in "$CERT" -noout -enddate | cut -d= -f2)). Create a new Developer ID Application certificate."
if ! openssl x509 -in "$CERT" -checkend 2592000 >/dev/null 2>&1; then
  printf '  \033[33mnote\033[0m expires within 30 days: %s\n' \
    "$(openssl x509 -in "$CERT" -noout -enddate | cut -d= -f2)"
else
  ok "valid until $(openssl x509 -in "$CERT" -noout -enddate | cut -d= -f2)"
fi

# ---------------------------------------------------------------------- repack
# One leaf certificate only. Not even the Apple intermediate: the preflight
# enumerates every certificate in the file and treats anything that isn't the
# wanted identity as an extra, and macOS runners already trust Apple's
# Developer ID intermediate.
say "Building a single-identity .p12"
NEWPW="$(openssl rand -base64 24)"; export NEWPW
# -keypbe/-certpbe/-macalg pin the LEGACY PKCS#12 algorithms. OpenSSL 3 defaults
# to AES-256 with a SHA-256 MAC, which macOS's Security framework cannot read —
# `security import` (what tauri runs to sign) rejects it as
#   SecKeychainItemImport: MAC verification failed during PKCS12 import
#   (wrong password?)
# The password is fine; that message is simply wrong about the cause. openssl
# reads such a file back happily, so no openssl-based check catches it — the
# first signal was a failed 7.5-minute CI build. See RELEASE_LESSONS (p).
openssl pkcs12 -export -out "$WORK/desktop.p12" -inkey "$KEY" -in "$CERT" \
  -name "$IDENT" -passout env:NEWPW \
  -keypbe PBE-SHA1-3DES -certpbe PBE-SHA1-3DES -macalg sha1 \
  || die "Repacking the .p12 failed."

if ! openssl pkcs12 -in "$WORK/desktop.p12" -passin env:NEWPW -nokeys -legacy \
        -out "$WORK/check.pem" 2>/dev/null; then
  openssl pkcs12 -in "$WORK/desktop.p12" -passin env:NEWPW -nokeys \
        -out "$WORK/check.pem" 2>/dev/null || die "Built a .p12 I can't reopen."
fi
N_CERT="$(grep -c 'BEGIN CERTIFICATE' "$WORK/check.pem" || true)"
[ "$N_CERT" = "1" ] || die "Built a .p12 holding $N_CERT certificates; the release preflight requires exactly 1."
ok "exactly 1 certificate, 1 private key"

# The check that actually predicts CI: import it with `security`, the same tool
# tauri uses on the runner, into a throwaway keychain we delete immediately.
# Everything above this line is openssl agreeing with openssl.
PROBE="$WORK/probe.keychain"
security create-keychain -p probe "$PROBE" >/dev/null 2>&1 \
  || die "Could not create a scratch keychain to test the .p12 with."
if security import "$WORK/desktop.p12" -k "$PROBE" -P "$NEWPW" \
     -T /usr/bin/codesign >/dev/null 2>&1; then
  security delete-keychain "$PROBE" >/dev/null 2>&1 || true
  ok "macOS imports it (same command the release runner uses)"
else
  security delete-keychain "$PROBE" >/dev/null 2>&1 || true
  die "Built a .p12 that macOS itself refuses to import, so signing would fail in
CI even though every openssl check passed. Send me this line:
  $(openssl version)"
fi

B64="$(base64 -i "$WORK/desktop.p12" | tr -d '\n')"
[ "${#B64}" -gt 1000 ] || die "The base64 came out ${#B64} characters long, which cannot be a real .p12."
ok "base64 is ${#B64} characters"

# ---------------------------------------------------------------------- upload
# Values go in on stdin, never argv: keeps the key and password out of shell
# history and out of `ps`.
say "Setting the secrets on $REPO"
printf '%s' "$B64"   | gh secret set APPLE_DESKTOP_CERTIFICATE          --repo "$REPO"
printf '%s' "$NEWPW" | gh secret set APPLE_DESKTOP_CERTIFICATE_PASSWORD --repo "$REPO"
# Set the identity from the same certificate that just went up, so the
# workflow's exact-string comparison cannot fail on a stray space or an old
# team name.
printf '%s' "$IDENT" | gh secret set APPLE_SIGNING_IDENTITY             --repo "$REPO"
ok "APPLE_DESKTOP_CERTIFICATE, APPLE_DESKTOP_CERTIFICATE_PASSWORD, APPLE_SIGNING_IDENTITY"

# ------------------------------------------------------------------------ keep
# Developer ID certificates are limited per account and revoking one breaks
# apps already shipped under it, so losing this key is expensive. Keep a copy.
mkdir -p "$KEEP_DIR"; chmod 700 "$KEEP_DIR"
cp "$WORK/desktop.p12" "$KEEP_DIR/developer-id-application.p12"
printf '%s\n' "$NEWPW" > "$KEEP_DIR/developer-id-application.password.txt"
chmod 600 "$KEEP_DIR/developer-id-application.p12" \
          "$KEEP_DIR/developer-id-application.password.txt"

# ------------------------------------------------------- what's still missing
# This script only owns the three certificate secrets. Signing also needs
# notarization credentials and the ENABLE_MACOS_SIGNING variable — and the
# workflows take the UNSIGNED branch unless that variable is exactly "true", so
# an incomplete setup ships an unsigned app with a green checkmark rather than
# failing. Report the gap here instead of letting a release discover it.
# --json needs gh >= 2.30; fall back to the table output on older versions.
gh_secret_names() {
  gh secret list --repo "$REPO" --json name --jq '.[].name' 2>/dev/null && return 0
  gh secret list --repo "$REPO" 2>/dev/null | awk '{print $1}'
}
gh_variable_value() {
  gh variable list --repo "$REPO" --json name,value \
     --jq ".[] | select(.name==\"$1\") | .value" 2>/dev/null && return 0
  gh variable list --repo "$REPO" 2>/dev/null | awk -v n="$1" '$1 == n {print $2}'
}
# `|| true` on both: this runs AFTER the secrets are set, and `set -e` plus
# `pipefail` would otherwise abort the script — silently, having printed
# nothing — just because a `gh` fallback exited non-zero. Reporting is
# best-effort by design; it must never swallow the summary.
SECRETS="$(gh_secret_names || true)"
MISSING=""
for s in APPLE_ID APPLE_PASSWORD APPLE_TEAM_ID; do
  printf '%s\n' "$SECRETS" | grep -qxF "$s" || MISSING="$MISSING $s"
done
SIGNING_ON="$(gh_variable_value ENABLE_MACOS_SIGNING || true)"

say "Done — signing identity: $IDENT"
cat <<SUMMARY
Backup copy (move both into your password manager, then delete the folder —
Developer ID certs are limited per account and revoking one breaks apps you
have already shipped):
  $KEEP_DIR/developer-id-application.p12
  $KEEP_DIR/developer-id-application.password.txt
SUMMARY

if [ -n "$MISSING" ] || [ "$SIGNING_ON" != "true" ]; then
  printf '\n\033[33mStill to do before a release can sign\033[0m\n'
  for s in $MISSING; do
    case "$s" in
      APPLE_ID) printf '  %s — your Apple ID email, for notarization:\n      gh secret set APPLE_ID --repo %s\n' "$s" "$REPO" ;;
      APPLE_PASSWORD) printf '  %s — an app-specific password from appleid.apple.com (NOT your Apple ID password):\n      gh secret set APPLE_PASSWORD --repo %s\n' "$s" "$REPO" ;;
      APPLE_TEAM_ID) printf '  %s — the 10-character Team ID, %s here:\n      gh secret set APPLE_TEAM_ID --repo %s\n' "$s" "$(printf '%s' "$IDENT" | sed -n 's/.*(\([A-Z0-9]*\))$/\1/p')" "$REPO" ;;
    esac
  done
  if [ "$SIGNING_ON" != "true" ]; then
    printf '  ENABLE_MACOS_SIGNING is %s — the workflows build UNSIGNED unless it is exactly "true":\n      gh variable set ENABLE_MACOS_SIGNING --repo %s --body true\n' \
      "${SIGNING_ON:-unset}" "$REPO"
  fi
  printf '\nSee desktop/SIGNING.md section 3 for what each one is.\n'
else
  printf '\n  ok   notarization secrets present, ENABLE_MACOS_SIGNING=true\n'
  printf '\nNext: a dry run of the Flasher release.\n  gh workflow run desktop-flasher-release.yml --repo %s --ref main -f dry_run=true\n' "$REPO"
fi
