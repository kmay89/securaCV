#!/usr/bin/env bash
#
# verify_pipeline.sh — operator smoke check for the live 4-container stack
# (mosquitto + Frigate + Home Assistant + SecuraCV) brought up via
# docker-compose.yml in this directory.
#
# It confirms the parts an operator can observe without secrets: that Frigate is
# publishing to MQTT and that `frigate_bridge` is ingesting events into the
# sealed log RIGHT NOW. Every check is correlated to this run — a stale retained
# MQTT message, a leftover "Event logged" line from a previous container boot,
# or a schema-only database cannot produce a false pass:
#   • the MQTT check excludes retained messages (-R);
#   • the ingest check publishes one nonce-tagged synthetic event (the same
#     payload shape ci_smoke.sh uses) and requires a log line for THAT event;
#   • the database check requires witness.db to have been written after this
#     script started, not merely to exist.
# It deliberately does NOT:
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
if [[ -n "$MQTT_USER" && -n "$MQTT_PASS" ]]; then
  mqtt_auth=(-u "$MQTT_USER" -P "$MQTT_PASS")
elif [[ -n "$MQTT_USER" ]]; then
  echo "⚠️  MQTT_USER is set but MQTT_PASS is empty; connecting without a password." >&2
  mqtt_auth=(-u "$MQTT_USER")
fi

# Correlation nonce: the ingest and database checks below must observe THIS
# run's activity, never leftovers from an earlier session.
NONCE="verifysmoke$(date +%s)$$"
MARKER="/tmp/verify_pipeline_marker"
# How long (seconds) to wait for the bridge to ingest the nonce event.
INGEST_RETRIES="${VERIFY_PIPELINE_INGEST_RETRIES:-30}"

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

# 1) Frigate publishes detections to frigate/events. Wait up to 15s for one.
#    -R drops retained messages: only a live publish within the window counts,
#    so a stale retained payload can't stand in for a running Frigate.
#    (Errors from mosquitto_sub stay visible on stderr for diagnosis.)
check_mqtt_publishes() {
  local output
  output=$("${compose_cmd[@]}" exec -T mosquitto \
    mosquitto_sub "${mqtt_auth[@]}" -t 'frigate/events' -R -C 1 -W 15 || true)
  if [[ -n "$output" ]]; then
    printf '%s\n' "$output" | head -n 1
    return 0
  fi
  return 1
}

# 2) The bridge is ingesting NOW: publish one nonce-tagged synthetic event and
#    require the bridge's "Event logged ... zone=<nonce>" line for it. Grepping
#    old logs alone could pass on a line from a previous boot while the bridge
#    is currently wedged. The camera/zone carry the nonce, so the per-bucket
#    camera+label dedup can't fold it into an earlier event either.
check_bridge_ingests_live() {
  local event
  event=$(printf '{"before":null,"after":{"id":"1719000000.%s","camera":"%s","label":"person","sub_label":null,"score":0.81,"top_score":0.92,"current_zones":["%s"],"entered_zones":["%s"],"false_positive":false,"has_clip":false,"has_snapshot":false},"type":"new"}' \
    "$NONCE" "$NONCE" "$NONCE" "$NONCE")
  "${compose_cmd[@]}" exec -T mosquitto \
    mosquitto_pub "${mqtt_auth[@]}" -t 'frigate/events' -m "$event" || return 1
  local logs i
  for ((i = 0; i < INGEST_RETRIES; i++)); do
    logs=$("${compose_cmd[@]}" logs --tail 200 --no-color securacv 2>/dev/null || true)
    if printf '%s\n' "$logs" | grep "Event logged" | grep -q "$NONCE"; then
      printf '%s\n' "$logs" | grep "Event logged" | grep "$NONCE" | tail -n 1
      return 0
    fi
    sleep 1
  done
  return 1
}

# 3) The sealed-log database was WRITTEN during this run — `test -s` alone
#    passes on a freshly initialized, event-less database. The marker file is
#    created before the ingest check, so this also cross-checks step 2's event
#    actually reached storage.
check_db_written_this_run() {
  "${compose_cmd[@]}" exec -T securacv sh -c \
    "test -s /data/witness.db && find /data/witness.db -newer $MARKER | grep -q ."
}

# Drop the freshness marker before any activity we want to attribute to this
# run. If the container is down this fails silently here and loudly in step 3.
"${compose_cmd[@]}" exec -T securacv sh -c "touch $MARKER" 2>/dev/null || true

step "Confirm MQTT publishes Frigate events (live, retained excluded)" check_mqtt_publishes
step "Confirm frigate_bridge ingests a nonce-tagged event right now" check_bridge_ingests_live
step "Confirm the sealed-log database was written during this run" check_db_written_this_run

if [[ $failures -ne 0 ]]; then
  echo "Verification failed: ${failures} step(s) did not pass." >&2
  echo "Tip: the deterministic pipeline gate runs in CI even without this live stack —" >&2
  echo "     'cargo test --test frigate_mqtt_e2e' and 'ci_smoke.sh' in this directory." >&2
  exit 1
fi

echo "All verification steps passed."
exit 0
