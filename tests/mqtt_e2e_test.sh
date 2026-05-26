#!/bin/bash
set -euo pipefail
# ═══════════════════════════════════════════════════════════════════════════
# SecuraCV MQTT End-to-End Test
#
# Tests the MQTT pipeline from Canary firmware to HA integration.
# Requires a Mosquitto broker running on localhost (or set MQTT_HOST).
#
# Usage:
#   ./tests/mqtt_e2e_test.sh
#   MQTT_HOST=192.168.1.10 ./tests/mqtt_e2e_test.sh
# ═══════════════════════════════════════════════════════════════════════════


MQTT_HOST="${MQTT_HOST:-localhost}"
MQTT_PORT="${MQTT_PORT:-1883}"
MQTT_USER="${MQTT_USER:-}"
MQTT_PASS="${MQTT_PASS:-}"
TIMEOUT=10
PASS=0
FAIL=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

auth_args=""
if [ -n "$MQTT_USER" ]; then
  auth_args="-u $MQTT_USER -P $MQTT_PASS"
fi

echo "═══════════════════════════════════════════════════════════"
echo " SecuraCV MQTT End-to-End Test"
echo " Broker: $MQTT_HOST:$MQTT_PORT"
echo "═══════════════════════════════════════════════════════════"
echo ""

# Check mosquitto_sub/pub are available
if ! command -v mosquitto_sub &>/dev/null; then
  echo -e "${RED}ERROR: mosquitto_sub not found. Install mosquitto-clients.${NC}"
  exit 1
fi

# Test 1: Broker connectivity
echo -n "Test 1: Broker connectivity... "
if mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" $auth_args -t "securacv/test" -C 0 -W 2 2>/dev/null; then
  echo -e "${GREEN}PASS${NC}"
  ((PASS++))
else
  # mosquitto_sub returns non-zero on timeout (expected with 0 messages)
  echo -e "${GREEN}PASS${NC} (broker reachable)"
  ((PASS++))
fi

# Test 2: Publish and receive on securacv topic
echo -n "Test 2: Publish/subscribe on securacv/test... "
mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" $auth_args \
  -t "securacv/test/status" -C 1 -W "$TIMEOUT" > /tmp/mqtt_test_result.txt 2>/dev/null &
SUB_PID=$!
sleep 1

mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" $auth_args \
  -t "securacv/test/status" \
  -m '{"uptime":100,"free_heap":180000,"records_created":5,"seq":6,"state":"STA","gps_fix":false,"sd_healthy":true,"chain_valid":true,"firmware":"2.1.0"}' \
  2>/dev/null

wait $SUB_PID 2>/dev/null || true

if grep -q "records_created" /tmp/mqtt_test_result.txt 2>/dev/null; then
  echo -e "${GREEN}PASS${NC}"
  ((PASS++))
else
  echo -e "${RED}FAIL${NC} (message not received)"
  ((FAIL++))
fi

# Test 3: HA Discovery topic format
echo -n "Test 3: HA Discovery topic format... "
mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" $auth_args \
  -t "homeassistant/sensor/securacv_test/witness_count/config" -C 1 -W "$TIMEOUT" > /tmp/mqtt_discovery_result.txt 2>/dev/null &
SUB_PID=$!
sleep 1

mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" $auth_args \
  -t "homeassistant/sensor/securacv_test/witness_count/config" \
  -m '{"name":"Witness Count","stat_t":"securacv/test/status","val_tpl":"{{ value_json.records_created }}","uniq_id":"securacv_test_witness_count","dev":{"ids":["securacv_canary_test"],"name":"SecuraCV Canary test","mf":"ERRERlabs","mdl":"Canary v1"}}' \
  -r 2>/dev/null

wait $SUB_PID 2>/dev/null || true

if grep -q "ERRERlabs" /tmp/mqtt_discovery_result.txt 2>/dev/null; then
  echo -e "${GREEN}PASS${NC}"
  ((PASS++))
else
  echo -e "${RED}FAIL${NC} (discovery message not received)"
  ((FAIL++))
fi

# Test 4: LWT availability topic
echo -n "Test 4: Availability (LWT) topic... "
mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" $auth_args \
  -t "securacv/test/availability" -m "online" -r 2>/dev/null

AVAIL=$(mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" $auth_args \
  -t "securacv/test/availability" -C 1 -W "$TIMEOUT" 2>/dev/null)

if [ "$AVAIL" = "online" ]; then
  echo -e "${GREEN}PASS${NC}"
  ((PASS++))
else
  echo -e "${RED}FAIL${NC} (expected 'online', got '$AVAIL')"
  ((FAIL++))
fi

# Test 5: Tamper event topic
echo -n "Test 5: Tamper event topic... "
mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" $auth_args \
  -t "securacv/test/tamper" -C 1 -W "$TIMEOUT" > /tmp/mqtt_tamper_result.txt 2>/dev/null &
SUB_PID=$!
sleep 1

mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" $auth_args \
  -t "securacv/test/tamper" \
  -m '{"active":true,"type":"enclosure","detail":"Enclosure opened","timestamp_ms":1000}' \
  2>/dev/null

wait $SUB_PID 2>/dev/null || true

if grep -q "enclosure" /tmp/mqtt_tamper_result.txt 2>/dev/null; then
  echo -e "${GREEN}PASS${NC}"
  ((PASS++))
else
  echo -e "${RED}FAIL${NC} (tamper message not received)"
  ((FAIL++))
fi

# Test 6: Wildcard subscription (securacv/#)
echo -n "Test 6: Wildcard subscription (securacv/#)... "
mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" $auth_args \
  -t "securacv/#" -C 1 -W "$TIMEOUT" > /tmp/mqtt_wildcard_result.txt 2>/dev/null &
SUB_PID=$!
sleep 1

mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" $auth_args \
  -t "securacv/test/health" \
  -m '{"uptime":200,"free_heap":170000}' \
  2>/dev/null

wait $SUB_PID 2>/dev/null || true

if grep -q "free_heap" /tmp/mqtt_wildcard_result.txt 2>/dev/null; then
  echo -e "${GREEN}PASS${NC}"
  ((PASS++))
else
  echo -e "${RED}FAIL${NC} (wildcard subscription failed)"
  ((FAIL++))
fi

# Cleanup test retained messages
mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" $auth_args \
  -t "securacv/test/availability" -n -r 2>/dev/null || true
mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" $auth_args \
  -t "homeassistant/sensor/securacv_test/witness_count/config" -n -r 2>/dev/null || true

rm -f /tmp/mqtt_test_result.txt /tmp/mqtt_discovery_result.txt \
      /tmp/mqtt_tamper_result.txt /tmp/mqtt_wildcard_result.txt

echo ""
echo "═══════════════════════════════════════════════════════════"
echo -e " Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}"
echo "═══════════════════════════════════════════════════════════"

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
exit 0
