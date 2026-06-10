#!/usr/bin/env bash
#
# ci_smoke.sh — deterministic, Docker-free CI smoke test of the Frigate → MQTT
# ingest path. It proves the REAL `frigate_bridge` binary, connected to a live
# mosquitto broker, ingests a `frigate/events` message into the sealed log.
#
# This is the automated, live-broker half of the release gate. It deliberately
# does NOT run Frigate or Home Assistant: real ML detection on a fixture is
# non-deterministic, and SecuraCV cannot control whether Frigate fires. What
# SecuraCV owns — "given a Frigate event on MQTT, the privacy-preserving claim
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
BRIDGE_LOG="$WORKDIR/bridge.log"
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

# 2) Start the real bridge. Loopback broker ⇒ no --allow-remote-mqtt needed.
#    The bridge has its own connect/subscribe retry loop, so a slightly slow
#    broker is fine; we wait for its "Subscribed" line before publishing.
RUST_LOG=info "$BRIDGE_BIN" \
  --db-path "$DB" \
  --mqtt-broker-addr "${MQTT_HOST}:${MQTT_PORT}" \
  --enable-reviews \
  >"$BRIDGE_LOG" 2>&1 &
BRIDGE_PID=$!

subscribed=0
for _ in $(seq 1 60); do
  if grep -q "Subscribed to frigate/events" "$BRIDGE_LOG"; then subscribed=1; break; fi
  if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then break; fi
  sleep 0.5
done
if [ "$subscribed" -ne 1 ]; then
  echo "❌ frigate_bridge never subscribed to the broker." >&2
  cat "$BRIDGE_LOG" >&2
  exit 1
fi

# 3) Publish a realistic person detection (retained, so a reconnect can't drop it).
EVENT='{"before":null,"after":{"id":"1719000000.ci-object-id","camera":"front_door","label":"person","sub_label":null,"score":0.81,"top_score":0.92,"current_zones":["porch"],"entered_zones":["driveway","porch"],"false_positive":false,"has_clip":true,"has_snapshot":true},"type":"new"}'
mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" -t frigate/events -r -m "$EVENT"

# 4) The bridge logs "Event logged" iff parse → sanitize → append_event_checked
#    all succeeded — i.e. the MQTT-wired binary produced a sealed event.
ingested=0
for _ in $(seq 1 60); do
  if grep -q "Event logged" "$BRIDGE_LOG"; then ingested=1; break; fi
  if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then break; fi
  sleep 0.5
done
if [ "$ingested" -ne 1 ]; then
  echo "❌ frigate_bridge did not ingest the frigate/events message." >&2
  cat "$BRIDGE_LOG" >&2
  exit 1
fi

if [ ! -s "$DB" ]; then
  echo "❌ sealed log database was not created at $DB." >&2
  exit 1
fi

# 5) A real (Frigate 0.14+) frigate/reviews payload must also land. Different
#    camera, so the camera+label-per-bucket dedup can't fold it into step 3.
REVIEW='{"type":"new","before":{"id":"1719000001.5-rev1","camera":"garage","severity":"detection","data":{"detections":["1719000001.4-obj1"],"objects":["person"],"sub_labels":[],"zones":[],"audio":[]}},"after":{"id":"1719000001.5-rev1","camera":"garage","severity":"alert","data":{"detections":["1719000001.4-obj1"],"objects":["person"],"sub_labels":[],"zones":["garage_zone"],"audio":[]}}}'
mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" -t frigate/reviews -r -m "$REVIEW"

reviews_ingested=0
for _ in $(seq 1 60); do
  if [ "$(grep -c "Event logged" "$BRIDGE_LOG")" -ge 2 ]; then reviews_ingested=1; break; fi
  if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then break; fi
  sleep 0.5
done
if [ "$reviews_ingested" -ne 1 ]; then
  echo "❌ frigate_bridge did not ingest the frigate/reviews message." >&2
  cat "$BRIDGE_LOG" >&2
  exit 1
fi

echo "✅ frigate_bridge ingested frigate/events and frigate/reviews messages into a sealed log."
grep "Event logged" "$BRIDGE_LOG"
