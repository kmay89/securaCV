#!/usr/bin/env bash
set -euo pipefail

failures=0
export_bundle_path=""

compose_cmd=(docker compose)
if ! docker compose version >/dev/null 2>&1; then
  if command -v docker-compose >/dev/null 2>&1; then
    compose_cmd=(docker-compose)
  else
    echo "❌ docker compose is required but not available." >&2
    exit 1
  fi
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

mqtt_step() {
  local output
  output=$("${compose_cmd[@]}" exec -T mosquitto sh -c "mosquitto_sub -t 'frigate/events' -C 1 -W 15" 2>/dev/null || true)
  if [[ -n "$output" ]]; then
    printf '%s\n' "$output" | head -n 1
    return 0
  fi
  return 1
}

python_cmd() {
  "${compose_cmd[@]}" exec -T securacv sh -c 'command -v python3 >/dev/null 2>&1 && echo python3 || command -v python >/dev/null 2>&1 && echo python'
}

query_count() {
  local sql=$1
  local py
  py=$(python_cmd | tr -d '\r')
  if [[ -z "$py" ]]; then
    echo "python not available in securacv container" >&2
    return 1
  fi
  "${compose_cmd[@]}" exec -T securacv sh -c "$py - <<'PY'
import sqlite3
conn = sqlite3.connect('/data/witness.db')
count = conn.execute(\"$sql\").fetchone()[0]
print(count)
PY"
}

check_ingest() {
  local count
  count=$(query_count "SELECT COUNT(*) FROM sealed_events" 2>/dev/null || true)
  if [[ -n "$count" && "$count" -ge 1 ]]; then
    echo "sealed_events: $count"
    return 0
  fi
  return 1
}

check_db_records() {
  if ! "${compose_cmd[@]}" exec -T securacv sh -c "test -s /data/witness.db"; then
    return 1
  fi
  local count
  count=$(query_count "SELECT COUNT(*) FROM checkpoints" 2>/dev/null || true)
  if [[ -n "$count" && "$count" -ge 1 ]]; then
    echo "checkpoints: $count"
    return 0
  fi
  return 1
}

check_vault_envelopes() {
  local count
  count=$("${compose_cmd[@]}" exec -T securacv sh -c "find /data/vault/envelopes -type f 2>/dev/null | wc -l" || true)
  if [[ -n "$count" && "$count" -ge 1 ]]; then
    echo "envelopes: $count"
    return 0
  fi
  return 1
}

check_export_bundle() {
  export_bundle_path=$("${compose_cmd[@]}" exec -T securacv sh -c "find /data -maxdepth 3 -type f -name '*export*bundle*.json' -print -quit" || true)
  if [[ -n "$export_bundle_path" ]]; then
    echo "bundle: $export_bundle_path"
    return 0
  fi
  return 1
}

check_verify_cli() {
  if [[ -n "$export_bundle_path" ]]; then
    "${compose_cmd[@]}" exec -T securacv export_verify --db /data/witness.db "$export_bundle_path"
    return $?
  fi
  "${compose_cmd[@]}" exec -T securacv log_verify --db /data/witness.db
}

step "Confirm MQTT publishes Frigate events" mqtt_step
step "Confirm SecuraCV ingests at least one event" check_ingest
step "Confirm SecuraCV DB exists and has records" check_db_records
step "Confirm vault sealing created at least one envelope" check_vault_envelopes
step "Confirm export bundle exists" check_export_bundle
step "Confirm verification CLI succeeds" check_verify_cli

if [[ $failures -ne 0 ]]; then
  echo "Verification failed: ${failures} step(s) did not pass." >&2
  exit 1
fi

echo "All verification steps passed."
exit 0
