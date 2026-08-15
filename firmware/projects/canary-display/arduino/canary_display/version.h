#pragma once
// Unified across all Canary variants: every fw-v* release tags one version
// for every firmware (see docs/firmware_ota.md). The release workflow fails
// if a published binary doesn't contain the tagged version string.
#define CANARY_FW_VERSION "2.4.10"
