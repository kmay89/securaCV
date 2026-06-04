#!/usr/bin/env bash
#
# verify_pipeline.sh — operator smoke check for the live 4-container stack
# (mosquitto + Frigate + Home Assistant + SecuraCV) brought up via
# docker-compose.yml in this directory.
#
# It confirms the parts an operator can observe without secrets: that Frigate is
# publishing to MQTT and that `frigate_bridge` is ingesting those events into the
# sealed log. It deliberately does NOT:
#   • read witness.db directly — the kernel stores it SQLCipher-encrypted, so a
#     plain sqlite3 query cannot open it;
#   • expect vault envelopes — `frigate_bridge` sign-seals the append-only log, it
#     does not vault-seal frames;
#   • produce an export bundle — that requires a break-glass quorum token, which
#     is out of scope for a smoke check.
#
# The deterministic, automated gate for the SecuraCV-owned pipeline lives in CI:
#   • `cargo test --test frigate_mqtt_e2e` — a Frigate event → sealed log → real
#     `log_verify` (against the encrypted DB), no Docker required; and
#   • `integrations/ha_frigate_mqtt/ci_smoke.sh` — the real `frigate_bridge`
#     binary ingesting a `frigate/events` message from a live mosquitto broker.
#
# If your broker requires authentication (mosquitto.conf disables anonymous
# access), export MQTT_USER / MQTT_PASS before running.
set -euo pipefail

failures=0

compose_cmd=(docker compose)
if ! docker compose version >/dev/null 2>&1; then
  if command -v docker-compose >/dev/null 2>&1; then
    compose_cmd=(docker-compose)
  else
    echo "❌ docker compose is required but not available." >&2
    exit 1
  fi
fi

MQTT_USER="${MQTT_USER:-}"
MQTT_PASS="${MQTT_PASS:-}"
mqtt_auth=()
if [[ -n "$MQTT_USER" ]]; then
  mqtt_auth=(-u "$MQTT_USER" -P "$MQTT_PASS")
fi

step() {
  local name=$1
  shift
  echo "==> ${name}"
  if "$@"; then
    echo "✅ ${name}"
  else
    echo "❌ ${name}" >&2
    failures=$((failures + 1))
  fi
  echo
}

# Frigate publishes detections to frigate/events. Wait up to 15s for one.
check_mqtt_publishes() {
  local output
  output=$("${compose_cmd[@]}" exec -T mosquitto \
    sh -c "mosquitto_sub ${mqtt_auth[*]} -t 'frigate/events' -C 1 -W 15" 2>/dev/null || true)
  if [[ -n "$output" ]]; then
    printf '%s\n' "$output" | head -n 1
    return 0
  fi
  return 1
}

# frigate_bridge logs "Event logged: ..." on every successful append to the
# sealed log. This works regardless of DB encryption and needs no secrets.
check_bridge_ingests() {
  local logs
  logs=$("${compose_cmd[@]}" logs --no-color securacv 2>/dev/null || true)
  if printf '%s\n' "$logs" | grep -q "Event logged"; then
    printf '%s\n' "$logs" | grep "Event logged" | tail -n 1
    return 0
  fi
  return 1
}

# The encrypted sealed-log DB exists and is non-empty.
check_db_exists() {
  "${compose_cmd[@]}" exec -T securacv sh -c "test -s /data/witness.db"
}

step "Confirm MQTT publishes Frigate events" check_mqtt_publishes
step "Confirm frigate_bridge ingests at least one event" check_bridge_ingests
step "Confirm the sealed-log database exists" check_db_exists

if [[ $failures -ne 0 ]]; then
  echo "Verification failed: ${failures} step(s) did not pass." >&2
  echo "Tip: the deterministic pipeline gate runs in CI even without this live stack —" >&2
  echo "     'cargo test --test frigate_mqtt_e2e' and 'ci_smoke.sh' in this directory." >&2
  exit 1
fi

echo "All verification steps passed."
exit 0
