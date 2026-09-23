#!/usr/bin/env bash
set -euo pipefail
# The SoftAP / STA security REQUEST (WPA2/WPA3 transition only when the build,
# the core and the passphrase allow it; PMF capable, never required; a driver
# refusal reported as WPA2) is canonical at
# firmware/common/network/ap_security_policy.h, host-tested there, consumed
# path-prefixed by the canary (PIO) tree, and carried as a byte-identical
# staged copy next to the canary-wap Arduino sketch. If the two ever differed,
# the two products would put different security on the same setup network —
# the drift the shared header exists to prevent. Edit the canonical file,
# copy it here.
cd "$(dirname "$0")/../.."
A="firmware/common/network/ap_security_policy.h"
B="firmware/projects/canary-wap/arduino/canary_wap/ap_security_policy.h"
if ! diff -u "$A" "$B"; then
  echo "" >&2
  echo "ap_security_policy.h drifted between common/ and the canary-wap sketch." >&2
  echo "Edit $A (the canonical copy) and mirror it: cp $A $B" >&2
  exit 1
fi
echo "ap_security_policy.h: common and canary-wap copies are identical."
