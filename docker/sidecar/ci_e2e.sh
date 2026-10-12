#!/usr/bin/env bash
# End-to-end test of the Docker sidecar: a real broker container plus the
# real sidecar image, fed Frigate's own message shapes (0.13 → 0.17, events
# and reviews) with every identity field populated by a sentinel.
#
# Asserts:
#   1. the sidecar comes up with zero configuration beyond FRIGATE_MQTT_HOST
#      (device key auto-generated, all three daemons started) and, by
#      default, binds every interface so the published 8799 answers /health
#      from OUTSIDE the container — the posture Home Assistant pairs over;
#   2. the replayed fixtures seal what they should (four tracked objects in
#      events mode, three review alerts in reviews mode) and NOTHING Frigate
#      said about who was seen survives: no sentinel in the sealed log (the
#      sealed-log document AND the whole events view, read from inside the
#      container — the database itself is SQLCipher-encrypted, so a copied
#      file is "not a database" to anything but the kernel), on
#      /events/latest, in the export bundle, on any witness/# or
#      homeassistant/# topic, or in the daemon log; a `face`-labeled object
#      and a `face`-only review alert seal nothing;
#   3. the sealed log verifies in-container (log_verify, derived keys);
#   4. GET /api/fleet answers with the kernel's own row and nothing else
#      (SECURACV_FLEET_ROLLCALL unset → `self`); with it set, the identity
#      document says `full`;
#   5. the retained, kernel-signed announce on witness/<id>/announce carries
#      exactly the contract's keys — no host (unless SECURACV_ANNOUNCE_HOST
#      names one), no code, no token, no seed, no chain state — and says
#      pairing is required;
#   6. pairing posture on the published port: a wrong code, no code, a
#      burned code (five wrong guesses) and the right code after a burn all
#      answer the SAME 401 {"error":"invalid_pairing_code"}; the sixth wrong
#      guess still does; a fresh `docker exec … pair` plus the right code
#      returns a paired token that reads /api/identity, and that token is
#      refused after `unpair`; `pair` prints exactly the four-line card and
#      the daemon log never contains a code; the fingerprint on the card is
#      sha256 of the announced key;
#   7. receipts: N polls of /events/latest add no export receipt rows while
#      one /export/bundle adds exactly one;
#   8. `doctor` (plain) passes on the paired install and names the pairing
#      state without the code; `doctor --live` runs the Frigate listen;
#   9. the HA MQTT Discovery config topic is retained, and the verify
#      button answers;
#  10. `docker stop` ends the container with exit 0 and clears the retained
#      announce (a removed sidecar stops offering a "discovered" card).
#
# Steps that need the kernel's pairing/announce endpoints (W2) or the
# tolerant Frigate parser (W1) are guarded: when the running image predates
# them they are SKIPPED with a message instead of failing, so the script is
# useful on a partial tree. CI sets CI_E2E_REQUIRE_ALL=1, which turns every
# skip into a failure: on the merged tree nothing may be missing.
#
# Usage (repo root): docker/sidecar/ci_e2e.sh [image-tag]
set -euo pipefail

IMG="${1:-securacv-sidecar:ci}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIXTURES="$HERE/fixtures/frigate"
NET="securacv-ci-$$"
BROKER="securacv-ci-mosquitto-$$"
SIDECAR="securacv-ci-sidecar-$$"
SIDECAR_B="securacv-ci-sidecar-b-$$"
CAPTURE="securacv-ci-capture-$$"
# Runner-side ports for the two sidecars: loopback-only so the check never
# opens a runner port to its network, and off 8799 so nothing else on the
# runner can collide with them.
PORT_A=18799
PORT_B=18798
# TEST-NET-1 (RFC 5737): never routable, so the announce-host check can
# name an address without anything answering on it.
ANNOUNCE_HOST_B="192.0.2.10"
REQUIRE_ALL="${CI_E2E_REQUIRE_ALL:-0}"
SCRATCH=$(mktemp -d)

cleanup() {
    docker rm -f "$CAPTURE" "$SIDECAR_B" "$SIDECAR" "$BROKER" >/dev/null 2>&1 || true
    docker network rm "$NET" >/dev/null 2>&1 || true
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

# `cmd | grep -q` is a trap under pipefail: grep stops reading at its first
# match, the command still writing into the pipe dies of SIGPIPE (141), and
# the pipeline reports that death as a failure. A line that is in the log
# reads as missing whenever the log runs on past it. So read the whole
# output first and grep the copy.
logs_have() {
    local out
    out=$(docker logs "$1" 2>&1) || true
    grep -qF -- "$2" <<<"$out"
}
logs_count() {
    local out
    out=$(docker logs "$1" 2>&1) || true
    grep -cF -- "$2" <<<"$out" || true
}
running() {
    [ "$(docker inspect -f '{{.State.Running}}' "$1" 2>/dev/null)" = "true" ]
}
fail() {
    echo "❌ $1" >&2
    if [ -n "${2:-}" ]; then
        docker logs "$2" >&2 || true
    fi
    exit 1
}
pass() { echo "✓ $*"; }
# A step the running image cannot take yet. CI refuses to skip.
skip() {
    if [ "$REQUIRE_ALL" = "1" ]; then
        fail "required step skipped: $*"
    fi
    echo "↷ skipped: $*"
}
# wait_for_log CONTAINER NEEDLE SECS — true once the log has the line.
wait_for_log() {
    local container="$1" needle="$2" secs="$3"
    for _ in $(seq 1 "$secs"); do
        if logs_have "$container" "$needle"; then
            return 0
        fi
        if ! running "$container"; then
            return 1
        fi
        sleep 1
    done
    return 1
}
mq_pub() { docker run --rm --network "$NET" eclipse-mosquitto:2 mosquitto_pub -h "$BROKER" "$@"; }
mq_sub() { docker run --rm --network "$NET" eclipse-mosquitto:2 mosquitto_sub -h "$BROKER" "$@"; }
# mq_pub_file TOPIC FILE — publish a fixture's bytes (the fixtures directory
# is mounted read-only into the throwaway client).
mq_pub_file() {
    docker run --rm --network "$NET" -v "$FIXTURES:/fx:ro" eclipse-mosquitto:2 \
        mosquitto_pub -h "$BROKER" -t "$1" -f "/fx/$(basename "$2")"
}
http_status() { curl -s -o /dev/null -w '%{http_code}' "$@" || true; }
cap_token() { docker exec "$1" cat /data/api_token | tr -d '[:space:]'; }
# sha256 of a hex-encoded key, first 8 hex: the fingerprint rule `pair` and
# the Home Assistant pairing step share.
fingerprint_of() {
    python3 -I -c 'import hashlib, sys; print(hashlib.sha256(bytes.fromhex(sys.argv[1])).hexdigest()[:8])' "$1"
}

# ---------------------------------------------------------------------------
echo "==> Building sidecar image ($IMG)"
docker build -f docker/sidecar/Dockerfile -t "$IMG" .

echo "==> Checking the fixtures parse and carry sentinels"
for f in "$FIXTURES"/*.json; do
    jq -e . "$f" >/dev/null || fail "fixture $f is not JSON"
done
# Every sentinel token the fixtures carry: strings that begin with SENTINEL_
# and numbers that begin with 7770 (exact tokens, so an epoch or a hash
# cannot match by accident).
jq -r '.. | strings | select(startswith("SENTINEL_"))' "$FIXTURES"/*.json | sort -u > "$SCRATCH/sentinels"
jq -r '.. | numbers | tostring | select(startswith("7770"))' "$FIXTURES"/*.json | sort -u >> "$SCRATCH/sentinels"
sentinel_count=$(wc -l < "$SCRATCH/sentinels" | tr -d ' ')
[ "$sentinel_count" -gt 100 ] || fail "only $sentinel_count sentinel tokens extracted from the fixtures"
fixture_files=("$FIXTURES"/*.json)
pass "$sentinel_count sentinel tokens extracted from ${#fixture_files[@]} fixtures"

echo "==> Starting broker"
docker network create "$NET" >/dev/null
docker run -d --name "$BROKER" --network "$NET" eclipse-mosquitto:2 \
    sh -c 'printf "listener 1883 0.0.0.0\nallow_anonymous true\n" > /mosquitto/config/mosquitto.conf && exec mosquitto -c /mosquitto/config/mosquitto.conf' >/dev/null

echo "==> Waiting for the broker to accept connections"
# The sidecar entrypoint fail-fasts (with a doctor hint) when the broker is
# unreachable, so start it only once mosquitto is actually listening.
broker_ready=0
for _ in $(seq 1 30); do
    if mq_pub -t ci/ping -m ping >/dev/null 2>&1; then
        broker_ready=1
        break
    fi
    sleep 1
done
if [ "$broker_ready" -ne 1 ]; then
    fail "broker never accepted connections" "$BROKER"
fi

# Everything the bridges publish, retained or not, for the whole run: the
# sentinel sweep reads this capture, so a zone event that is not retained
# cannot slip past a later subscribe.
docker run -d --name "$CAPTURE" --network "$NET" eclipse-mosquitto:2 \
    mosquitto_sub -h "$BROKER" -v -t 'witness/#' -t 'witness_b/#' -t 'homeassistant/#' >/dev/null

echo "==> Starting sidecar A (zero config beyond FRIGATE_MQTT_HOST; 8799 published on the runner's loopback)"
docker run -d --name "$SIDECAR" --network "$NET" \
    -e FRIGATE_MQTT_HOST="$BROKER" -p "127.0.0.1:${PORT_A}:8799" "$IMG" >/dev/null

echo "==> Waiting for the bridge to subscribe"
wait_for_log "$SIDECAR" "Subscribed to frigate/events" 60 \
    || fail "sidecar never subscribed to frigate/events" "$SIDECAR"
if logs_have "$SIDECAR" "SECURACV_API_BIND=all: witness_api binds 0.0.0.0:8799"; then
    pass "the entrypoint binds every interface by default and logs the exposure notice"
else
    fail "no SECURACV_API_BIND=all startup notice — the default posture is not bind-all" "$SIDECAR"
fi

echo "==> Waiting for the API to answer from OUTSIDE the container (the published port)"
lan_up=0
for _ in $(seq 1 60); do
    if curl -fsS "http://127.0.0.1:${PORT_A}/health" >/dev/null 2>&1; then
        lan_up=1
        break
    fi
    running "$SIDECAR" || break
    sleep 1
done
[ "$lan_up" -eq 1 ] || fail "the default sidecar never answered /health on the published port" "$SIDECAR"
pass "/health answers across the published port with no configuration"

# Which contracts the running binaries implement. The API's identity route
# arrives with pairing and the announce (W2); the Frigate link topic arrives
# with the tolerant parser (W1). Each is a contract item, not a log line.
identity_probe=$(http_status -H "Authorization: Bearer $(cap_token "$SIDECAR")" "http://127.0.0.1:${PORT_A}/api/identity")
case "$identity_probe" in
    200) W2_PRESENT=1; pass "kernel implements /api/identity (pairing + announce contracts present)" ;;
    404|405) W2_PRESENT=0; echo "   kernel answers $identity_probe on /api/identity: pairing/announce steps will be skipped" ;;
    *) fail "/api/identity answered HTTP $identity_probe with the capability token" "$SIDECAR" ;;
esac
if mq_sub -t 'witness/frigate_link' -C 1 -W 30 >/dev/null 2>&1; then
    W1_PRESENT=1
    pass "frigate_bridge publishes witness/frigate_link (tolerant Frigate parser present)"
else
    W1_PRESENT=0
    echo "   no retained witness/frigate_link within 30s: the exact seal counts will be skipped"
fi

echo "==> Replaying the Frigate events fixtures (every version shape, identity fields as sentinels)"
for f in "$FIXTURES"/events_*.json; do
    mq_pub_file frigate/events "$f"
    echo "   published $(basename "$f")"
done
# Four fixtures carry an object the kernel seals; the `face` one must not.
EXPECTED_A=4
expect_a="$EXPECTED_A"
[ "$W1_PRESENT" -eq 1 ] || expect_a=1
sealed_a=0
for _ in $(seq 1 60); do
    sealed_a=$(logs_count "$SIDECAR" "Event logged")
    [ "$sealed_a" -ge "$expect_a" ] && break
    sleep 1
done
[ "$sealed_a" -ge "$expect_a" ] || fail "sidecar sealed $sealed_a of the expected $expect_a replayed events" "$SIDECAR"
sleep 3
sealed_a=$(logs_count "$SIDECAR" "Event logged")
if [ "$W1_PRESENT" -eq 1 ]; then
    [ "$sealed_a" -eq "$EXPECTED_A" ] \
        || fail "events mode sealed $sealed_a events, expected exactly $EXPECTED_A (the face-labeled object must seal nothing)" "$SIDECAR"
    pass "events mode sealed exactly $EXPECTED_A events; the face-labeled object sealed nothing"
else
    skip "exact seal count (tolerant parser absent; $sealed_a sealed)"
fi

echo "==> Sweeping every output for the fixtures' sentinels"
# The capability token reads the sealed-log document and the latest event
# across the published port; the whole retained log is read the way its
# owner reads it — the kernel API from INSIDE the container, every event
# since bucket zero, with the token the container holds (the database is
# SQLCipher-encrypted: a copied witness.db is "file is not a database" to
# sqlite3, so a raw dump cannot be the sweep); the MQTT capture and the
# daemon log complete the set. The export bundle joins the set later, when
# the receipts step fetches one.
events_view() {
    # events_view CONTAINER FILE — GET /events?since_bucket=0 from inside
    # the container (curl ships in the image for the HEALTHCHECK).
    docker exec "$1" sh -c 'curl -fsS -H "Authorization: Bearer $(tr -d "[:space:]" < /data/api_token)" "http://127.0.0.1:8799/events?since_bucket=0"' > "$2"
}
# events_in FILE — how many sealed events an events view (or any kernel
# event document) carries, whatever the batch/bucket nesting.
events_in() { jq '[.. | objects | select(has("zone_id") and has("time_bucket"))] | length' "$1"; }
curl -fsS -H "Authorization: Bearer $(cap_token "$SIDECAR")" "http://127.0.0.1:${PORT_A}/api/sealed-log" > "$SCRATCH/a_sealed_log.json" \
    || fail "GET /api/sealed-log failed with the capability token" "$SIDECAR"
curl -fsS -H "Authorization: Bearer $(cap_token "$SIDECAR")" "http://127.0.0.1:${PORT_A}/events/latest" > "$SCRATCH/a_events_latest.json" \
    || fail "GET /events/latest failed with the capability token" "$SIDECAR"
events_view "$SIDECAR" "$SCRATCH/a_events_view.json" \
    || fail "GET /events?since_bucket=0 inside sidecar A failed (curl with the container's own token)" "$SIDECAR"
docker logs "$SIDECAR" > "$SCRATCH/a_logs.txt" 2>&1 || true
docker logs "$CAPTURE" > "$SCRATCH/mqtt_capture.txt" 2>&1 || true
[ -s "$SCRATCH/mqtt_capture.txt" ] || fail "the MQTT capture is empty — nothing was published under witness/# or homeassistant/#" "$CAPTURE"
sweep_outputs() {
    # sweep_outputs LABEL FILE... — fail on the first sentinel found anywhere.
    # A token must stand as a whole word (grep -w): the numeric sentinels
    # are 7770-prefixed, and a digit-substring match once failed on a
    # wall-clock bucket start (1777062000 contains the `area` sentinel
    # 7770620). The SENTINEL_ prefix is a leak wherever it appears.
    local label="$1"; shift
    local tok hit
    if hit=$(grep -lF -- "SENTINEL_" "$@" 2>/dev/null) && [ -n "$hit" ]; then
        echo "a SENTINEL_ string found in: $hit" >&2
        grep -nF -- "SENTINEL_" "$hit" | head -n 5 >&2
        fail "identity leaked ($label): a SENTINEL_ string reached an output"
    fi
    while IFS= read -r tok; do
        [ -n "$tok" ] || continue
        if hit=$(grep -lwF -- "$tok" "$@" 2>/dev/null) && [ -n "$hit" ]; then
            echo "sentinel '$tok' found in: $hit" >&2
            grep -nwF -- "$tok" "$hit" | head -n 5 >&2
            fail "identity leaked ($label): Frigate's '$tok' reached an output"
        fi
    done < "$SCRATCH/sentinels"
    pass "no sentinel in $label"
}
sweep_outputs "sidecar A: sealed-log document, events view since bucket zero, /events/latest, MQTT capture, daemon log" \
    "$SCRATCH/a_sealed_log.json" "$SCRATCH/a_events_view.json" "$SCRATCH/a_events_latest.json" \
    "$SCRATCH/mqtt_capture.txt" "$SCRATCH/a_logs.txt"
# Belt and braces: the events view must have carried the sealed events at
# all, or the sweep read nothing.
view_events_a=$(events_in "$SCRATCH/a_events_view.json")
[ "$view_events_a" -ge "$expect_a" ] \
    || fail "the events view inside sidecar A carries $view_events_a events, fewer than the $expect_a sealed — the sweep read nothing"
grep -q '"payload"' "$SCRATCH/a_sealed_log.json" || fail "the sealed-log document has no entries — the sweep read nothing"
pass "the events view carries $view_events_a sealed events for the sweep"

echo "==> Verifying the sealed log in-container"
if ! docker exec "$SIDECAR" sh -c 'DEVICE_KEY_SEED=$(head -n1 /data/device_key) log_verify --db /data/witness.db'; then
    fail "log_verify failed on the sidecar-produced sealed log"
fi

echo "==> Checking GET /api/fleet answers with the kernel's own row and nothing else"
# No Canary speaks on this broker and SECURACV_FLEET_ROLLCALL is unset, so
# the roll-call is the kernel's row alone — a 200, never an error.
fleet_doc=$(curl -fsS "http://127.0.0.1:${PORT_A}/api/fleet" || true)
[ "$(printf '%s' "$fleet_doc" | jq -r '.kernel // empty')" = "witness-kernel" ] \
    || fail "/api/fleet did not answer with the kernel's row: ${fleet_doc:-<no response>}" "$SIDECAR"
[ "$(printf '%s' "$fleet_doc" | jq -r '.devices | length')" = "1" ] \
    || fail "/api/fleet lists more than the kernel's own row by default: $fleet_doc"
pass "/api/fleet serves the kernel's own row only (roll-call self by default)"

echo "==> Checking /events without a token is still refused across the published port"
events_status=$(http_status "http://127.0.0.1:${PORT_A}/events")
[ "$events_status" = "401" ] || fail "/events answered HTTP ${events_status:-<none>} without a token (expected 401)" "$SIDECAR"
pass "/events answers 401 without a token across the published port"

# ---------------------------------------------------------------------------
echo "==> Checking the retained announce"
ANNOUNCE_DEVICE_ID=""
ANNOUNCE_KEY=""
if [ "$W2_PRESENT" -eq 1 ]; then
    announce_line=$(mq_sub -v -t 'witness/+/announce' -C 1 -W 60 || true)
    [ -n "$announce_line" ] || fail "no retained witness/+/announce within 60s" "$SIDECAR"
    announce_topic="${announce_line%% *}"
    announce="${announce_line#* }"
    printf '%s' "$announce" > "$SCRATCH/announce.json"
    jq -e . "$SCRATCH/announce.json" >/dev/null || fail "the announce is not JSON: $announce"
    # Exactly the contract's keys: anything else on an open retained topic
    # is a leak by construction.
    expected_keys='["announced_bucket_size","announced_bucket_start","api","availability_topic","device_id","discovery_prefix","kernel_version","mqtt_topic_prefix","pairing","sig","v","verifying_key"]'
    actual_keys=$(jq -c 'keys' "$SCRATCH/announce.json")
    [ "$actual_keys" = "$expected_keys" ] || fail "announce keys are $actual_keys, expected $expected_keys"
    [ "$(jq -c '.api | keys' "$SCRATCH/announce.json")" = '["bind","port","scheme"]' ] \
        || fail "announce api keys are $(jq -c '.api | keys' "$SCRATCH/announce.json") — no host may ride along unless SECURACV_ANNOUNCE_HOST names one"
    [ "$(jq -r '.pairing' "$SCRATCH/announce.json")" = "required" ] || fail "announce pairing is not 'required': $announce"
    [ "$(jq -r '.api.bind' "$SCRATCH/announce.json")" = "all" ] || fail "announce api.bind is not 'all': $announce"
    [ "$(jq -r '.api.port' "$SCRATCH/announce.json")" = "8799" ] || fail "announce api.port is not 8799: $announce"
    [ "$(jq -r '.api.scheme' "$SCRATCH/announce.json")" = "http" ] || fail "announce api.scheme is not http: $announce"
    [ "$(jq -r '.mqtt_topic_prefix' "$SCRATCH/announce.json")" = "witness" ] || fail "announce mqtt_topic_prefix is not witness"
    [ "$(jq -r '.availability_topic' "$SCRATCH/announce.json")" = "witness/status" ] || fail "announce availability_topic is not witness/status"
    ANNOUNCE_DEVICE_ID=$(jq -r '.device_id' "$SCRATCH/announce.json")
    ANNOUNCE_KEY=$(jq -r '.verifying_key' "$SCRATCH/announce.json")
    [[ "$ANNOUNCE_DEVICE_ID" =~ ^pwk_[0-9a-f]{8}$ ]] || fail "announce device_id has the wrong shape: $ANNOUNCE_DEVICE_ID"
    [[ "$ANNOUNCE_KEY" =~ ^[0-9a-f]{64}$ ]] || fail "announce verifying_key is not 64 hex"
    [[ "$(jq -r '.sig' "$SCRATCH/announce.json")" =~ ^[0-9a-f]{128}$ ]] || fail "announce sig is not 128 hex"
    [ "$announce_topic" = "witness/$ANNOUNCE_DEVICE_ID/announce" ] || fail "announce topic $announce_topic does not match its device_id"
    # No secret of this container is on the open topic: not the seed, not
    # the capability token.
    seed_a=$(docker exec "$SIDECAR" head -n1 /data/device_key | tr -d '[:space:]')
    grep -qF -- "$seed_a" "$SCRATCH/announce.json" && fail "the announce contains the device key seed"
    grep -qF -- "$(cap_token "$SIDECAR")" "$SCRATCH/announce.json" && fail "the announce contains the capability token"
    grep -qiE '"(code|token|seed|chain_length|latest_hash|host)"' "$SCRATCH/announce.json" && fail "the announce carries a forbidden key"
    pass "retained announce on $announce_topic: contract keys only, pairing required, bind all, signed, no host/code/token/seed"
    # The announce's key is the key the API serves.
    api_key=$(curl -fsS -H "Authorization: Bearer $(cap_token "$SIDECAR")" "http://127.0.0.1:${PORT_A}/api/identity" | jq -r '.verifying_key')
    [ "$api_key" = "$ANNOUNCE_KEY" ] || fail "announce key $ANNOUNCE_KEY differs from /api/identity key $api_key"
    pass "the announced key is the key /api/identity serves"
else
    skip "retained announce assertions (kernel has no /api/identity)"
fi

# ---------------------------------------------------------------------------
echo "==> Pairing posture across the published port"
# pair_post CODE — POST /api/pair; sets PAIR_STATUS and PAIR_BODY. The
# per-IP lockout (five failures → 2 s, doubling) is the backstop, not the
# posture under test, so a 429 is waited out and the request retried.
pair_post() {
    local code="$1" raw wait
    for _ in 1 2 3 4 5 6; do
        raw=$(curl -s -w '\n%{http_code}' -H 'Content-Type: application/json' \
            --data "{\"code\":\"$code\",\"label\":\"ci\"}" "http://127.0.0.1:${PORT_A}/api/pair" || true)
        PAIR_STATUS="${raw##*$'\n'}"
        PAIR_BODY="${raw%$'\n'*}"
        if [ "$PAIR_STATUS" != "429" ]; then
            return 0
        fi
        wait=$(printf '%s' "$PAIR_BODY" | jq -r '.retry_after // 2' 2>/dev/null || echo 2)
        sleep $((wait + 1))
    done
    return 1
}
expect_invalid_code() {
    # expect_invalid_code CODE LABEL — the constant refusal, byte for byte.
    pair_post "$1" || fail "POST /api/pair stayed locked (429) for '$2'" "$SIDECAR"
    [ "$PAIR_STATUS" = "401" ] || fail "'$2': expected 401, got HTTP $PAIR_STATUS: $PAIR_BODY" "$SIDECAR"
    [ "$PAIR_BODY" = '{"error":"invalid_pairing_code"}' ] || fail "'$2': expected the constant body, got: $PAIR_BODY" "$SIDECAR"
}
# pair_card CONTAINER — run `docker exec … pair` and keep its stdout.
pair_card() { docker exec "$1" pair; }
card_code() { sed -n 's/^SecuraCV pairing code: \([0-9]\{6\}\)   .*/\1/p' <<<"$1"; }
card_fingerprint() { sed -n 's/^Kernel key fingerprint: \([0-9a-f]\{8\}\)   .*/\1/p' <<<"$1"; }
wrong_code_for() { if [ "$1" = "000000" ]; then echo "000001"; else echo "000000"; fi; }

PAIRED_TOKEN=""
PAIRED_TOKEN_ID=""
if [ "$W2_PRESENT" -eq 1 ]; then
    # (a) no code outstanding → the same 401
    expect_invalid_code "000000" "no code outstanding"
    pass "POST /api/pair with no code outstanding answers 401 invalid_pairing_code"
    [ "$(http_status "http://127.0.0.1:${PORT_A}/api/pair")" = "405" ] || fail "GET /api/pair is not 405"
    [ "$(http_status -X POST "http://127.0.0.1:${PORT_A}/api/pair")" = "400" ] || fail "POST /api/pair without a body is not 400"
    pass "GET /api/pair is 405; a bodiless POST is 400 (malformed input is not a strike)"

    # (b) `docker exec securacv pair` prints exactly the card
    card=$(pair_card "$SIDECAR") || fail "docker exec … pair failed" "$SIDECAR"
    [ "$(printf '%s\n' "$card" | wc -l | tr -d ' ')" = "4" ] || fail "pair printed $(printf '%s\n' "$card" | wc -l) lines, expected 4:
$card"
    code1=$(card_code "$card")
    fp1=$(card_fingerprint "$card")
    [[ "$code1" =~ ^[0-9]{6}$ ]] || fail "pair card line 1 has no 6-digit code: $card"
    printf '%s\n' "$card" | sed -n 1p | grep -qE '^SecuraCV pairing code: [0-9]{6}   \(valid 10 minutes, 5 attempts\)$' \
        || fail "pair card line 1 is off-contract: $(printf '%s\n' "$card" | sed -n 1p)"
    printf '%s\n' "$card" | sed -n 2p | grep -qE '^Kernel key fingerprint: [0-9a-f]{8}   \(Home Assistant shows the same fingerprint before it pairs\)$' \
        || fail "pair card line 2 is off-contract: $(printf '%s\n' "$card" | sed -n 2p)"
    [ "$(printf '%s\n' "$card" | sed -n 3p)" = "Kernel URL to confirm in Home Assistant: http://<this docker host>:8799" ] \
        || fail "pair card line 3 is off-contract: $(printf '%s\n' "$card" | sed -n 3p)"
    [ "$(printf '%s\n' "$card" | sed -n 4p)" = 'In Home Assistant: Settings → Devices & Services → the discovered "SecuraCV sidecar" card → Configure → enter the code.' ] \
        || fail "pair card line 4 is off-contract: $(printf '%s\n' "$card" | sed -n 4p)"
    [ "$fp1" = "$(fingerprint_of "$ANNOUNCE_KEY")" ] || fail "pair fingerprint $fp1 is not sha256 of the announced key ($(fingerprint_of "$ANNOUNCE_KEY"))"
    pass "pair prints exactly the four-line card; its fingerprint is sha256 of the announced key"

    # (c) five wrong guesses burn the code; every refusal is the same 401
    wrong=$(wrong_code_for "$code1")
    for guess in 1 2 3 4 5; do
        expect_invalid_code "$wrong" "wrong guess $guess"
    done
    pass "five wrong guesses each answer the same 401 (the code is now burned)"
    # (d) the sixth wrong guess still answers the same 401 — no oracle on "burned"
    expect_invalid_code "$wrong" "sixth wrong guess after the burn"
    pass "the sixth wrong guess answers the same 401"
    # (e) the right code after the burn is refused the same way
    expect_invalid_code "$code1" "right code after the burn"
    pass "the right code after a burn answers the same 401"
    docker exec "$SIDECAR" sh -c 'test ! -e /data/pairing_code' || fail "the burned pairing code file still exists"

    # (f) a fresh pair + the right code returns a token
    card2=$(pair_card "$SIDECAR") || fail "second docker exec … pair failed" "$SIDECAR"
    code2=$(card_code "$card2")
    [[ "$code2" =~ ^[0-9]{6}$ ]] || fail "second pair card has no code: $card2"
    pair_post "$code2" || fail "POST /api/pair stayed locked for the right code" "$SIDECAR"
    [ "$PAIR_STATUS" = "200" ] || fail "the right code answered HTTP $PAIR_STATUS: $PAIR_BODY" "$SIDECAR"
    PAIRED_TOKEN=$(printf '%s' "$PAIR_BODY" | jq -r '.token // empty')
    PAIRED_TOKEN_ID=$(printf '%s' "$PAIR_BODY" | jq -r '.token_id // empty')
    [[ "$PAIRED_TOKEN" =~ ^[0-9a-f]{64}$ ]] || fail "pair response has no 64-hex token: $PAIR_BODY"
    [[ "$PAIRED_TOKEN_ID" =~ ^[0-9a-f]{8}$ ]] || fail "pair response has no 8-hex token_id: $PAIR_BODY"
    [ "$(printf '%s' "$PAIR_BODY" | jq -r '.device_id')" = "$ANNOUNCE_DEVICE_ID" ] || fail "pair response device_id differs from the announce: $PAIR_BODY"
    [ "$(printf '%s' "$PAIR_BODY" | jq -r '.verifying_key')" = "$ANNOUNCE_KEY" ] || fail "pair response verifying_key differs from the announce: $PAIR_BODY"
    [ -n "$(printf '%s' "$PAIR_BODY" | jq -r '.kernel_version // empty')" ] || fail "pair response has no kernel_version: $PAIR_BODY"
    pass "a fresh pair + the right code returns a paired token naming the announced identity"
    # single use: the same code again is just another refusal
    expect_invalid_code "$code2" "the consumed code presented again"
    docker exec "$SIDECAR" sh -c 'test ! -e /data/pairing_code' || fail "the consumed pairing code file still exists"
    pass "the consumed code is refused the same way (single use)"

    # (g) the paired token reads /api/identity
    identity=$(curl -fsS -H "Authorization: Bearer $PAIRED_TOKEN" "http://127.0.0.1:${PORT_A}/api/identity" || true)
    [ -n "$identity" ] || fail "the paired token could not read /api/identity" "$SIDECAR"
    [ "$(printf '%s' "$identity" | jq -r '.device_id')" = "$ANNOUNCE_DEVICE_ID" ] || fail "/api/identity device_id differs: $identity"
    [ "$(printf '%s' "$identity" | jq -r '.fleet_rollcall')" = "self" ] || fail "/api/identity fleet_rollcall is not self: $identity"
    [ "$(printf '%s' "$identity" | jq -r '.paired_tokens')" = "1" ] || fail "/api/identity paired_tokens is not 1: $identity"
    [ "$(http_status "http://127.0.0.1:${PORT_A}/api/identity")" = "401" ] || fail "/api/identity without a token is not 401"
    pass "the paired token reads /api/identity (fleet_rollcall self, one paired token); no token → 401"

    # The daemon log never carries a code — either of them.
    docker logs "$SIDECAR" > "$SCRATCH/a_logs_after_pair.txt" 2>&1 || true
    grep -qF -- "$code1" "$SCRATCH/a_logs_after_pair.txt" && fail "the daemon log contains the first pairing code"
    grep -qF -- "$code2" "$SCRATCH/a_logs_after_pair.txt" && fail "the daemon log contains the second pairing code"
    grep -qF -- "$PAIRED_TOKEN" "$SCRATCH/a_logs_after_pair.txt" && fail "the daemon log contains the paired token"
    pass "the daemon log carries neither pairing code nor the paired token"
else
    skip "pairing posture (kernel has no /api/pair)"
fi

# ---------------------------------------------------------------------------
echo "==> Receipts: polling /events/latest adds none, one /export/bundle adds one"
receipt_count() {
    docker exec "$SIDECAR" sh -c 'DEVICE_KEY_SEED=$(head -n1 /data/device_key) log_verify --db /data/witness.db --json' \
        | jq -r '.export_receipts_verified'
}
if [ "$W2_PRESENT" -eq 1 ]; then
    before=$(receipt_count)
    [[ "$before" =~ ^[0-9]+$ ]] || fail "log_verify --json gave no export_receipts_verified: $before"
    for _ in 1 2 3 4 5; do
        [ "$(http_status -H "Authorization: Bearer $PAIRED_TOKEN" "http://127.0.0.1:${PORT_A}/events/latest")" = "200" ] \
            || fail "/events/latest did not answer 200 to the paired token" "$SIDECAR"
    done
    after_polls=$(receipt_count)
    [ "$after_polls" = "$before" ] || fail "5 polls of /events/latest moved the export receipt count from $before to $after_polls"
    pass "5 polls of /events/latest added no export receipt ($before)"
    export_status=$(curl -sS -o "$SCRATCH/a_export_bundle.json" -w '%{http_code}' \
        -H "Authorization: Bearer $PAIRED_TOKEN" "http://127.0.0.1:${PORT_A}/export/bundle?last=24h" || true)
    [ "$export_status" = "200" ] || fail "/export/bundle answered HTTP ${export_status:-<none>} to the paired token (expected 200)" "$SIDECAR"
    after_export=$(receipt_count)
    [ "$after_export" = "$((before + 1))" ] || fail "one /export/bundle moved the export receipt count from $before to $after_export (expected $((before + 1)))"
    pass "one /export/bundle added exactly one export receipt ($after_export)"
    # The bundle is the hand-off unit: swept like every other output.
    [ "$(events_in "$SCRATCH/a_export_bundle.json")" -ge 1 ] || fail "the export bundle carries no events — nothing to sweep"
    sweep_outputs "sidecar A: export bundle (24h, paired token)" "$SCRATCH/a_export_bundle.json"
else
    skip "receipt accounting (needs the receipt-less state reads)"
fi

# ---------------------------------------------------------------------------
echo "==> doctor: setup checks pass on the paired install, --live listens"
if [ "$W2_PRESENT" -eq 1 ]; then
    doctor_out=$(docker exec "$SIDECAR" doctor) || fail "plain doctor failed on a healthy, paired install:
$doctor_out"
    grep -qF -- "✓ paired: yes (1 token)" <<<"$doctor_out" || fail "doctor does not report the paired token:
$doctor_out"
    grep -qF -- "- pairing code outstanding: no" <<<"$doctor_out" || fail "doctor does not report 'no code outstanding':
$doctor_out"
    grep -qF -- "Retained announce present" <<<"$doctor_out" || fail "doctor does not see the retained announce:
$doctor_out"
    grep -qF -- "Frigate traffic: not checked" <<<"$doctor_out" || fail "plain doctor ran (or did not mention) the live listen:
$doctor_out"
    grep -qF -- "All checks passed." <<<"$doctor_out" || fail "doctor did not pass:
$doctor_out"
    pass "plain doctor passes on the paired install and names the pairing state"
    # A code outstanding: doctor says so in minutes, never the digits.
    card3=$(pair_card "$SIDECAR") || fail "third docker exec … pair failed" "$SIDECAR"
    code3=$(card_code "$card3")
    doctor_out=$(docker exec "$SIDECAR" doctor) || fail "doctor failed with a code outstanding:
$doctor_out"
    grep -qE -- "- pairing code outstanding: yes \(expires in (9|10) min\)" <<<"$doctor_out" || fail "doctor does not report the outstanding code's expiry:
$doctor_out"
    grep -qF -- "$code3" <<<"$doctor_out" && fail "doctor printed the pairing code"
    pass "doctor reports an outstanding code by expiry only, never the digits"
else
    skip "plain doctor pass (its announce check needs the kernel announce)"
fi
# --live: the listen runs (nothing publishes here, so it fails and says so).
live_out=$(docker exec -e DOCTOR_LISTEN_SECS=2 "$SIDECAR" doctor --live 2>&1 || true)
grep -qF -- "Listening on frigate/events for 2s" <<<"$live_out" || fail "doctor --live did not run the Frigate listen:
$live_out"
grep -qF -- "No messages on frigate/events within 2s" <<<"$live_out" || fail "doctor --live did not report the silent topic:
$live_out"
pass "doctor --live runs the Frigate listen (plain doctor does not)"

# ---------------------------------------------------------------------------
echo "==> unpair revokes the paired token"
if [ "$W2_PRESENT" -eq 1 ]; then
    docker exec "$SIDECAR" unpair "$PAIRED_TOKEN_ID" || fail "docker exec … unpair $PAIRED_TOKEN_ID failed" "$SIDECAR"
    revoked=$(http_status -H "Authorization: Bearer $PAIRED_TOKEN" "http://127.0.0.1:${PORT_A}/api/identity")
    [ "$revoked" = "401" ] || fail "a revoked paired token answered HTTP $revoked (expected 401)" "$SIDECAR"
    doctor_out=$(docker exec "$SIDECAR" doctor || true)
    grep -qF -- "- paired: no" <<<"$doctor_out" || fail "doctor still reports a paired token after unpair:
$doctor_out"
    pass "unpair revokes the token without a restart; doctor reports 'paired: no'"
else
    skip "unpair (kernel has no paired tokens)"
fi

# ---------------------------------------------------------------------------
echo "==> Starting sidecar B (reviews mode, SECURACV_ANNOUNCE_HOST, SECURACV_FLEET_ROLLCALL=1, prefix witness_b)"
docker run -d --name "$SIDECAR_B" --network "$NET" \
    -e FRIGATE_MQTT_HOST="$BROKER" -e FRIGATE_ENABLE_REVIEWS=true \
    -e MQTT_TOPIC_PREFIX=witness_b -e SECURACV_ANNOUNCE_HOST="$ANNOUNCE_HOST_B" -e SECURACV_FLEET_ROLLCALL=1 \
    -p "127.0.0.1:${PORT_B}:8799" "$IMG" >/dev/null
wait_for_log "$SIDECAR_B" "Subscribed to frigate/reviews" 60 \
    || fail "sidecar B never subscribed to frigate/reviews" "$SIDECAR_B"
logs_have "$SIDECAR_B" "fleet_rollcall=full" || fail "sidecar B did not log the full roll-call" "$SIDECAR_B"
logs_have "$SIDECAR_B" "SECURACV_ANNOUNCE_HOST=$ANNOUNCE_HOST_B: the announce names this host" || fail "sidecar B did not log the announce host" "$SIDECAR_B"
pass "sidecar B is up in reviews mode with the roll-call and announce-host opt-ins"

echo "==> Replaying the Frigate reviews fixtures"
for f in "$FIXTURES"/reviews_*.json; do
    mq_pub_file frigate/reviews "$f"
    echo "   published $(basename "$f")"
done
# Three alerts seal; the detection-only update and the face-only alert do not.
EXPECTED_B=3
# What the events view must carry for the sweep to have read anything: the
# exact count with the tolerant parser, nothing to insist on without it.
expect_b="$EXPECTED_B"
[ "$W1_PRESENT" -eq 1 ] || expect_b=0
if [ "$W1_PRESENT" -eq 1 ]; then
    sealed_b=0
    for _ in $(seq 1 60); do
        sealed_b=$(logs_count "$SIDECAR_B" "Event logged")
        [ "$sealed_b" -ge "$EXPECTED_B" ] && break
        sleep 1
    done
    sleep 3
    sealed_b=$(logs_count "$SIDECAR_B" "Event logged")
    [ "$sealed_b" -eq "$EXPECTED_B" ] \
        || fail "reviews mode sealed $sealed_b events, expected exactly $EXPECTED_B (detection-only and face-only items must seal nothing)" "$SIDECAR_B"
    pass "reviews mode sealed exactly $EXPECTED_B alerts; the detection-only and face-only items sealed nothing"
else
    sleep 10
    skip "exact reviews seal count (tolerant parser absent; $(logs_count "$SIDECAR_B" "Event logged") sealed)"
fi

echo "==> Sweeping sidecar B's outputs for the fixtures' sentinels"
curl -fsS -H "Authorization: Bearer $(cap_token "$SIDECAR_B")" "http://127.0.0.1:${PORT_B}/api/sealed-log" > "$SCRATCH/b_sealed_log.json" \
    || fail "GET /api/sealed-log on B failed with the capability token" "$SIDECAR_B"
curl -fsS -H "Authorization: Bearer $(cap_token "$SIDECAR_B")" "http://127.0.0.1:${PORT_B}/events/latest" > "$SCRATCH/b_events_latest.json" \
    || fail "GET /events/latest on B failed with the capability token" "$SIDECAR_B"
events_view "$SIDECAR_B" "$SCRATCH/b_events_view.json" \
    || fail "GET /events?since_bucket=0 inside sidecar B failed (curl with the container's own token)" "$SIDECAR_B"
docker logs "$SIDECAR_B" > "$SCRATCH/b_logs.txt" 2>&1 || true
docker logs "$CAPTURE" > "$SCRATCH/mqtt_capture.txt" 2>&1 || true
sweep_outputs "sidecar B: sealed-log document, events view since bucket zero, /events/latest, MQTT capture, daemon log" \
    "$SCRATCH/b_sealed_log.json" "$SCRATCH/b_events_view.json" "$SCRATCH/b_events_latest.json" \
    "$SCRATCH/mqtt_capture.txt" "$SCRATCH/b_logs.txt"
view_events_b=$(events_in "$SCRATCH/b_events_view.json")
[ "$view_events_b" -ge "$expect_b" ] \
    || fail "the events view inside sidecar B carries $view_events_b events, fewer than the $expect_b sealed — the sweep read nothing"

echo "==> Checking B's announce names the operator's host, and its roll-call is full"
if [ "$W2_PRESENT" -eq 1 ]; then
    announce_b=$(mq_sub -t 'witness_b/+/announce' -C 1 -W 60 || true)
    [ -n "$announce_b" ] || fail "no retained witness_b/+/announce within 60s" "$SIDECAR_B"
    [ "$(printf '%s' "$announce_b" | jq -r '.api.host')" = "$ANNOUNCE_HOST_B" ] || fail "B's announce does not name SECURACV_ANNOUNCE_HOST: $announce_b"
    [ "$(printf '%s' "$announce_b" | jq -c '.api | keys')" = '["bind","host","port","scheme"]' ] || fail "B's announce api keys are off-contract: $announce_b"
    pass "B's announce carries api.host=$ANNOUNCE_HOST_B (and only because the operator set it)"
    card_b=$(pair_card "$SIDECAR_B") || fail "docker exec … pair on B failed" "$SIDECAR_B"
    [ "$(printf '%s\n' "$card_b" | sed -n 3p)" = "Kernel URL to confirm in Home Assistant: http://${ANNOUNCE_HOST_B}:8799" ] \
        || fail "B's pair card does not name the announce host: $(printf '%s\n' "$card_b" | sed -n 3p)"
    pass "B's pair card names http://$ANNOUNCE_HOST_B:8799"
    identity_b=$(curl -fsS -H "Authorization: Bearer $(cap_token "$SIDECAR_B")" "http://127.0.0.1:${PORT_B}/api/identity" || true)
    [ "$(printf '%s' "$identity_b" | jq -r '.fleet_rollcall')" = "full" ] || fail "B's /api/identity fleet_rollcall is not full: $identity_b"
    pass "SECURACV_FLEET_ROLLCALL=1 → /api/identity reports fleet_rollcall full"
    doctor_b=$(docker exec "$SIDECAR_B" doctor) || fail "plain doctor failed on B (a fresh, unpaired install):
$doctor_b"
    grep -qF -- "- paired: no" <<<"$doctor_b" || fail "B's doctor does not report unpaired:
$doctor_b"
    grep -qF -- "kernel URL it is offered: http://${ANNOUNCE_HOST_B}:8799" <<<"$doctor_b" || fail "B's doctor does not name the announce host:
$doctor_b"
    grep -qF -- "fleet roll-call: full" <<<"$doctor_b" || fail "B's doctor does not report the full roll-call:
$doctor_b"
    pass "plain doctor passes on B, a fresh unpaired install with an outstanding code"
else
    skip "B's announce host, roll-call and doctor (kernel has no announce)"
fi
# Two sidecars on one broker must not fight over the broker: each bridge's
# MQTT client id is derived from its kernel's device id, so B's bridges never
# replaced A's sessions. A link that flapped while B was up is that collision
# (the broker closes the older session for every new one with the same id),
# and it is what left A's bridge unable to finish its clean shutdown in time.
if logs_have "$SIDECAR" "Connection closed by peer abruptly"; then
    fail "sidecar A's broker link dropped while sidecar B was up: the two collided on the broker (MQTT client ids not unique per kernel?)" "$SIDECAR"
fi
pass "sidecar A's broker link held while sidecar B shared the broker (client ids unique per kernel)"
docker rm -f "$SIDECAR_B" >/dev/null 2>&1 || true

# ---------------------------------------------------------------------------
echo "==> Checking the HA Discovery config topic is retained"
# Generous window: the publisher polls the event API every 30s.
if discovery=$(mq_sub -t 'homeassistant/#' -C 1 -W 90) && [ -n "$discovery" ]; then
    pass "retained discovery payload present"
else
    fail "no retained homeassistant/# discovery payload found"
fi

echo "==> Pressing the HA verify button (witness/cmd/verify)"
mq_pub -t witness/cmd/verify -m PRESS
chain_state=$(mq_sub -t witness/chain_problem -C 1 -W 60 || true)
case "$chain_state" in
    OFF) pass "chain integrity published: no problem (chain valid)" ;;
    ON) fail "verification ran but reported a chain problem" "$SIDECAR" ;;
    *) fail "no retained witness/chain_problem state after button press" "$SIDECAR" ;;
esac

# ---------------------------------------------------------------------------
echo "==> Clean shutdown: docker stop exits 0 and clears the retained announce"
docker stop -t 30 "$SIDECAR" >/dev/null
exit_code=$(docker inspect -f '{{.State.ExitCode}}' "$SIDECAR")
[ "$exit_code" = "0" ] || fail "docker stop ended the sidecar with exit code $exit_code (SIGTERM was not forwarded cleanly)" "$SIDECAR"
logs_have "$SIDECAR" "shutdown signal received; stopping the daemons" || fail "the entrypoint did not log the forwarded shutdown" "$SIDECAR"
pass "docker stop → exit 0 after a forwarded shutdown"
# Exit 0 alone would also be the entrypoint's bounded fallback (SIGKILL after
# its grace); the daemons' own last lines prove each clean path ran to its end.
logs_have "$SIDECAR" "event_mqtt_bridge stopped" || fail "event_mqtt_bridge did not finish its clean shutdown (announce clear, offline, DISCONNECT)" "$SIDECAR"
logs_have "$SIDECAR" "frigate_bridge stopped" || fail "frigate_bridge did not finish its clean shutdown" "$SIDECAR"
if logs_have "$SIDECAR" "did not exit within"; then
    fail "the entrypoint had to end a daemon: a clean-shutdown path is not bounded" "$SIDECAR"
fi
logs_have "$SIDECAR" "sidecar stopped" || fail "the entrypoint did not log its own clean stop" "$SIDECAR"
pass "every daemon finished its own clean exit before the entrypoint's grace (no SIGKILL fallback)"
if [ "$W2_PRESENT" -eq 1 ]; then
    if leftover=$(mq_sub -v -t "witness/$ANNOUNCE_DEVICE_ID/announce" -C 1 -W 5) && [ -n "$leftover" ]; then
        fail "the retained announce survived a clean shutdown: $leftover"
    fi
    pass "the retained announce is cleared on clean shutdown"
    status_after=$(mq_sub -t witness/status -C 1 -W 5 || true)
    [ "$status_after" = "offline" ] || fail "witness/status after shutdown is '${status_after:-<none>}', expected offline"
    pass "witness/status says offline after shutdown"
else
    skip "announce clear on shutdown (kernel has no announce)"
fi

echo "✅ sidecar e2e passed: bind-all default, fixture replay with no identity leak, verify, roll-call self, signed announce, pairing posture, receipts, doctor split, unpair, announce host + full roll-call opt-ins, discovery, button, clean shutdown"
