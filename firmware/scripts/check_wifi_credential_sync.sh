#!/usr/bin/env bash
set -euo pipefail
# The home Wi-Fi credential rule (SSID 1..32 bytes, password 0..64 bytes, NO
# minimum, open network accepted) and the words every setup surface shows
# beside the field are canonical at
# firmware/common/network/wifi_credential_policy.h, host-tested there
# (firmware/tests_host/test_wifi_credential_policy.cpp), consumed path-prefixed
# by the canary (PIO) tree and the shared setup portal, staged into the
# canary-display sketch by setup.sh regen, and carried as a byte-identical
# staged copy next to the canary-wap Arduino sketch. If the two ever differed,
# two products would judge the same password differently — the drift the
# shared header exists to prevent. Edit the canonical file, copy it here.
cd "$(dirname "$0")/../.."
A="firmware/common/network/wifi_credential_policy.h"
B="firmware/projects/canary-wap/arduino/canary_wap/wifi_credential_policy.h"
if ! diff -u "$A" "$B"; then
  echo "" >&2
  echo "wifi_credential_policy.h drifted between common/ and the canary-wap sketch." >&2
  echo "Edit $A (the canonical copy) and mirror it: cp $A $B" >&2
  exit 1
fi
echo "wifi_credential_policy.h: common and canary-wap copies are identical."
