#!/usr/bin/env bash
# End-to-end test of the Docker sidecar: a real broker container plus the
# real sidecar image, fed one realistic frigate/events message.
#
# Asserts:
#   1. the sidecar comes up with zero configuration beyond FRIGATE_MQTT_HOST
#      (device key auto-generated, all three daemons started);
#   2. the published event is ingested into the sealed log;
#   3. the sealed log verifies in-container (log_verify, derived keys);
#   4. the HA MQTT Discovery config topic is retained on the broker;
#   5. GET /api/fleet answers with the kernel's own row: the config named
#      api.fleet_peers_path and the kernel accepted it (deny_unknown_fields),
#      and a summary the bridge has not written yet is an empty peer list,
#      never an error;
#   6. SECURACV_API_BIND=all reaches the LAN: a second sidecar in that mode
#      with 8799 published on the runner's loopback serves /api/fleet to a
#      curl OUTSIDE the container (the Witness Wall's path), and /events
#      without a token still answers 401 there (the bind never relaxed
#      authentication).
#
# Usage (repo root): docker/sidecar/ci_e2e.sh [image-tag]
set -euo pipefail

IMG="${1:-securacv-sidecar:ci}"
NET="securacv-ci-$$"
BROKER="securacv-ci-mosquitto-$$"
SIDECAR="securacv-ci-sidecar-$$"
SIDECAR_LAN="securacv-ci-sidecar-lan-$$"
# The runner-side port for the LAN-mode container: loopback-only so the
# check never opens a runner port to its network, and off 8799 so nothing
# else on the runner can collide with it.
LAN_PORT=18799

cleanup() {
    docker rm -f "$SIDECAR_LAN" "$SIDECAR" "$BROKER" >/dev/null 2>&1 || true
    docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> Building sidecar image ($IMG)"
docker build -f docker/sidecar/Dockerfile -t "$IMG" .

echo "==> Starting broker"
docker network create "$NET" >/dev/null
docker run -d --name "$BROKER" --network "$NET" eclipse-mosquitto:2 \
    sh -c 'printf "listener 1883 0.0.0.0\nallow_anonymous true\n" > /mosquitto/config/mosquitto.conf && exec mosquitto -c /mosquitto/config/mosquitto.conf' >/dev/null

echo "==> Waiting for the broker to accept connections"
# The sidecar entrypoint fail-fasts (with a doctor hint) when the broker is
# unreachable, so start it only once mosquitto is actually listening.
# Without this, a cold runner that has to pull eclipse-mosquitto:2 loses
# the race and the whole e2e dies in its first 100ms.
broker_ready=0
for _ in $(seq 1 30); do
    if docker run --rm --network "$NET" eclipse-mosquitto:2 \
        mosquitto_pub -h "$BROKER" -t ci/ping -m ping >/dev/null 2>&1; then
        broker_ready=1
        break
    fi
    sleep 1
done
if [ "$broker_ready" -ne 1 ]; then
    echo "❌ broker never accepted connections" >&2
    docker logs "$BROKER" >&2 || true
    exit 1
fi

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

echo "==> Checking GET /api/fleet answers with the kernel's own row"
# The entrypoint names api.fleet_peers_path in the kernel config and hands
# the same file to event_mqtt_bridge. No Canary speaks on this broker, so
# the bridge has written nothing yet: the roll-call must still be a 200 with
# the kernel's row (a missing summary is an empty peer list, not an error).
fleet_doc=$(docker exec "$SIDECAR" curl -fsS http://127.0.0.1:8799/api/fleet || true)
case "$fleet_doc" in
    *'"witness-kernel"'*)
        echo "✓ /api/fleet serves the kernel's row (api.fleet_peers_path accepted)" ;;
    *)
        echo "❌ /api/fleet did not answer with the kernel's row: ${fleet_doc:-<no response>}" >&2
        docker logs "$SIDECAR" >&2 || true
        exit 1 ;;
esac

echo "==> Starting a second sidecar with SECURACV_API_BIND=all (8799 published on the runner's loopback)"
# Its own /data (a fresh anonymous volume): the LAN-mode container must not
# share the first one's database, key or token file. Publishing disabled so
# it does not race the first container's HA Discovery topics on the broker.
docker run -d --name "$SIDECAR_LAN" --network "$NET" \
    -e FRIGATE_MQTT_HOST="$BROKER" -e SECURACV_API_BIND=all -e SECURACV_PUBLISH=false \
    -p "127.0.0.1:${LAN_PORT}:8799" "$IMG" >/dev/null

echo "==> Waiting for the LAN-mode API to answer from OUTSIDE the container"
lan_up=0
for _ in $(seq 1 60); do
    if curl -fsS "http://127.0.0.1:${LAN_PORT}/health" >/dev/null 2>&1; then
        lan_up=1
        break
    fi
    if ! docker ps -q --no-trunc | grep -q "$(docker inspect -f '{{.Id}}' "$SIDECAR_LAN")"; then
        break
    fi
    sleep 1
done
if [ "$lan_up" -ne 1 ]; then
    echo "❌ SECURACV_API_BIND=all sidecar never answered /health on the published port" >&2
    docker logs "$SIDECAR_LAN" >&2 || true
    exit 1
fi
if docker logs "$SIDECAR_LAN" 2>&1 | grep -q "SECURACV_API_BIND=all: witness_api binds 0.0.0.0:8799"; then
    echo "✓ the entrypoint announced the LAN bind once, with the exposure notice"
else
    echo "❌ no SECURACV_API_BIND=all startup notice in the LAN-mode sidecar's log" >&2
    docker logs "$SIDECAR_LAN" >&2 || true
    exit 1
fi

echo "==> Checking GET /api/fleet is readable from the runner (the Witness Wall's path)"
lan_fleet=$(curl -fsS "http://127.0.0.1:${LAN_PORT}/api/fleet" || true)
case "$lan_fleet" in
    *'"witness-kernel"'*)
        echo "✓ /api/fleet serves the kernel's row across the published port" ;;
    *)
        echo "❌ /api/fleet did not answer across the published port: ${lan_fleet:-<no response>}" >&2
        docker logs "$SIDECAR_LAN" >&2 || true
        exit 1 ;;
esac

echo "==> Checking /events without a token is still refused across the published port"
# The bind widened, the authentication did not: every data endpoint keeps
# demanding the rotating capability token. 401 is the kernel's answer to a
# missing bearer; anything else means `all` mode relaxed more than the bind.
lan_events_status=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${LAN_PORT}/events" || true)
if [ "$lan_events_status" = "401" ]; then
    echo "✓ /events answers 401 without a token in SECURACV_API_BIND=all mode"
else
    echo "❌ /events answered HTTP ${lan_events_status:-<none>} without a token (expected 401)" >&2
    docker logs "$SIDECAR_LAN" >&2 || true
    exit 1
fi
docker rm -f "$SIDECAR_LAN" >/dev/null 2>&1 || true

echo "==> Checking the HA Discovery config topic is retained"
# Generous window: the publisher polls the event API every 30s.
if docker run --rm --network "$NET" eclipse-mosquitto:2 \
    mosquitto_sub -h "$BROKER" -t 'homeassistant/#' -C 1 -W 90 | grep -q .; then
    echo "✓ retained discovery payload present"
else
    echo "❌ no retained homeassistant/# discovery payload found" >&2
    exit 1
fi

echo "==> Pressing the HA verify button (witness/cmd/verify)"
docker run --rm --network "$NET" eclipse-mosquitto:2 \
    mosquitto_pub -h "$BROKER" -t witness/cmd/verify -m PRESS
chain_state=$(docker run --rm --network "$NET" eclipse-mosquitto:2 \
    mosquitto_sub -h "$BROKER" -t witness/chain_problem -C 1 -W 60 || true)
if [ "$chain_state" = "OFF" ]; then
    echo "✓ chain integrity published: no problem (chain valid)"
elif [ "$chain_state" = "ON" ]; then
    echo "❌ verification ran but reported a chain problem" >&2
    docker logs "$SIDECAR" >&2 || true
    exit 1
else
    echo "❌ no retained witness/chain_problem state after button press" >&2
    docker logs "$SIDECAR" >&2 || true
    exit 1
fi

echo "✅ sidecar e2e passed: zero-config start, ingest, verify, fleet roll-call, LAN bind opt-in, discovery, button"
