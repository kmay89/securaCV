#!/usr/bin/env bash
#
# cloudkit_schema.sh — inspect and promote the app's CloudKit schema.
#
# The away-alert path (Native/AwayPush.swift) writes a content-free
# `WitnessWake` record to the USER's own private database, and a
# CKQuerySubscription on that type is what makes Apple push the wake to
# their other devices. CloudKit auto-creates record types in DEVELOPMENT on
# first write, but production has no such magic: the type and its index must
# be deployed deliberately.
#
# That asymmetry is a trap, because the failure is silent. A production build
# with no `WitnessWake` in its schema does not error at launch, does not warn,
# and does not tell the user anything — away alerts simply never arrive. This
# script exists so "did the schema ship?" is a command with an answer instead
# of a thing someone remembers to click.
#
# The two things production needs:
#   * RECORD TYPE WitnessWake, with the `sev` field (the coarse wake class —
#     the only thing a wake is allowed to carry).
#   * A QUERYABLE index on the create-time field. AwayPush.sweepOldWakes
#     queries `creationDate < cutoff` to clear day-old wakes out of the
#     user's iCloud; without the index that query fails at runtime and the
#     litter accumulates.
#
# Usage:
#   ios/scripts/cloudkit_schema.sh check      # export dev, report what's there
#   ios/scripts/cloudkit_schema.sh promote    # verify, then deploy to production
#
# Config (env, or edit the defaults):
#   SECURACV_TEAM_ID       Apple team id — the same value CI passes as
#                          APPLE_DEVELOPMENT_TEAM. Required.
#   SECURACV_CK_CONTAINER  defaults to the container in the app entitlements.
#
# First run needs a management token, which is the one genuinely manual step:
#   xcrun cktool save-token --type management
# (Create it at icloud.developer.apple.com -> container -> Settings -> Tokens.)

set -euo pipefail

CONTAINER="${SECURACV_CK_CONTAINER:-iCloud.com.securacv.witness}"
TEAM_ID="${SECURACV_TEAM_ID:-${APPLE_DEVELOPMENT_TEAM:-}}"
RECORD_TYPE="WitnessWake"
WAKE_FIELD="sev"
SCHEMA_FILE="${TMPDIR:-/tmp}/securacv-schema.ckdb"

die() { printf '\033[0;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
ok()  { printf '\033[0;32m✓\033[0m %s\n' "$*"; }
note(){ printf '  %s\n' "$*"; }

command -v xcrun >/dev/null 2>&1 || die "xcrun not found — this needs a Mac with Xcode."
xcrun cktool --help >/dev/null 2>&1 || die "cktool unavailable — needs Xcode 14 or newer."
[ -n "$TEAM_ID" ] || die "set SECURACV_TEAM_ID (or APPLE_DEVELOPMENT_TEAM) to your Apple team id."

export_dev_schema() {
  printf 'Exporting development schema for %s…\n' "$CONTAINER"
  if ! xcrun cktool export-schema \
        --team-id "$TEAM_ID" \
        --container-id "$CONTAINER" \
        --environment development \
        --output-file "$SCHEMA_FILE" 2>/dev/null; then
    die "export failed. Usually the management token: run 'xcrun cktool save-token --type management'."
  fi
  ok "exported to $SCHEMA_FILE"
}

# The guard that matters. Importing a schema that lacks the record type is a
# silent no-op that LOOKS like success — you would come away believing away
# alerts were deployed when nothing changed. Refuse instead.
verify_schema() {
  local hard_fail="$1" missing=0

  if grep -q "RECORD TYPE ${RECORD_TYPE}" "$SCHEMA_FILE"; then
    ok "${RECORD_TYPE} is in the development schema"
  else
    printf '\033[0;31m✗\033[0m %s is NOT in the development schema\n' "$RECORD_TYPE"
    note "CloudKit creates types on first write, and nothing has written a wake yet."
    note "Fix: on a device signed into iCloud, open a development build, set an"
    note "     alert rule to \"Anywhere\" in Alerts -> Tell me when…, and run a"
    note "     test alert. Then re-run this script."
    missing=1
  fi

  # Missing `sev` is as fatal as a missing type, not a cosmetic gap.
  # AwayPush.publishWake ALWAYS sets this field, and production CloudKit
  # rejects a save carrying a field the schema doesn't define — so the write
  # fails, no record is created, no push is sent, and away alerts are dead.
  # (The extension's generic-line fallback decodes a notification that lacks
  # a class; it cannot rescue a write that never succeeded.)
  if grep -A20 "RECORD TYPE ${RECORD_TYPE}" "$SCHEMA_FILE" 2>/dev/null \
     | grep -q "\b${WAKE_FIELD}\b"; then
    ok "the ${WAKE_FIELD} field is present (the coarse wake class)"
  elif [ "$missing" -eq 0 ]; then
    printf '\033[0;31m✗\033[0m %s exists but has no %s field\n' "$RECORD_TYPE" "$WAKE_FIELD"
    note "Every wake write sets ${WAKE_FIELD}, and production rejects a save with an"
    note "undefined field — so no wake would ever be written and no push sent."
    missing=1
  fi

  # createdTimestamp is what `creationDate` maps to in the schema language.
  if grep -A20 "RECORD TYPE ${RECORD_TYPE}" "$SCHEMA_FILE" 2>/dev/null \
     | grep -qi "createdTimestamp.*QUERYABLE"; then
    ok "create-time is QUERYABLE (sweepOldWakes can clear old wakes)"
  elif [ "$missing" -eq 0 ]; then
    printf '\033[0;33m!\033[0m create-time is not marked QUERYABLE\n'
    note "Away alerts still work; the day-old sweep will fail and wakes pile up"
    note "in the user's iCloud. Add a Queryable index on createdTimestamp in the"
    note "dashboard (Schema -> Indexes -> ${RECORD_TYPE}), then re-export."
  fi

  if [ "$missing" -eq 1 ] && [ "$hard_fail" = "strict" ]; then
    die "refusing to promote: the schema is missing ${RECORD_TYPE} or its ${WAKE_FIELD} field, so away alerts would be inoperative while the deploy looked like it worked."
  fi
}

case "${1:-check}" in
  check)
    export_dev_schema
    verify_schema lenient
    printf '\nNothing was deployed. Run with "promote" to push this to production.\n'
    ;;
  promote)
    export_dev_schema
    verify_schema strict
    printf '\nDeploying to PRODUCTION (%s).\n' "$CONTAINER"
    printf 'Schema deploys are additive and cannot be undone. Continue? [y/N] '
    read -r reply
    [ "$reply" = "y" ] || [ "$reply" = "Y" ] || die "aborted."
    xcrun cktool import-schema \
      --team-id "$TEAM_ID" \
      --container-id "$CONTAINER" \
      --environment production \
      --file "$SCHEMA_FILE"
    ok "production schema updated"
    printf '\nVerify for real: install the TestFlight build on a phone signed into\n'
    printf 'iCloud, arm an "Anywhere" rule, and let a Canary go dark while that\n'
    printf 'phone is on cellular. If nothing arrives, the Alerts tab states the\n'
    printf 'actual reason rather than failing silently.\n'
    ;;
  *)
    die "usage: $(basename "$0") [check|promote]"
    ;;
esac
