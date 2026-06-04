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
#
# HOST-ONLY: this model runs in tract on a Raspberry Pi / x86 host (witnessd). It does NOT run
# on the ESP32-S3 — that path uses the Grove Vision AI V2 (SSCMA) board for on-device inference
# (see firmware/projects/canary-vision). A 60 MB ONNX model cannot fit on an MCU, and tract does
# not target microcontrollers.
set -euo pipefail

# Pinned model: tiny-YOLOv2 (ONNX Model Zoo, VOC, MIT). Raw-grid output is decoded + NMS'd on
# the host (detect.tract_format = "yolov2", the default). Chosen because tract can run it — the
# TF-exported SSD/SSDLite models use ONNX `Loop` ops for in-graph NMS that tract does not
# implement. Update URL and SHA together.
MODEL_URL="https://github.com/onnx/models/raw/main/validated/vision/object_detection_segmentation/tiny-yolov2/model/tinyyolov2-8.onnx"
MODEL_SHA256="583fb7fdc948435ceac9fa82efc7708701efe8382a859a3dd46526b155f5f2ae"

# Resolve the repo root from this script's location so it works from any CWD.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODEL_DIR="${REPO_ROOT}/vendor/models"
MODEL_PATH="${MODEL_DIR}/tinyyolov2-8.onnx"

# Validate dependencies up front, before downloading anything. (sha256_of runs inside a
# command substitution, so an exit there would only kill the subshell — check here instead.)
if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
  echo "error: need 'sha256sum' or 'shasum' to verify the model" >&2
  exit 1
fi
if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
  echo "error: need 'curl' or 'wget' to download the model" >&2
  exit 1
fi

sha256_of() {
  # Print the SHA-256 of "$1", portable across sha256sum (Linux) and shasum (macOS).
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

if [ -f "${MODEL_PATH}" ] && [ "$(sha256_of "${MODEL_PATH}")" = "${MODEL_SHA256}" ]; then
  echo "Model already present and verified: ${MODEL_PATH}"
  exit 0
fi

mkdir -p "${MODEL_DIR}"
tmp="$(mktemp "${MODEL_DIR}/.tinyyolov2.XXXXXX")"
trap 'rm -f "${tmp}"' EXIT

echo "Downloading detection model -> ${MODEL_PATH}"
if command -v curl >/dev/null 2>&1; then
  curl -fSL "${MODEL_URL}" -o "${tmp}"
else
  wget -qO "${tmp}" "${MODEL_URL}"
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
