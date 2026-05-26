#!/usr/bin/env bash
set -euo pipefail
# ═══════════════════════════════════════════════════════════════════════
# Microcopy lint orchestrator
# ═══════════════════════════════════════════════════════════════════════
#
# Four sub-checks that gate the device's user-facing copy:
#
#   1. Plain-words audit       — banned technical jargon (CSI, RSSI,
#      (headline dashboard)      NVS, ...) must not appear in the COPY
#                                object or HTML body of /. Calibrated
#                                for grandma / kids / parents.
#
#   2. Plain-words audit       — narrow internal-jargon list (NVS,
#      (legacy admin)            FreeRTOS, esp_err_t, TODO, ...) for
#                                /admin's HTML body. Power-user terms
#                                like RSSI / threshold / endpoint are
#                                allowed since admin's audience expects
#                                them; this check only catches terms
#                                that no human-facing UI should show.
#
#   3. Tooltip coverage        — every data-tip="key" attribute on the
#                                headline dashboard resolves to a defined
#                                entry in COPY.tooltips. Catches keys
#                                that get renamed without updating the
#                                bank (silent empty tooltips).
#
#   4. Reading-grade FKGL      — aggregate Flesch-Kincaid across the
#      (headline dashboard)      headline COPY corpus stays at or below
#                                7th grade. The plan's target is 6th;
#                                one grade of slack absorbs noisy short
#                                strings.
#
# Each check is independent. A failure in any one fails the whole job.
# Failures print a clear message describing what to fix.
#
# Usage:   firmware/scripts/microcopy_lint.sh
# Locally: SKIP_FKGL=1 firmware/scripts/microcopy_lint.sh   # if no python
#


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DASH_HTML="$(cd "$SCRIPT_DIR/../.." && pwd)/firmware/projects/canary-wap/arduino/canary_wap/csi_dashboard_html.h"
ADMIN_HTML="$(cd "$SCRIPT_DIR/../.." && pwd)/firmware/projects/canary-wap/arduino/canary_wap/web_ui.h"
BANNED_TERMS="$SCRIPT_DIR/microcopy_banned_terms.txt"
BANNED_TERMS_ADMIN="$SCRIPT_DIR/microcopy_banned_terms_admin.txt"

red()    { echo -e "\033[0;31m✗ $1\033[0m"; }
green()  { echo -e "\033[0;32m✓ $1\033[0m"; }
yellow() { echo -e "\033[0;33m⚠ $1\033[0m"; }

if [ ! -f "$DASH_HTML" ]; then
  red "Dashboard not found: $DASH_HTML"
  exit 2
fi
if [ ! -f "$BANNED_TERMS" ]; then
  red "Banned-terms list not found: $BANNED_TERMS"
  exit 2
fi
if [ ! -f "$ADMIN_HTML" ]; then
  red "Admin HTML not found: $ADMIN_HTML"
  exit 2
fi
if [ ! -f "$BANNED_TERMS_ADMIN" ]; then
  red "Admin banned-terms list not found: $BANNED_TERMS_ADMIN"
  exit 2
fi

echo "═══════════════════════════════════════════════════════════════════"
echo "  Microcopy lint"
echo "═══════════════════════════════════════════════════════════════════"

ERRORS=0

# ─────────────────────────────────────────────────────────────────────────
# 1. Plain-words audit (banned-term grep against COPY block + HTML body)
# ─────────────────────────────────────────────────────────────────────────

echo ""
echo "── Plain-words audit ──"

# Extract user-facing regions only:
#   - The COPY = { ... } object literal
#   - The <body> ... <script> region (HTML body, no JS)
# Code/comments outside these legitimately use technical terms.
#
# The body/script section markers are line-anchored on purpose. The
# previous unanchored patterns (/<body>/ and /<script>/) matched ANY
# line containing the literal token, so a JS comment like
# "...disconnect class on <body> dims the orb..." inside the COPY
# block flipped in_body=1 long after the real <script> open tag had
# already passed — leaving the awk in "scan everything until EOF"
# mode and tripping the banned-term grep on legitimate JS code
# (csi/preset/etc.). Three contributors (PR #399, PR #401, PR #402)
# hit this in the same session before the gotcha became obvious.
#
# The patterns are deliberately broader than ^<body>$ — they accept:
#   - attribute variants  : <body class="...">, <script type="module">
#   - trailing whitespace : <body>␣␣ or <body>\r (CRLF endings)
# while still requiring the tag to start at column 1. Comments like
# " * disconnect class on <body> dims the orb." don't match because
# they begin with whitespace + asterisk, not the literal tag.
# (PR #404 reviews r3214602477, r3214602482.)
USER_FACING=$(awk '
  /^const COPY = \{/                  { in_copy=1 }
  in_copy                             { print }
  in_copy && /^};/                    { in_copy=0 }
  /^<body[^>]*>[[:space:]]*$/         { in_body=1; next }
  /^<script[^>]*>[[:space:]]*$/       { in_body=0 }
  in_body                             { print }
' "$DASH_HTML")

# Grep banned terms (whole-word, case-insensitive, fixed-string).
#   -F (fixed strings) avoids treating term metacharacters like `[` as
#       regex (PR #408 review r3214719890). Without -F, a future term
#       like "[object Object]" would be parsed as a character class
#       and silently match almost anything.
#   `^[[:space:]]*$` filters blank-or-whitespace lines so a stray
#       indented blank doesn't pass through to grep as an empty
#       pattern (which would match every input line).
#   sed strips leading + trailing whitespace so a contributor who
#       indents an entry can't break -w word-boundary matching.
#   `|| true` because grep returns 1 on no matches; we want the
#       inverse.
HITS=$(echo "$USER_FACING" \
       | grep -wiFf <(grep -v '^#' "$BANNED_TERMS" \
                      | grep -v '^[[:space:]]*$' \
                      | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//') \
       || true)

if [ -n "$HITS" ]; then
  red "Plain-words audit FAILED — banned technical jargon found in user-facing copy:"
  echo "$HITS" | head -20
  echo ""
  echo "These terms read as gibberish to grandma / kids / parents."
  echo "Suggested replacements live at the top of $BANNED_TERMS."
  ERRORS=$((ERRORS + 1))
else
  green "Plain-words audit OK — no banned jargon in COPY or HTML body."
fi

# ─────────────────────────────────────────────────────────────────────────
# 2. Plain-words audit — legacy /admin (web_ui.h)
# ─────────────────────────────────────────────────────────────────────────
#
# Same shape as the headline pass but against a narrower banned-terms
# list (microcopy_banned_terms_admin.txt) calibrated for admin's
# power-user audience: RSSI / threshold / endpoint are allowed,
# only truly internal jargon (NVS, FreeRTOS, esp_err_t, TODO, ...)
# is caught.
#
# web_ui.h's <script> tag is indented two spaces (the convention used
# in that PROGMEM raw-string), unlike csi_dashboard_html.h's column-1
# placement. The awk pattern accepts optional leading whitespace on
# the section markers so both files work without per-file logic.
# Comments like " * <body> tag handling" still don't match because
# the leading-whitespace clause is followed by the strict
# `<body[^>]*>[[:space:]]*$` shape — text after the `>` rules them
# out.

echo ""
echo "── Plain-words audit (admin) ──"

ADMIN_BODY=$(awk '
  /^[[:space:]]*<body[^>]*>[[:space:]]*$/   { in_body=1; next }
  /^[[:space:]]*<script[^>]*>[[:space:]]*$/ { in_body=0 }
  in_body                                   { print }
' "$ADMIN_HTML")

ADMIN_HITS=$(echo "$ADMIN_BODY" \
       | grep -wiFf <(grep -v '^#' "$BANNED_TERMS_ADMIN" \
                      | grep -v '^[[:space:]]*$' \
                      | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//') \
       || true)

if [ -n "$ADMIN_HITS" ]; then
  red "Admin plain-words audit FAILED — internal jargon found in admin HTML body:"
  echo "$ADMIN_HITS" | head -20
  echo ""
  echo "These terms are codebase-internal and have no business in any UI."
  echo "Suggested replacements live at the top of $BANNED_TERMS_ADMIN."
  ERRORS=$((ERRORS + 1))
else
  green "Admin plain-words audit OK — no internal jargon in /admin HTML body."
fi

# ─────────────────────────────────────────────────────────────────────────
# 3. Tooltip coverage
# ─────────────────────────────────────────────────────────────────────────

echo ""
echo "── Tooltip coverage ──"

if python3 "$SCRIPT_DIR/microcopy_tooltip_coverage.py" "$DASH_HTML"; then
  green "Tooltip coverage OK — every data-tip key resolves."
else
  ERRORS=$((ERRORS + 1))
fi

# ─────────────────────────────────────────────────────────────────────────
# 4. Reading-grade FKGL (skippable if python3 missing locally)
# ─────────────────────────────────────────────────────────────────────────

echo ""
echo "── Reading-grade Flesch-Kincaid ──"

if [ "${SKIP_FKGL:-0}" = "1" ]; then
  yellow "FKGL skipped (SKIP_FKGL=1)."
elif ! command -v python3 >/dev/null 2>&1; then
  yellow "python3 not available — FKGL skipped. CI will run it."
else
  if python3 "$SCRIPT_DIR/microcopy_fkgl.py" "$DASH_HTML"; then
    green "FKGL OK."
  else
    ERRORS=$((ERRORS + 1))
  fi
fi

# ─────────────────────────────────────────────────────────────────────────

echo ""
if [ "$ERRORS" -eq 0 ]; then
  echo "═══════════════════════════════════════════════════════════════════"
  green "Microcopy lint passed."
  echo "═══════════════════════════════════════════════════════════════════"
  exit 0
fi

echo "═══════════════════════════════════════════════════════════════════"
red "Microcopy lint FAILED — $ERRORS check(s) reported issues."
echo "═══════════════════════════════════════════════════════════════════"
exit 1
