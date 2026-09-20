#!/usr/bin/env bash
set -euo pipefail
# The MQTT broker transport DECISION (plain / TLS with a verified CA / pinned
# fingerprint / the explicit lab opt-in, and fail-closed for everything
# incomplete) is canonical at firmware/common/network/mqtt_transport_logic.h,
# consumed path-prefixed by canary-display / -sense / -vision, and carried as
# a byte-identical staged copy next to the canary-wap Arduino sketch (whose
# esp_mqtt bridge applies the same decision). If the two ever differed, one
# product would refuse a provisioning another accepts — the exact drift the
# shared header exists to prevent. Edit the canonical file, copy it here.
cd "$(dirname "$0")/../.."
A="firmware/common/network/mqtt_transport_logic.h"
B="firmware/projects/canary-wap/arduino/canary_wap/mqtt_transport_logic.h"
if ! diff -u "$A" "$B"; then
  echo "" >&2
  echo "mqtt_transport_logic.h drifted between common/ and the canary-wap sketch." >&2
  echo "Edit $A (the canonical copy) and mirror it: cp $A $B" >&2
  exit 1
fi
echo "mqtt_transport_logic.h: common and canary-wap copies are identical."
