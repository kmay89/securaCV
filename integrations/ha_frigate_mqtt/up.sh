#!/usr/bin/env bash
#
# up.sh — bring the local Home Assistant + Frigate + Mosquitto + SecuraCV
# stack up in one command, wait until each service answers, then run
# verify_pipeline.sh (the acceptance check) against it.
#
#   ./up.sh                 # build + start + wait + verify
#   ./up.sh --no-verify     # build + start + wait only
#   ./up.sh --timeout 600   # seconds to wait for each service (default 300)
#
# What it will NOT do for you: choose the broker password. The Mosquitto
# password file is created interactively (README "Quickstart" step 1) so the
# password never lands in a script, a shell history line or a process list.
# up.sh checks the file exists and stops with that exact command if not.
#
# Exit codes: 0 stack up (and verified, unless --no-verify); 1 a precondition
# failed or a service never answered; otherwise verify_pipeline.sh's code.
#
# Linted by shellcheck in .github/workflows/docker-sidecar.yml. Running it
# needs Docker, which CI's lint job does not start — the run is an
# operator's (ENTERPRISE_READINESS_TODO.md §6, "Make integration runnable in
# one command").
set -euo pipefail

cd "$(dirname "$0")"

VERIFY=1
TIMEOUT=300
while [ $# -gt 0 ]; do
  case "$1" in
    --no-verify) VERIFY=0; shift ;;
    --timeout)
      [ $# -ge 2 ] || { echo "--timeout needs a number of seconds" >&2; exit 1; }
      TIMEOUT="$2"; shift 2 ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1 (try --help)" >&2; exit 1 ;;
  esac
done
case "$TIMEOUT" in
  ''|*[!0-9]*) echo "--timeout must be a whole number of seconds" >&2; exit 1 ;;
esac

compose_cmd=(docker compose)
if ! docker compose version >/dev/null 2>&1; then
  if command -v docker-compose >/dev/null 2>&1; then
    compose_cmd=(docker-compose)
  else
    echo "❌ docker compose is required but not available." >&2
    exit 1
  fi
fi

# The compose file requires SECURACV_MQTT_PASSWORD (from the shell or .env).
if [ -z "${SECURACV_MQTT_PASSWORD:-}" ]; then
  if [ -f .env ] && grep -q '^SECURACV_MQTT_PASSWORD=.' .env; then
    SECURACV_MQTT_PASSWORD="$(grep '^SECURACV_MQTT_PASSWORD=' .env | tail -n1 | cut -d= -f2-)"
  else
    echo "❌ SECURACV_MQTT_PASSWORD is not set in the shell or in .env." >&2
    echo "   Follow README.md Quickstart step 1, then add it to .env." >&2
    exit 1
  fi
fi
if [ "$SECURACV_MQTT_PASSWORD" = "change-me" ]; then
  echo "❌ .env still holds the example password (change-me). Pick your own (README step 1)." >&2
  exit 1
fi

# Mosquitto exits at start when its configured password_file is missing.
if ! "${compose_cmd[@]}" run --rm --no-deps --entrypoint sh mosquitto \
       -c 'test -s /mosquitto/config/passwd' >/dev/null 2>&1; then
  echo "❌ No Mosquitto password file yet. Create it first (you will be prompted):" >&2
  echo "" >&2
  echo "   ${compose_cmd[*]} run --rm --no-deps --entrypoint sh mosquitto -c \\" >&2
  echo "     'mosquitto_passwd -c /mosquitto/config/passwd securacv &&" >&2
  echo "      chown mosquitto:mosquitto /mosquitto/config/passwd &&" >&2
  echo "      chmod 600 /mosquitto/config/passwd'" >&2
  exit 1
fi

echo "▶ Building and starting the stack…"
"${compose_cmd[@]}" up -d --build

# wait_for_port <name> <port>: poll 127.0.0.1:<port> (all four services bind
# to localhost only) until it accepts a TCP connection or TIMEOUT runs out.
wait_for_port() {
  local name="$1" port="$2" waited=0
  printf '⏳ Waiting for %s on 127.0.0.1:%s' "$name" "$port"
  until (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; do
    if [ "$waited" -ge "$TIMEOUT" ]; then
      echo ""
      echo "❌ $name did not answer on port $port within ${TIMEOUT}s." >&2
      echo "   ${compose_cmd[*]} logs --tail 50 $name" >&2
      return 1
    fi
    sleep 5
    waited=$((waited + 5))
    printf '.'
  done
  echo " up (${waited}s)"
}

wait_for_port mosquitto 1883
wait_for_port frigate 5000
wait_for_port homeassistant 8123

# A port that accepts a connection can still be starting up; ask Frigate for
# its version when curl is available (Home Assistant's onboarding page needs
# a browser anyway).
if command -v curl >/dev/null 2>&1; then
  if curl -fsS --max-time 10 http://127.0.0.1:5000/api/version >/dev/null; then
    echo "✅ Frigate API answers."
  else
    echo "⚠️  Frigate's port is open but /api/version did not answer yet." >&2
  fi
fi

echo ""
echo "Home Assistant: http://localhost:8123"
echo "Frigate:        http://localhost:5000"

if [ "$VERIFY" -eq 0 ]; then
  echo "Skipping verify_pipeline.sh (--no-verify)."
  exit 0
fi

echo ""
echo "▶ Running verify_pipeline.sh…"
MQTT_USER=securacv MQTT_PASS="$SECURACV_MQTT_PASSWORD" ./verify_pipeline.sh
