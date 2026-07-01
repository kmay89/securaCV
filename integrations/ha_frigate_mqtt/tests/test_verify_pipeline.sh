#!/usr/bin/env bash
#
# Regression tests for verify_pipeline.sh's false-pass hardening. A docker
# shim on PATH emulates the compose stack so each historical false-pass
# scenario can be replayed deterministically:
#   1. healthy stack            -> must pass
#   2. stale logs, wedged bridge -> must FAIL (used to pass on old log lines)
#   3. silent broker            -> must FAIL
# It also pins that the MQTT subscribe excludes retained messages (-R).
#
#   bash integrations/ha_frigate_mqtt/tests/test_verify_pipeline.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="$HERE/../verify_pipeline.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/bin"
cat > "$TMP/bin/docker" <<'SHIM'
#!/usr/bin/env bash
# docker-compose shim: behavior is driven by files in $SCENARIO_DIR.
#   sub_output    - what mosquitto_sub prints (empty/missing = silent broker)
#   bridge_alive  - if present, a publish appends the ingest log line and
#                   touches db_mtime (a live, healthy bridge)
#   logs          - the securacv container log
set -euo pipefail
[ "${1:-}" = "compose" ] && shift
case "${1:-}" in
  version) exit 0 ;;
  logs)
    cat "$SCENARIO_DIR/logs" 2>/dev/null || true
    exit 0 ;;
  exec)
    shift
    [ "${1:-}" = "-T" ] && shift
    svc="$1"; shift
    tool="$1"
    case "$svc:$tool" in
      mosquitto:mosquitto_sub)
        printf '%s\n' "$@" > "$SCENARIO_DIR/sub_args"
        cat "$SCENARIO_DIR/sub_output" 2>/dev/null || true
        exit 0 ;;
      mosquitto:mosquitto_pub)
        if [ -f "$SCENARIO_DIR/bridge_alive" ]; then
          # A live bridge ingests the published event: log its zone (the
          # nonce rides in the camera field) and write the database.
          msg=""
          for arg in "$@"; do msg="$arg"; done
          zone=$(printf '%s' "$msg" | sed -n 's/.*"camera":"\([^"]*\)".*/\1/p')
          echo "Event logged: BoundaryCrossingObjectLarge zone=zone:$zone conf=0.92" \
            >> "$SCENARIO_DIR/logs"
          touch "$SCENARIO_DIR/db_mtime"
        fi
        exit 0 ;;
      securacv:sh)
        shift; [ "${1:-}" = "-c" ] && shift
        cmd="$1"
        case "$cmd" in
          *touch*)
            touch "$SCENARIO_DIR/marker"
            exit 0 ;;
          *find*)
            if [ -f "$SCENARIO_DIR/db_mtime" ] && \
               [ "$SCENARIO_DIR/db_mtime" -nt "$SCENARIO_DIR/marker" ]; then
              exit 0
            fi
            exit 1 ;;
          *"test -s"*)
            # A bare existence check (the old script's semantics): passes on
            # any leftover non-empty database, fresh or stale.
            [ -f "$SCENARIO_DIR/db_exists" ] && exit 0
            exit 1 ;;
        esac
        exit 1 ;;
    esac
    exit 1 ;;
esac
exit 1
SHIM
chmod +x "$TMP/bin/docker"

run_scenario() {
  local name="$1"
  export SCENARIO_DIR="$TMP/$name"
  mkdir -p "$SCENARIO_DIR"
  if PATH="$TMP/bin:$PATH" VERIFY_PIPELINE_INGEST_RETRIES=2 \
    bash "$SCRIPT" > "$SCENARIO_DIR/out" 2>&1; then
    return 0
  else
    return 1
  fi
}

fail() {
  echo "not ok: $1" >&2
  [ -n "${SCENARIO_DIR:-}" ] && cat "$SCENARIO_DIR/out" >&2
  exit 1
}

# 1. Healthy stack: live broker output, live bridge -> pass.
mkdir -p "$TMP/healthy"
echo '{"after":{"camera":"front_door"}}' > "$TMP/healthy/sub_output"
touch "$TMP/healthy/bridge_alive"
run_scenario healthy || fail "healthy stack must pass"
echo "ok: healthy stack passes"

# 1b. The subscribe must exclude retained messages.
grep -q -- '-R' "$TMP/healthy/sub_args" \
  || fail "mosquitto_sub must be called with -R (exclude retained)"
echo "ok: retained messages excluded from the MQTT check"

# 2. Stale stack, wedged bridge: an old "Event logged" line and a leftover
#    non-empty database exist, but nothing ingests the nonce event and the
#    db is never written during the run. The old script passed on exactly
#    this state; the hardened one must fail.
mkdir -p "$TMP/stale"
echo '{"after":{"camera":"front_door"}}' > "$TMP/stale/sub_output"
echo "Event logged: BoundaryCrossingObjectLarge zone=zone:porch conf=0.92" > "$TMP/stale/logs"
touch "$TMP/stale/db_exists"
# no bridge_alive
if run_scenario stale; then
  fail "stale logs with a wedged bridge must not pass"
fi
echo "ok: stale log lines no longer produce a false pass"

# 3. Silent broker: no live frigate/events publish -> fail.
mkdir -p "$TMP/silent"
touch "$TMP/silent/bridge_alive"
if run_scenario silent; then
  fail "a silent broker must not pass"
fi
echo "ok: silent broker fails"

echo "all verify_pipeline tests passed"
