#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$SCRIPT_DIR/securacv_scanner"
DIST_DIR="$SCRIPT_DIR/dist"

# The scanner app is SHELVED — it depends on furi_hal_bt_start_observer(), a
# function that exists in NO official / Unleashed / Momentum firmware, so ufbt
# cannot compile it (securacv_scanner/README.md). This script is kept for
# reference; surface that up front so nobody installs four SDKs for a build
# that cannot succeed.
shelved_banner() {
  cat >&2 <<'EOF'
============================================================================
  SHELVED — NOT BUILDABLE. The securacv_scanner app uses
  furi_hal_bt_start_observer(), which exists in no official, Unleashed, or
  Momentum firmware. `ufbt` WILL fail at the compile step for every target
  below. This script is kept for reference only — see
  tools/flipper/securacv_scanner/README.md.
============================================================================
EOF
}

usage() {
  shelved_banner
  cat <<EOF
SecuraCV Flipper Zero FAP Build Script  [SHELVED — NOT BUILDABLE]

Usage: $0 [firmware]

Firmware targets:
  official     Official Flipper firmware (release channel) [default]
  dev          Official Flipper firmware (dev channel)
  unleashed    Unleashed firmware (DarkFlippers)
  momentum     Momentum firmware (Next-Flip)
  all          Build for all targets

Prerequisites:
  pip install ufbt

Examples:
  $0              # Build for official release
  $0 unleashed    # Build for Unleashed
  $0 all          # Build for all 4 firmware variants
EOF
  exit 0
}

build_target() {
  local target="$1"
  local label="$2"

  echo "=== Building for $label ==="

  case "$target" in
    official)
      ufbt update --channel=release
      ;;
    dev)
      ufbt update --channel=dev
      ;;
    unleashed)
      echo "Note: Unleashed requires manual SDK setup. Trying latest release index..."
      ufbt update --index-url="https://up.unleashedflip.com/directory.json" || \
        { echo "Unleashed SDK unavailable. Build with official SDK instead."; ufbt update --channel=release; }
      ;;
    momentum)
      echo "Note: Momentum requires manual SDK setup. Trying latest release index..."
      ufbt update --index-url="https://up.momentum-fw.dev/firmware/directory.json" || \
        { echo "Momentum SDK unavailable. Build with official SDK instead."; ufbt update --channel=release; }
      ;;
    *)
      echo "Unknown target: $target" >&2
      exit 1
      ;;
  esac

  cd "$APP_DIR"
  ufbt

  mkdir -p "$DIST_DIR"
  cp "$APP_DIR/dist/securacv_scanner.fap" "$DIST_DIR/securacv_scanner-${target}.fap"
  echo "  -> $DIST_DIR/securacv_scanner-${target}.fap"
  echo ""
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
fi

TARGET="${1:-official}"

# Loud up front for real invocations too — the compile below cannot succeed.
shelved_banner

if ! command -v ufbt &>/dev/null; then
  echo "Error: ufbt not found." >&2
  # Check common pip user-install locations
  PIP_BIN=""
  for candidate in \
    "$HOME/Library/Python/3.*/bin" \
    "$HOME/.local/bin"; do
    # shellcheck disable=SC2086
    for dir in $candidate; do
      if [[ -x "$dir/ufbt" ]]; then
        PIP_BIN="$dir"
        break 2
      fi
    done
  done

  if [[ -n "$PIP_BIN" ]]; then
    echo "" >&2
    echo "Found ufbt at: $PIP_BIN/ufbt" >&2
    echo "It is not on your PATH. Fix with:" >&2
    echo "" >&2
    echo "  export PATH=\"$PIP_BIN:\$PATH\"" >&2
    echo "" >&2
    echo "To make it permanent, add that line to your shell profile (~/.zshrc or ~/.bashrc)." >&2
  else
    echo "Install with: pip install ufbt" >&2
    echo "If already installed, ensure the pip scripts directory is on your PATH." >&2
  fi
  exit 1
fi

if [[ "$TARGET" == "all" ]]; then
  build_target official "Official (Release)"
  build_target dev "Official (Dev)"
  build_target unleashed "Unleashed"
  build_target momentum "Momentum"
  echo "=== All builds complete ==="
  echo "FAP files in: $DIST_DIR/"
  ls -la "$DIST_DIR/"*.fap
else
  case "$TARGET" in
    official)   build_target official "Official (Release)" ;;
    dev)        build_target dev "Official (Dev)" ;;
    unleashed)  build_target unleashed "Unleashed" ;;
    momentum)   build_target momentum "Momentum" ;;
    *)          echo "Unknown target: $TARGET. Use: official, dev, unleashed, momentum, all" >&2; exit 1 ;;
  esac
fi
