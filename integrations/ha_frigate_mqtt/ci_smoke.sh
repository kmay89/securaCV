#!/usr/bin/env bash
#
# ci_smoke.sh — deterministic, Docker-free CI smoke test of the Frigate → MQTT
# ingest path. It proves the REAL `frigate_bridge` binary, connected to a live
# mosquitto broker, ingests Frigate's messages into the sealed log — in BOTH of
# its modes, one bridge run each:
#
#   • events mode (the default): subscribes to `frigate/events`, seals a
#     tracked-object message;
#   • alerts mode (`--enable-reviews`): subscribes to `frigate/reviews` INSTEAD
#     and seals an alert-severity review item. The two modes are exclusive by
#     design (a review alert is Frigate's own roll-up of the same detections),
#     so one process never subscribes to both topics.
#
# This is the automated, live-broker half of the release gate. It deliberately
# does NOT run Frigate or Home Assistant: real ML detection on a fixture is
# non-deterministic, and SecuraCV cannot control whether Frigate fires. What
# SecuraCV owns — "given a Frigate message on MQTT, the privacy-preserving claim
# lands in the sealed log" — is what this asserts.
#
# The cryptographic verification of a bridge-produced (SQLCipher-encrypted) log
# is covered separately and hermetically by `cargo test --test frigate_mqtt_e2e`,
# which runs the real `log_verify` binary. Together they cover the chain.
#
# Usage:  BRIDGE_BIN=./target/debug/frigate_bridge integrations/ha_frigate_mqtt/ci_smoke.sh
set -euo pipefail

BRIDGE_BIN="${BRIDGE_BIN:-./target/debug/frigate_bridge}"
MQTT_HOST="${MQTT_HOST:-127.0.0.1}"
MQTT_PORT="${MQTT_PORT:-1883}"

if [ ! -x "$BRIDGE_BIN" ]; then
  echo "❌ frigate_bridge binary not found at '$BRIDGE_BIN' (build it first)." >&2
  exit 1
fi
for tool in mosquitto mosquitto_pub; do
  command -v "$tool" >/dev/null 2>&1 || { echo "❌ '$tool' is required." >&2; exit 1; }
done

WORKDIR="$(mktemp -d)"
DB="$WORKDIR/witness.db"
MOSQ_CONF="$WORKDIR/mosquitto.conf"
MOSQ_PID=""
BRIDGE_PID=""

# The seed only has to be present and stable for one run; this is a throwaway DB.
export DEVICE_KEY_SEED="${DEVICE_KEY_SEED:-devkey:ci_frigate_smoke:00112233445566778899aabbccddeeff}"

cleanup() {
  [ -n "$BRIDGE_PID" ] && kill "$BRIDGE_PID" 2>/dev/null || true
  [ -n "$MOSQ_PID" ] && kill "$MOSQ_PID" 2>/dev/null || true
  rm -rf "$WORKDIR"
}
trap cleanup EXIT

# 1) Local, anonymous mosquitto bound to loopback.
cat >"$MOSQ_CONF" <<EOF
listener ${MQTT_PORT} ${MQTT_HOST}
allow_anonymous true
EOF
mosquitto -c "$MOSQ_CONF" &
MOSQ_PID=$!

# Wait until BRIDGE_LOG contains $1 (or the bridge exits); $2 names the wait.
wait_for_line() {
  local needle="$1" what="$2" seen=0
  for _ in $(seq 1 60); do
    if grep -q -- "$needle" "$BRIDGE_LOG"; then seen=1; break; fi
    if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then break; fi
    sleep 0.5
  done
  if [ "$seen" -ne 1 ]; then
    echo "❌ frigate_bridge $what." >&2
    cat "$BRIDGE_LOG" >&2
    exit 1
  fi
}

# One bridge run: start the real binary with the given mode flags, wait for its
# "Subscribed to <topic>" line, publish one message (retained, so a reconnect
# can't drop it), and require the "Event logged" line that is printed iff
# parse → sanitize → append_event_checked all succeeded — i.e. the MQTT-wired
# binary produced a sealed event. Then stop the bridge so the next mode starts
# from a quiet broker. Loopback broker ⇒ no --allow-remote-mqtt needed; the
# bridge has its own connect/subscribe retry loop, so a slow broker is fine.
run_mode() {
  local label="$1" topic="$2" payload="$3"
  shift 3
  BRIDGE_LOG="$WORKDIR/bridge-${label}.log"
  RUST_LOG=info "$BRIDGE_BIN" \
    --db-path "$DB" \
    --mqtt-broker-addr "${MQTT_HOST}:${MQTT_PORT}" \
    "$@" \
    >"$BRIDGE_LOG" 2>&1 &
  BRIDGE_PID=$!

  wait_for_line "Subscribed to ${topic}" "never subscribed to ${topic} in ${label} mode"
  mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" -t "$topic" -r -m "$payload"
  wait_for_line "Event logged" "did not ingest the ${topic} message in ${label} mode"

  kill "$BRIDGE_PID" 2>/dev/null || true
  wait "$BRIDGE_PID" 2>/dev/null || true
  BRIDGE_PID=""
  grep "Event logged" "$BRIDGE_LOG"
}

# 2) Events mode: a realistic person detection, as Frigate publishes it (the
#    `sub_label` pair, the ids, the clip/snapshot flags are all discarded at
#    parse; only the claim's fields reach the log).
EVENT='{"before":null,"after":{"id":"1719000000.ci-object-id","camera":"front_door","label":"person","sub_label":["Someone Named","0.79"],"score":0.81,"top_score":0.92,"current_zones":["porch"],"entered_zones":["driveway","porch"],"false_positive":false,"has_clip":true,"has_snapshot":true},"type":"new"}'
EVENT="${EVENT//\"0.79\"/0.79}"
run_mode events frigate/events "$EVENT"

if [ ! -s "$DB" ]; then
  echo "❌ sealed log database was not created at $DB." >&2
  exit 1
fi

# 3) Alerts mode: a real (Frigate 0.14+) frigate/reviews item that is already an
#    alert at `new`, on a different camera so the camera+bucket dedup cannot fold
#    it into step 2.
REVIEW='{"type":"new","before":{"id":"1719000001.5-rev1","camera":"garage","severity":"detection","data":{"detections":["1719000001.4-obj1"],"objects":["person"],"sub_labels":[],"zones":[],"audio":[]}},"after":{"id":"1719000001.5-rev1","camera":"garage","severity":"alert","data":{"detections":["1719000001.4-obj1"],"objects":["person"],"sub_labels":[],"zones":["garage_zone"],"audio":[]}}}'
run_mode alerts frigate/reviews "$REVIEW" --enable-reviews

echo "✅ frigate_bridge ingested a frigate/events message (events mode) and a frigate/reviews alert (alerts mode) into a sealed log."
