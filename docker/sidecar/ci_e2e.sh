#!/usr/bin/env bash
# End-to-end test of the Docker sidecar: a real broker container plus the
# real sidecar image, fed one realistic frigate/events message.
#
# Asserts:
#   1. the sidecar comes up with zero configuration beyond FRIGATE_MQTT_HOST
#      (device key auto-generated, all three daemons started);
#   2. the published event is ingested into the sealed log;
#   3. the sealed log verifies in-container (log_verify, derived keys);
#   4. the HA MQTT Discovery config topic is retained on the broker.
#
# Usage (repo root): docker/sidecar/ci_e2e.sh [image-tag]
set -euo pipefail

IMG="${1:-securacv-sidecar:ci}"
NET="securacv-ci-$$"
BROKER="securacv-ci-mosquitto-$$"
SIDECAR="securacv-ci-sidecar-$$"

cleanup() {
    docker rm -f "$SIDECAR" "$BROKER" >/dev/null 2>&1 || true
    docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> Building sidecar image ($IMG)"
docker build -f docker/sidecar/Dockerfile -t "$IMG" .

echo "==> Starting broker"
docker network create "$NET" >/dev/null
docker run -d --name "$BROKER" --network "$NET" eclipse-mosquitto:2 \
    sh -c 'printf "listener 1883 0.0.0.0\nallow_anonymous true\n" > /mosquitto/config/mosquitto.conf && exec mosquitto -c /mosquitto/config/mosquitto.conf' >/dev/null

echo "==> Starting sidecar (zero config beyond FRIGATE_MQTT_HOST)"
docker run -d --name "$SIDECAR" --network "$NET" \
    -e FRIGATE_MQTT_HOST="$BROKER" "$IMG" >/dev/null

echo "==> Waiting for the bridge to subscribe"
subscribed=0
for _ in $(seq 1 60); do
    if docker logs "$SIDECAR" 2>&1 | grep -q "Subscribed to frigate/events"; then
        subscribed=1
        break
    fi
    if ! docker ps -q --no-trunc | grep -q "$(docker inspect -f '{{.Id}}' "$SIDECAR")"; then
        break
    fi
    sleep 1
done
if [ "$subscribed" -ne 1 ]; then
    echo "❌ sidecar never subscribed to frigate/events" >&2
    docker logs "$SIDECAR" >&2 || true
    exit 1
fi

echo "==> Publishing a realistic frigate event"
EVENT='{"before":null,"after":{"id":"1719000000.ci-object-id","camera":"front_door","label":"person","sub_label":null,"score":0.81,"top_score":0.92,"current_zones":["porch"],"entered_zones":["driveway","porch"],"false_positive":false,"has_clip":true,"has_snapshot":true},"type":"new"}'
docker run --rm --network "$NET" eclipse-mosquitto:2 \
    mosquitto_pub -h "$BROKER" -t frigate/events -r -m "$EVENT"

echo "==> Waiting for ingest"
ingested=0
for _ in $(seq 1 60); do
    if docker logs "$SIDECAR" 2>&1 | grep -q "Event logged"; then
        ingested=1
        break
    fi
    sleep 1
done
if [ "$ingested" -ne 1 ]; then
    echo "❌ sidecar did not ingest the frigate/events message" >&2
    docker logs "$SIDECAR" >&2 || true
    exit 1
fi

echo "==> Verifying the sealed log in-container"
if ! docker exec "$SIDECAR" sh -c 'DEVICE_KEY_SEED=$(head -n1 /data/device_key) log_verify --db /data/witness.db'; then
    echo "❌ log_verify failed on the sidecar-produced sealed log" >&2
    exit 1
fi

echo "==> Checking the HA Discovery config topic is retained"
# Generous window: the publisher polls the event API every 30s.
if docker run --rm --network "$NET" eclipse-mosquitto:2 \
    mosquitto_sub -h "$BROKER" -t 'homeassistant/#' -C 1 -W 90 | grep -q .; then
    echo "✓ retained discovery payload present"
else
    echo "❌ no retained homeassistant/# discovery payload found" >&2
    exit 1
fi

echo "✅ sidecar e2e passed: zero-config start, ingest, verify, discovery"
