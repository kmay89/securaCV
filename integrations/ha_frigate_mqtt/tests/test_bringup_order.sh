#!/usr/bin/env bash
#
# Pins the ORDER of the stack's first-run commands in README.md and
# RUNBOOK.md: a command in a ```bash block must write SECURACV_MQTT_PASSWORD
# to .env before the first `docker compose` command (any subcommand but
# `version` loads the file). Compose interpolates EVERY service when it loads
# docker-compose.yml, even for `run --no-deps mosquitto`, and the frigate and
# securacv services require the variable (`${SECURACV_MQTT_PASSWORD:?...}`),
# so a doc that runs Compose first fails its operator on step one with
#   required variable SECURACV_MQTT_PASSWORD is missing a value
# Both docs shipped in that order until the 2026-09 review caught it.
#
#   bash integrations/ha_frigate_mqtt/tests/test_bringup_order.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="$HERE/.."
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# The premise. If docker-compose.yml stops requiring the variable, the reason
# for this ordering rule is gone: say so instead of passing on a stale claim.
# shellcheck disable=SC2016  # the literal `${...:?` text, not an expansion
grep -qF '${SECURACV_MQTT_PASSWORD:?' "$DIR/docker-compose.yml" \
  || fail "docker-compose.yml no longer requires SECURACV_MQTT_PASSWORD; revisit this test and the docs' step 1"

# check_order FILE -> exit 0 when .env is written first, else print why and
# exit 1. Only lines inside ```bash / ```sh fences count (indented fences
# too, as in the runbook's checklist), so prose that names a command is not
# mistaken for running it.
check_order() {
  awk '
    function is_fence(l) { return l ~ /^[[:space:]]*```/ }
    fence == 0 && is_fence($0) { fence = ($0 ~ /```(bash|sh)[[:space:]]*$/) ? 1 : 2; next }
    fence != 0 && is_fence($0) { fence = 0; next }
    fence == 1 && !env && /SECURACV_MQTT_PASSWORD=.*>>?[[:space:]]*\.env/ { env = NR }
    fence == 1 && !env && /cp[[:space:]]+\.env\.example[[:space:]]+\.env/ { env = NR }
    fence == 1 && !compose && /docker compose / && !/docker compose version/ { compose = NR }
    END {
      if (!compose) { print "no `docker compose` command found in a bash block"; exit 1 }
      if (!env) { print "no bash block writes SECURACV_MQTT_PASSWORD to .env"; exit 1 }
      if (env > compose) {
        printf "line %d runs Compose before line %d writes .env\n", compose, env
        exit 1
      }
    }
  ' "$1"
}

# The checker must refuse the order both docs used to have, or a pass below
# proves nothing.
cat > "$TMP/wrong_order.md" <<'MD'
```bash
docker compose run --rm --no-deps --entrypoint sh mosquitto -c 'true'
echo 'SECURACV_MQTT_PASSWORD=<the password>' >> .env
```
MD
if check_order "$TMP/wrong_order.md" >/dev/null; then
  fail "the checker accepted Compose before .env; it cannot guard anything"
fi
echo "ok: the checker refuses Compose-before-.env"

for doc in README.md RUNBOOK.md; do
  if ! why="$(check_order "$DIR/$doc")"; then
    fail "$doc: $why. Write SECURACV_MQTT_PASSWORD to .env before the first docker compose command."
  fi
  echo "ok: $doc writes .env before its first docker compose command"
done

echo "all bring-up order tests passed"
