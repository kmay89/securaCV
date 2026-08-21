#!/usr/bin/env bash
# The onboarding pure helpers exist in two places on purpose, for now:
#
#   firmware/common/network/provision_core.h              (canonical, shared —
#                                                          consumed by the
#                                                          common setup portal
#                                                          that sense/vision
#                                                          adopt)
#   firmware/projects/canary-display/include/canary/net/  (the display's copy;
#     provision_core.h                                     its include flips to
#                                                          the common path when
#                                                          the display migrates
#                                                          to the shared portal
#                                                          — see docs/design/
#                                                          onboarding_shared_module.md)
#
# Until that migration, the two files must stay byte-identical — the whole
# LESSONS_LEARNED history of this feature is fixes landing in one portal and
# not the others. A drift here fails CI instead of shipping two truths.
set -euo pipefail
cd "$(dirname "$0")/../.."

A="firmware/common/network/provision_core.h"
B="firmware/projects/canary-display/include/canary/net/provision_core.h"

if ! diff -u "$A" "$B"; then
  echo "" >&2
  echo "provision_core.h drifted between common/ and canary-display." >&2
  echo "Edit $A (the canonical copy) and mirror it to $B verbatim." >&2
  exit 1
fi
echo "provision_core.h: common and canary-display copies are identical."
