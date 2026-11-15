#!/usr/bin/env bash
# One desktop app's THIRD_PARTY_LICENSES.html: the notices its installers owe.
#
#   .github/scripts/third_party_notices.sh flasher|lab <out.html>
#
# Two parts, one file:
#   1. every Rust crate compiled into the app for the targets that ship, with
#      its license text: `cargo about generate` over the app's Cargo.lock,
#      configured by <app dir>/about.toml (whose accepted list is deny.toml's)
#      and rendered with third_party_notices.hbs. A crate under a license
#      nobody accepted fails here.
#   2. the programs bundled beside the app, which no Cargo.lock of ours names:
#      espflash (both apps), and for the Flasher rpiboot and libusb (macOS).
#      Each license file is fetched at the ref the release pins and included
#      verbatim. The Flasher's mass-storage-gadget64 boot image is named, with
#      where usbboot says its source is, and no more: it is a Linux system
#      image whose components carry their own licenses (usbboot's Apache-2.0
#      LICENSE is what rpiboot is under), and their texts are not reproduced.
#
# Pins: ESPFLASH_VERSION comes from .github/espflash-pins.env (the file the
# build reads); the Flasher's USBBOOT_SHA and LIBUSB_VERSION come from the
# environment (desktop-flasher-release.yml's `env:`). A missing pin, a failed
# fetch or an empty license file stops the script: a release with half its
# notices is not one this writes.
#
# Run by the `notices` job of desktop-flasher-release.yml and
# desktop-release.yml, whose finalize jobs attach the file to the release.
# Needs cargo-about, curl, sed and awk; runs from the repository root.
set -euo pipefail

app="${1:-}" out="${2:-}"
case "$app" in
  flasher) dir=desktop     name="SecuraCV Flasher" ;;
  lab)     dir=desktop-lab name="SecuraCV Lab" ;;
  *) echo "usage: $0 flasher|lab <out.html>" >&2; exit 2 ;;
esac
[ -n "$out" ] || { echo "usage: $0 flasher|lab <out.html>" >&2; exit 2; }

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fail() { echo "::error::third_party_notices.sh: $*" >&2; exit 1; }

espflash="$(sed -n 's/^ESPFLASH_VERSION=//p' "$root/.github/espflash-pins.env")"
[[ "$espflash" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] \
  || fail "no x.y.z ESPFLASH_VERSION in .github/espflash-pins.env"
if [ "$app" = flasher ]; then
  [[ "${USBBOOT_SHA:-}" =~ ^[0-9a-f]{40}$ ]] || fail "USBBOOT_SHA is not a commit id"
  [[ "${LIBUSB_VERSION:-}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "LIBUSB_VERSION is not an x.y.z version"
fi

# ── 1. the crates ────────────────────────────────────────────────────────────
cargo about generate --locked \
  --manifest-path "$root/$dir/src-tauri/Cargo.toml" \
  --config "$root/$dir/about.toml" \
  --output-file "$work/crates.html" \
  "$here/third_party_notices.hbs"
[ "$(grep -c '<!-- bundled-programs -->' "$work/crates.html")" = 1 ] \
  || fail "third_party_notices.hbs must carry the bundled-programs marker exactly once"
grep -q 'class="license"' "$work/crates.html" \
  || fail "cargo-about listed no license at all for $dir/src-tauri/Cargo.toml"

# ── 2. the bundled programs ──────────────────────────────────────────────────
# program <heading> <one HTML sentence saying what it is and where it is from>
#         then pairs of <label> <url>: each url's text, escaped, under its label.
program() {
  local heading="$1" blurb="$2" label url n=0; shift 2
  printf '        <h3>%s</h3>\n        <p>%s</p>\n' "$heading" "$blurb"
  while [ $# -gt 0 ]; do
    label="$1" url="$2"; shift 2; n=$((n + 1))
    curl -fsSL --retry 3 --retry-delay 5 "$url" -o "$work/text" || fail "could not fetch $url"
    [ -s "$work/text" ] || fail "$url is empty"
    printf '        <h4>%s</h4>\n        <pre class="license-text">' "$label"
    sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g' "$work/text"
    printf '</pre>\n'
  done
  [ "$n" -gt 0 ] || fail "$heading: no license file named"
}

raw=https://raw.githubusercontent.com
{
  program "espflash ${espflash}" \
    "The flash engine, bundled as a separate program. MIT or Apache-2.0, at your option. Source: <a href=\"https://github.com/esp-rs/espflash/tree/v${espflash}\">esp-rs/espflash v${espflash}</a>." \
    "LICENSE-MIT" "$raw/esp-rs/espflash/v${espflash}/LICENSE-MIT" \
    "LICENSE-APACHE" "$raw/esp-rs/espflash/v${espflash}/LICENSE-APACHE"
  if [ "$app" = flasher ]; then
    program "rpiboot and the mass-storage-gadget64 boot image (Raspberry Pi usbboot)" \
      "Bundled as a separate program and the boot image it sends to the board, both taken from <a href=\"https://github.com/raspberrypi/usbboot/tree/${USBBOOT_SHA}\">raspberrypi/usbboot at ${USBBOOT_SHA}</a>. rpiboot is licensed under Apache-2.0: usbboot's LICENSE, below. The boot image is not one program under that one license. usbboot's mass-storage-gadget64/README.md calls it a bootloader image that loads a Linux initramfs, so it is a Linux system image whose components carry their own licenses, the Linux kernel's GPL-2.0 among them. Their license texts are not reproduced in this file; usbboot names the image's source as the <a href=\"https://github.com/raspberrypi/buildroot/tree/mass-storage-gadget64\">mass-storage-gadget64 branch of raspberrypi/buildroot</a>." \
      "LICENSE" "$raw/raspberrypi/usbboot/${USBBOOT_SHA}/LICENSE"
    program "libusb ${LIBUSB_VERSION} (macOS installers)" \
      "Bundled as a separate dynamic library, libusb-1.0.0.dylib, which rpiboot loads. LGPL-2.1-or-later. Built from the release tarball: <a href=\"https://github.com/libusb/libusb/releases/tag/v${LIBUSB_VERSION}\">libusb/libusb v${LIBUSB_VERSION}</a>." \
      "COPYING" "$raw/libusb/libusb/v${LIBUSB_VERSION}/COPYING"
  fi
} > "$work/programs.html"

# ── splice, name the app, and prove what was written ─────────────────────────
awk -v programs="$work/programs.html" '
  /<!-- bundled-programs -->/ { while ((getline line < programs) > 0) print line; next }
  { print }' "$work/crates.html" | sed "s/@APP@/${name}/g" > "$work/out.html"
grep -q "<h3>espflash ${espflash}</h3>" "$work/out.html" || fail "the espflash section did not land"
! grep -q '@APP@\|<!-- bundled-programs -->' "$work/out.html" || fail "a placeholder survived"
mkdir -p "$(dirname "$out")"
cp "$work/out.html" "$out"
echo "wrote $out: $(grep -c 'class="license"' "$out") crate license texts, $(grep -c '<h3>' "$work/programs.html") bundled program(s)"
