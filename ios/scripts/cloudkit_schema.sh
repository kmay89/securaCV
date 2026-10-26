#!/usr/bin/env bash
#
# cloudkit_schema.sh — inspect and promote the app's CloudKit schema.
#
# WHAT LIVES IN THE CONTAINER
#   Everything SecuraCV keeps in iCloud is in ONE container, in the USER's own
#   PRIVATE database. SecuraCV runs no server and cannot read any of it
#   (Invariant IV, realized as infrastructure rather than promised in copy).
#   Three record types, and they are the whole list:
#
#     * WitnessWake  — the content-free away-alert wake. A CKQuerySubscription
#                      on this type is what makes Apple push a wake to the
#                      user's other devices (Native/AwayPush.swift).
#     * EscalationWake — the same content-free wake, in the ONE shared zone
#                      (HouseholdEscalations). Written only when a top-tier
#                      alarm went unanswered, so somebody the owner invited is
#                      told (Cloud/HouseholdShare.swift). It is the only thing
#                      in this container another person can read, and the zone
#                      is the boundary: docs/design/cloudkit_backend.md §6.5.
#     * PairedDevice — the fleet list, so a second iPhone or an iPad "just has
#                      your fleet" (Cloud/CloudSync.swift).
#
#   Why they are here at all, what will never join them, and the trade-offs
#   taken with eyes open: docs/design/cloudkit_backend.md.
#
# WHY THIS SCRIPT EXISTS
#   CloudKit auto-creates record types and indexes in DEVELOPMENT on first
#   write. Production has no such magic: types and indexes must be deployed
#   deliberately.
#
#   That asymmetry is a trap, because the failure is silent in BOTH directions.
#   Production rejects a save carrying a field the schema doesn't define, and
#   rejects a query against a field with no queryable index — and both call
#   sites in this app deliberately swallow their errors, because a failed sync
#   must never stall the local alert already reaching the person in the room.
#   So a half-deployed schema does not error at launch, does not warn, and does
#   not tell the user anything. The feature simply never works.
#
#   This script exists so "did the schema ship?" is a command with an answer
#   instead of a thing someone remembers to click.
#
# WHY `check` READS BOTH ENVIRONMENTS
#   The first `check` exported DEVELOPMENT only — the one environment that
#   fills in a field the app writes by itself, because CloudKit mints it on the
#   first save. So it went green while the open question was PRODUCTION, which
#   every TestFlight and App Store build talks to: PairedDevice.tlsCertFP was
#   first written in ios-v0.6.0, and nothing on record says it was ever
#   promoted. A gate that reads only the environment that fills itself in
#   answers "did the schema ship?" with yes every time. So `check` now holds
#   BOTH exports to the same table and names the environment that is short:
#   development says whether there is anything to promote, production whether
#   it was promoted.
#
# THE TABLE IS THE POINT
#   `requirements` below is the list of everything production needs. Adding a
#   CKRecord type to the app and forgetting to add it here is the exact drift
#   this script was written to prevent — and it happened: the first version of
#   this file knew only about WitnessWake, while CloudSync had been writing and
#   querying PairedDevice the whole time. So the table is now cross-checked
#   against the Swift sources by scripts/lint_cloudkit_container.py, which runs
#   in CI on every push. Edit the table; the checks below read it.
#
# Usage:
#   ios/scripts/cloudkit_schema.sh check      # export dev AND prod, report what each lacks
#   ios/scripts/cloudkit_schema.sh promote    # verify dev, then deploy it to production
#
#   `check` exits non-zero when something required is missing from EITHER
#   environment, so it can gate a release step rather than only inform a human
#   reading the scrollback. A gap only production has is fixed by `promote`.
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
SCHEMA_FILE="${TMPDIR:-/tmp}/securacv-schema.ckdb"
# Read-only: `check` compares it, nothing ever imports it. `promote` deploys
# the DEVELOPMENT export above, never this one.
PROD_SCHEMA_FILE="${TMPDIR:-/tmp}/securacv-schema.production.ckdb"

die() { printf '\033[0;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
ok()  { printf '\033[0;32m✓\033[0m %s\n' "$*"; }
bad() { printf '\033[0;31m✗\033[0m %s\n' "$*"; }
warn(){ printf '\033[0;33m!\033[0m %s\n' "$*"; }
note(){ printf '    %s\n' "$*"; }

# --- what production must have ----------------------------------------------
#
# One row per record type:
#
#   type | fields every write sets | indexes the app's queries REQUIRE | advised
#
# `-` means "none". A REQUIRED index is one whose absence kills the feature; an
# ADVISED one degrades it without breaking it, so it warns instead of failing.
# Field names are spelled exactly as the app writes them, because that is what
# production compares against.
requirements() {
  cat <<'EOF'
WitnessWake|sev|-|createdTimestamp
EscalationWake|sev|-|createdTimestamp
AlertAnswered|-|createdTimestamp|-
PairedDevice|name deviceType baseURL pairedAt tlsCertFP|recordName|-
EOF
}

# The user-visible symptom of a missing type. A gate that says "record type not
# found" makes someone go read Swift; a gate that names what the user loses
# makes the priority obvious from the terminal.
why_type() {
  case "$1" in
    WitnessWake)
      echo "away alerts never arrive — the wake write is rejected, so no push is ever sent" ;;
    EscalationWake)
      echo "nobody else is ever told — an unanswered alarm reaches the owner's devices and stops there" ;;
    AlertAnswered)
      echo "a household member is woken about an alarm the owner already answered on another device: acknowledging is device-local, and this marker is how the owner's iPhone tells their iPad to stand down before its escalation timer fires" ;;
    PairedDevice)
      echo "the fleet never appears on a second iPhone or iPad — the sync read comes back empty, forever" ;;
    *)
      echo "the feature behind this record type silently does nothing" ;;
  esac
}

why_index() {
  case "$1.$2" in
    WitnessWake.createdTimestamp)
      echo "AwayPush.sweepOldWakes queries 'creationDate < cutoff'; without the index the query fails and spent wakes accumulate in the user's iCloud" ;;
    EscalationWake.createdTimestamp)
      echo "HouseholdShare.sweepOldEscalations queries 'creationDate < cutoff'; without the index the query fails and spent escalations pile up in the shared zone the household can read" ;;
    AlertAnswered.createdTimestamp)
      echo "HouseholdShare.sweepOldAnswered queries 'creationDate < cutoff'; without the index the query fails and the answered markers accumulate forever — each one carrying a precise CloudKit creation time, which turns a suppression aid into a timestamped record of when the owner was awake" ;;
    PairedDevice.recordName)
      echo "CloudSync.pull() queries every PairedDevice with a match-all predicate, which production refuses without a queryable index; the error is swallowed, so fleet sync reads empty and looks like 'you have no devices'" ;;
    *)
      echo "a query the app depends on fails at runtime" ;;
  esac
}

# How a missing type gets created in development, so the fix is a paragraph and
# not a research project. CloudKit only mints a type when something writes one.
how_to_seed() {
  case "$1" in
    WitnessWake)
      note "Fix: on a device signed into iCloud, open a development build, set an"
      note "     alert rule to \"Anywhere\" in Alerts -> Tell me when…, and run a"
      note "     test alert. Then re-run this script." ;;
    EscalationWake)
      note "Fix: on a device signed into iCloud, open a development build, go to"
      note "     Alerts -> Tell me when… -> If nobody answers, and invite someone"
      note "     (a second Apple account you control is enough). Let a tamper"
      note "     alert go unacknowledged past the escalation window so one record"
      note "     is written. Then re-run this script." ;;
    PairedDevice)
      note "Fix: on a device signed into iCloud, open a development build and pair"
      note "     one Canary. The first pair writes the record type. Then re-run"
      note "     this script." ;;
    *)
      note "Fix: exercise the feature once in a development build signed into"
      note "     iCloud so CloudKit mints the type, then re-run this script." ;;
  esac
}

command -v xcrun >/dev/null 2>&1 || die "xcrun not found — this needs a Mac with Xcode."
xcrun cktool --help >/dev/null 2>&1 || die "cktool unavailable — needs Xcode 14 or newer."
[ -n "$TEAM_ID" ] || die "set SECURACV_TEAM_ID (or APPLE_DEVELOPMENT_TEAM) to your Apple team id."

# export_schema <development|production> <file>
export_schema() {
  local env="$1" file="$2"
  printf 'Exporting %s schema for %s…\n' "$env" "$CONTAINER"
  if ! xcrun cktool export-schema \
        --team-id "$TEAM_ID" \
        --container-id "$CONTAINER" \
        --environment "$env" \
        --output-file "$file" 2>/dev/null; then
    die "$env export failed. Usually the management token: run 'xcrun cktool save-token --type management'."
  fi
  ok "exported ${env} to $file"
}

# Production mints nothing on write: a type, field or index is there only if a
# deploy put it there. So every production gap has the same fix.
promote_fix() {
  note "Fix: run '$(basename "$0") promote' — it re-checks development and"
  note "     deploys it to production. If development lacks it too, fix that first."
  note "     If cktool refuses the production import, deploy the same development"
  note "     schema from CloudKit Console -> CloudKit Database -> Deploy Schema"
  note "     Changes (Apple's documented route to production), then re-run check."
}

# The whole `RECORD TYPE X ( … );` stanza of one exported schema file, or nothing.
#
# The first version of this file read a fixed `grep -A20` window after the
# header line. That silently under-reads: CloudKit's exported schema lists its
# own system fields alongside yours, so PairedDevice's six fields plus the
# GRANT lines run well past twenty — and a check that looks in the wrong window
# reports "field missing" for a field that is right there, or worse, misses a
# real gap because the window ended early. Read to the closing paren instead.
record_block() {
  awk -v t="$1" '
    $0 ~ "RECORD TYPE[[:space:]]+" t "[[:space:]]*\\(" { inblock = 1 }
    inblock { print }
    inblock && /\);[[:space:]]*$/ { exit }
  ' "$2"
}

# A field name, matched on word boundaries so `name` does not match `recordName`
# and `sev` does not match `severity`. `grep -w` is the portable spelling of
# that on both BSD and GNU grep.
block_has_field() {
  printf '%s\n' "$1" | grep -qw -- "$2"
}

# The exported schema spells an index as an attribute on the field line, so the
# test is "one line carries both the field and the word QUERYABLE."
#
# The table names the two SYSTEM-field indexes the way the dashboard does
# (recordName, createdTimestamp), but the schema language an export is written
# in spells those fields `"___recordID" REFERENCE QUERYABLE` and
# `"___createTime" TIMESTAMP QUERYABLE` (Apple, "Integrating a Text-Based
# Schema into Your Workflow"). Matching only the dashboard name would report
# both indexes missing from any export written that way, in both environments,
# so `check` could not go green and `promote` would always refuse. Accept
# either spelling; neither can match a different field.
schema_field_name() {
  case "$1" in
    recordName)       echo "___recordID" ;;
    createdTimestamp) echo "___createTime" ;;
    *)                echo "$1" ;;
  esac
}

block_has_queryable() {
  printf '%s\n' "$1" | grep -w -e "$2" -e "$(schema_field_name "$2")" | grep -qi "QUERYABLE"
}

missing=0   # something REQUIRED is absent — promoting would ship a dead feature
degraded=0  # only advised indexes are absent — the feature works, imperfectly

# Where an index gap is fixed. In development, the dashboard; in production,
# a promote, which carries development's indexes along with its types and fields.
index_fix() {
  local env="$1" type="$2" idx="$3"
  if [ "$env" = "production" ]; then
    note "A promote carries development's indexes along with its types and fields."
    promote_fix
  else
    note "Add it in the dashboard: Schema -> Indexes -> $type -> $idx (Queryable),"
    note "then re-export."
  fi
}

# verify_schema <strict|lenient> <development|production> <file>
# Resets the counters, so after each call they describe that environment alone.
verify_schema() {
  local hard_fail="$1" env="$2" file="$3"
  local type fields req_idx adv_idx block field idx
  missing=0
  degraded=0

  printf '\n── %s ──\n' "$env"
  while IFS='|' read -r type fields req_idx adv_idx; do
    [ -n "$type" ] || continue
    printf '\n%s\n' "$type"

    block="$(record_block "$type" "$file")"
    if [ -z "$block" ]; then
      bad "$type is NOT in the $env schema"
      note "Without it: $(why_type "$type")."
      if [ "$env" = "production" ]; then
        note "Production never creates a type on write; it has only what was promoted."
        promote_fix
      else
        note "CloudKit creates types on first write, and nothing has written one yet."
        how_to_seed "$type"
      fi
      missing=1
      continue
    fi
    ok "$type is in the $env schema"

    # A missing field is as fatal as a missing type, not a cosmetic gap.
    # Production rejects a save carrying a field the schema doesn't define, so
    # the write fails, no record is created, and the feature is dead.
    for field in $fields; do
      [ "$field" = "-" ] && continue
      if block_has_field "$block" "$field"; then
        ok "  field $field"
      else
        bad "  $type exists in $env but has no $field field"
        note "Every write sets it, and production rejects a save with an undefined"
        note "field — so nothing is ever written. Effect: $(why_type "$type")."
        [ "$env" = "production" ] && promote_fix
        missing=1
      fi
    done

    for idx in $req_idx; do
      [ "$idx" = "-" ] && continue
      if block_has_queryable "$block" "$idx"; then
        ok "  $idx is QUERYABLE"
      else
        bad "  $idx is not QUERYABLE in $env (required)"
        note "$(why_index "$type" "$idx")."
        index_fix "$env" "$type" "$idx"
        missing=1
      fi
    done

    for idx in $adv_idx; do
      [ "$idx" = "-" ] && continue
      if block_has_queryable "$block" "$idx"; then
        ok "  $idx is QUERYABLE"
      else
        warn "  $idx is not QUERYABLE in $env (advised)"
        note "$(why_index "$type" "$idx")."
        index_fix "$env" "$type" "$idx"
        degraded=1
      fi
    done
  done <<EOF
$(requirements)
EOF

  printf '\n'
  if [ "$missing" -eq 1 ] && [ "$env" = "production" ]; then
    bad "the production schema is incomplete — at least one feature is inoperative in every TestFlight and App Store build"
  elif [ "$missing" -eq 1 ]; then
    bad "the $env schema is incomplete — at least one feature would be inoperative in production"
  elif [ "$degraded" -eq 1 ]; then
    warn "everything works in $env, but something advised is missing (see above)"
  else
    ok "every record type, field, and index this app depends on is in the $env schema"
  fi

  if [ "$missing" -eq 1 ] && [ "$hard_fail" = "strict" ]; then
    die "refusing to promote: deploying this would look like it worked while the feature stayed dead. Fix the gaps above first."
  fi
}

case "${1:-check}" in
  check)
    export_schema development "$SCHEMA_FILE"
    export_schema production "$PROD_SCHEMA_FILE"
    verify_schema lenient development "$SCHEMA_FILE"
    dev_missing="$missing"
    verify_schema lenient production "$PROD_SCHEMA_FILE"
    prod_missing="$missing"

    printf '\n'
    if [ "$prod_missing" -eq 1 ] && [ "$dev_missing" -eq 0 ]; then
      bad "PRODUCTION lacks what the app writes or queries (named above); development has all of it."
      note "Fix: run '$(basename "$0") promote'."
    elif [ "$prod_missing" -eq 1 ]; then
      bad "PRODUCTION and DEVELOPMENT both lack what the app writes or queries (named above)."
      note "Fix development first (the steps above), then run '$(basename "$0") promote'."
    elif [ "$dev_missing" -eq 1 ]; then
      bad "DEVELOPMENT lacks what the app writes or queries (named above); production has it."
      note "Shipped builds are unaffected, but 'promote' refuses until development is fixed."
    else
      ok "both environments carry everything this app writes and queries"
    fi
    printf '\nNothing was deployed. Run with "promote" to push development to production.\n'
    [ "$dev_missing" -eq 0 ] && [ "$prod_missing" -eq 0 ] || exit 1
    ;;
  promote)
    export_schema development "$SCHEMA_FILE"
    verify_schema strict development "$SCHEMA_FILE"
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
    printf '\nThen run "%s check": it reads production back, so green there means the\n' "$(basename "$0")"
    printf 'deploy landed. That proves the schema, not the feature.\n'
    printf '\nVerify for real, both halves:\n'
    printf '  * Away alerts — install the TestFlight build on a phone signed into\n'
    printf '    iCloud, arm an "Anywhere" rule, and let a Canary go dark while that\n'
    printf '    phone is on cellular. If nothing arrives, the Alerts tab states the\n'
    printf '    actual reason rather than failing silently.\n'
    printf '  * Fleet sync — sign a second device into the same iCloud account and\n'
    printf '    confirm the paired Canaries appear without pairing them again.\n'
    ;;
  *)
    die "usage: $(basename "$0") [check|promote]"
    ;;
esac
