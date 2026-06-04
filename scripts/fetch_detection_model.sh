#!/usr/bin/env bash
#
# Fetch and verify the object-detection model used by witnessd's tract backend.
#
# This is a ONE-TIME, operator-initiated step — not a runtime download. The kernel itself
# performs no network I/O to obtain models, preserving local-only custody (Invariant IV).
# Run this once, then enable object detection with `detect.backend = "tract"`; the model
# path defaults to the location written below, so you do NOT need to set detect.tract_model.
#
#   bash scripts/fetch_detection_model.sh
#
# Idempotent: if the model already exists and matches the pinned SHA-256, it does nothing.
# Requires a build with `--features backend-tract` to actually run the tract backend.
set -euo pipefail

# Pinned model: SSDLite MobileNet v2 (ONNX Model Zoo, Apache-2.0). Update both together.
MODEL_URL="https://github.com/onnx/models/raw/main/vision/object_detection_segmentation/ssdlite_mobilenet_v2/model/ssdlite_mobilenet_v2_12.onnx"
MODEL_SHA256="ad6303f1ca2c3dcc0d86a87c36892be9b97b02a0105faa5cc3cfae79a2b11a31"

# Resolve the repo root from this script's location so it works from any CWD.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODEL_DIR="${REPO_ROOT}/vendor/models"
MODEL_PATH="${MODEL_DIR}/ssdlite_mobilenet_v2_12.onnx"

sha256_of() {
  # Print the SHA-256 of "$1", portable across sha256sum (Linux) and shasum (macOS).
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    echo "error: need 'sha256sum' or 'shasum' to verify the model" >&2
    exit 1
  fi
}

if [ -f "${MODEL_PATH}" ] && [ "$(sha256_of "${MODEL_PATH}")" = "${MODEL_SHA256}" ]; then
  echo "Model already present and verified: ${MODEL_PATH}"
  exit 0
fi

mkdir -p "${MODEL_DIR}"
tmp="$(mktemp "${MODEL_DIR}/.ssdlite.XXXXXX")"
trap 'rm -f "${tmp}"' EXIT

echo "Downloading detection model -> ${MODEL_PATH}"
if command -v curl >/dev/null 2>&1; then
  curl -fSL "${MODEL_URL}" -o "${tmp}"
elif command -v wget >/dev/null 2>&1; then
  wget -qO "${tmp}" "${MODEL_URL}"
else
  echo "error: need 'curl' or 'wget' to download the model" >&2
  exit 1
fi

actual="$(sha256_of "${tmp}")"
if [ "${actual}" != "${MODEL_SHA256}" ]; then
  echo "error: SHA-256 mismatch — refusing to install a model that does not match the pin" >&2
  echo "  expected ${MODEL_SHA256}" >&2
  echo "  actual   ${actual}" >&2
  exit 1
fi

mv "${tmp}" "${MODEL_PATH}"
trap - EXIT
echo "Verified and installed: ${MODEL_PATH}"
echo "Enable object detection with: detect.backend = \"tract\"  (build with --features backend-tract)"
