#!/usr/bin/env bash
# Places, or after a bundle verifies, the AppImage tools in pins.txt.
#
#   pin.sh place                 download each row, check its sha256, chmod +x,
#                                and put it in tauri-bundler's tools directory
#                                under the row's name; export LDAI_RUNTIME_FILE
#   pin.sh verify <project-path> after `tauri build`: each tool still hashes to
#                                what the bundle leaves (linuxdeploy carries the
#                                bundler's own 3-byte mark, so the bundler read
#                                THIS directory), and every .AppImage under
#                                <project-path>/src-tauri/target carries the
#                                pinned runtime's Build ID
#
# A third argument names another pins table; only the tests pass one. Why this
# exists: action.yml and .github/RELEASE_LESSONS.md 2026-11-04 (c).
set -euo pipefail

mode="${1:?usage: pin.sh place | verify <project-path>}"
project="${2:-}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
table="${3:-$here/pins.txt}"
runtime_name="type2-runtime-x86_64"

if [ "$(uname -m)" != "x86_64" ]; then
  echo "::error::pin-appimage-tools has rows for x86_64 only, and this runner is $(uname -m). Add that architecture's rows to pins.txt (its names come from prepare_tools() in tauri-bundler's linuxdeploy.rs) before bundling an AppImage here: unpinned, the bundler downloads them itself, one from a moving tag."
  exit 1
fi

# tauri-bundler's tools directory (linuxdeploy.rs): dirs::cache_dir()/tauri,
# which is $XDG_CACHE_HOME when that is an absolute path and $HOME/.cache
# otherwise. bundle.useLocalToolsDir would move it under the cargo target dir;
# neither app's tauri.conf.json sets it, and verify fails if the bundler looked
# anywhere else.
if [[ "${XDG_CACHE_HOME:-}" == /* ]]; then
  cache="$XDG_CACHE_HOME"
else
  cache="$HOME/.cache"
fi
dest="$cache/tauri"

rows() {
  local n=0 name sha after url extra
  while read -r name sha after url extra; do
    case "$name" in ''|'#'*) continue ;; esac
    if ! [[ "$sha" =~ ^[0-9a-f]{64}$ ]] || ! [[ "$after" =~ ^([0-9a-f]{64}|=)$ ]] \
       || ! [[ "$url" == https://* ]] || [ -n "$extra" ] || [[ "$name" == */* ]]; then
      echo "::error::$table: malformed row for '$name' (want: name sha256 after-sha256-or-= https-url)" >&2
      return 1
    fi
    printf '%s %s %s %s\n' "$name" "$sha" "$after" "$url"
    n=$((n + 1))
  done < "$table"
  if [ "$n" -eq 0 ]; then
    echo "::error::$table has no rows" >&2
    return 1
  fi
}

sha_of() { sha256sum "$1" | cut -d' ' -f1; }

# The GNU Build ID of the ELF at the front of a file: the runtime itself, or an
# .AppImage, whose first bytes ARE its runtime (appimagetool writes only into
# the runtime's reserved sections, never its build-id note).
build_id() {
  { readelf -n "$1" 2>/dev/null || true; } | awk '/Build ID:/ && id == "" { id = $3 } END { print id }'
}

table_rows="$(rows)"

case "$mode" in
  place)
    mkdir -p "$dest"
    while read -r name sha after url; do
      tmp="$(mktemp "$dest/.pin-XXXXXX")"
      if ! curl -fsSL --retry 3 --retry-all-errors -o "$tmp" "$url"; then
        rm -f "$tmp"
        echo "::error::could not download $url for $name. Nothing was placed for it, so this step stops the bundle rather than let tauri-bundler fetch its own copy."
        exit 1
      fi
      got="$(sha_of "$tmp")"
      if [ "$got" != "$sha" ]; then
        rm -f "$tmp"
        echo "::error::$name: $url has sha256 $got, and ${table##*/} pins $sha. The release asset was replaced or its tag moved. Nothing was placed for it, and the bundle does not run. Re-pin only after finding out what changed upstream (.github/RELEASE_LESSONS.md 2026-11-04 (c))."
        exit 1
      fi
      chmod 0755 "$tmp"
      mv -f "$tmp" "$dest/$name"
      echo "placed $dest/$name (sha256 $sha, from $url)"
    done <<< "$table_rows"
    if [ ! -f "$dest/$runtime_name" ]; then
      echo "::error::$table has no $runtime_name row, so appimagetool would download its own runtime."
      exit 1
    fi
    # appimagetool takes the runtime from here instead of downloading it
    # (linuxdeploy-plugin-appimage passes LDAI_RUNTIME_FILE as --runtime-file).
    if [ -n "${GITHUB_ENV:-}" ]; then
      echo "LDAI_RUNTIME_FILE=$dest/$runtime_name" >> "$GITHUB_ENV"
    fi
    echo "LDAI_RUNTIME_FILE=$dest/$runtime_name"
    ;;

  verify)
    if [ -z "$project" ]; then
      echo "::error::verify needs the Tauri project path (desktop, desktop-lab)."
      exit 1
    fi
    fail=0
    while read -r name sha after url; do
      f="$dest/$name"
      want="$sha"
      [ "$after" = "=" ] || want="$after"
      if [ ! -f "$f" ]; then
        echo "::error::$name is gone from $dest after the bundle."
        fail=1
        continue
      fi
      got="$(sha_of "$f")"
      if [ "$got" = "$want" ]; then
        echo "ok $name (sha256 $got)"
      elif [ "$after" != "=" ] && [ "$got" = "$sha" ]; then
        echo "::error::$name is byte-for-byte as placed: tauri-bundler never ran prepare_tools() on $dest (it zeroes bytes 8-10 of the linuxdeploy it uses), so this AppImage was bundled with tools from somewhere else. Did the tools directory move (bundle.useLocalToolsDir, a CLI bump)?"
        fail=1
      else
        echo "::error::$name in $dest has sha256 $got after the bundle, not $want: something other than the bundler's own mark changed it."
        fail=1
      fi
    done <<< "$table_rows"
    want_id="$(build_id "$dest/$runtime_name")"
    if [ -z "$want_id" ]; then
      echo "::error::no GNU Build ID readable in $dest/$runtime_name (is readelf installed?)."
      exit 1
    fi
    images=()
    if [ -d "$project/src-tauri/target" ]; then
      mapfile -t images < <(find "$project/src-tauri/target" -type f -path '*/bundle/appimage/*.AppImage' | sort)
    fi
    if [ "${#images[@]}" -eq 0 ]; then
      echo "::error::no .AppImage under $project/src-tauri/target/*/bundle/appimage, so there is nothing to verify the runtime of."
      exit 1
    fi
    for img in "${images[@]}"; do
      id="$(build_id "$img")"
      if [ "$id" = "$want_id" ]; then
        echo "ok $img (AppImage runtime build $id, the pinned $runtime_name)"
      else
        echo "::error::$img starts with AppImage runtime build '${id:-none}', not the pinned $runtime_name ($want_id): appimagetool used a runtime of its own (was LDAI_RUNTIME_FILE set for the bundle?)."
        fail=1
      fi
    done
    exit "$fail"
    ;;

  *)
    echo "::error::pin.sh: unknown mode '$mode' (place | verify)"
    exit 2
    ;;
esac
