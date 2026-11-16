# WebServer 2.0.17+securacv.1

The `WebServer` library of [arduino-esp32](https://github.com/espressif/arduino-esp32)
**2.0.17** (tag `2.0.17`, `libraries/WebServer/`), copied byte for byte, with
one fix backported: [GHSA-8cmm-3887-r32j](https://github.com/espressif/arduino-esp32/security/advisories/GHSA-8cmm-3887-r32j)
(CVE-2026-42854, critical). It exists for the core-2 builds only, and it goes
away when the last of them moves to core 3.

## Why it is here

In 2.0.17, `WebServer::_parseRequest` takes the multipart boundary from the
client's `Content-Type` header with no length limit, and `_parseForm` sizes a
stack array from it (`char fastBoundary[4 + boundary.length() + 1]`). That runs
inside `handleClient()` for any POST, PUT, PATCH or DELETE with a multipart
body, to any URI, before any route handler is called, so none of our handlers'
Host, Origin or CSRF checks run first. A boundary longer than the loop task's
free stack overflows it: about 8 KB on Vision, about 24 KB on the displays.

Upstream fixed it in 3.3.8 (commit `bb86048`, PR #12486) by refusing a boundary
longer than 70 characters, the RFC 2046 maximum. No 2.x release carries the fix,
and 2.0.17 is the last 2.x release.

These builds compile Arduino WebServer (`common/network/node_http.cpp` and
`setup_portal.cpp`, or the display's `glass_web.cpp` and `provision.cpp`) on
core 2.0.17, and link this copy instead of the core's:

| Product | Builds |
|---|---|
| canary-vision | `canary-vision-default`, `-xiao-c3`, `-xiao-s3`, `-c3-super-mini`, `-debug` (PlatformIO) |
| canary-display | the watch release image (Arduino CLI, core 2.0.17 row); `canary-display-nightstand-s3`, `-touch169`, `-amoled241`, `-nightlight-c3`, and the CI-only `-watch`, `-watch-modes`, `-watch-debug` (PlatformIO) |
| canary-sentinel | `canary-sentinel-lite` (PlatformIO, CI only; not released) |

They stay on core 2 for now because moving each one is its own piece of work: a
bench pass per display panel, and, for Vision, Seeed_Arduino_SSCMA, which has
never been compiled on core 3. Core 3.3.8 and later carry the upstream fix, so a
product that moves drops this copy in the same change.

## How the builds pick it up

- **PlatformIO.** Those envs list `../../third_party` (this directory's parent)
  in `lib_extra_dirs` (`firmware/envs/platformio/canary-vision.ini`,
  `canary-display.ini`'s `canary_display_base` and `canary_display_c3_base`,
  `canary-sentinel.ini`'s `canary-sentinel-lite`). PlatformIO searches
  `lib_extra_dirs` before the framework's own libraries, so `<WebServer.h>`
  resolves here, and the build's dependency graph names
  `WebServer @ 2.0.17+securacv.1` where the core's copy would show
  `WebServer @ 2.0.0`. `firmware/scripts/check_webserver_link.py pio` reads
  that log: `firmware.yml` fails a build whose log names the core's copy, and
  the release workflows withhold its image. `[env:canary-display-dash]`, which moves to core 3, states its own
  `lib_extra_dirs` without this storage, for itself and every env extending it.
- **Arduino CLI.** Every core 2.0.17 row that compiles the display sketch passes
  `--library` with this directory (`firmware-release.yml`,
  `flasher-release.yml`, `firmware.yml`). A library given with `--library`
  wins over a library in every other location, the core's included. Passing
  it is not proof it was used, so those rows compile with `--format json`
  and `check_webserver_link.py cli --expect <this directory>` holds the one
  WebServer the result lists to this directory: `firmware.yml` fails a build
  that linked any other, and the release workflows withhold its image.
- **Not covered:** a person who builds the display sketch on core 2.0.17
  themselves (the Arduino IDE, or the sketch.yaml `watch` and `dash` profiles)
  gets the core's own WebServer unless they pass this directory the same way
  (`firmware/projects/canary-display/arduino/canary_display/README.md` says
  how). Images built from a tree that carries this copy (fw-v2.4.21 onward),
  and the CI builds, use it. The canary-vision and core-2 display images
  published through fw-v2.4.20 link the core's own, and a flasher rebuild of a
  tag cut before this copy existed still does: `flasher-release.yml` rebuilds
  such a tag as it was released, and warns that it does.

## The change

```diff
@@ -17,6 +17,7 @@
   Modified 8 May 2015 by Hristo Gochkov (proper post and file upload handling)
+  Modified 16 Nov 2026 for SecuraCV (GHSA-8cmm-3887-r32j: multipart boundary capped at 70, fixed-size boundary buffer)
 */
@@ -164,6 +165,10 @@ bool WebServer::_parseRequest(WiFiClient& client) {
           boundaryStr.replace("\"","");
+          if (boundaryStr.length() > 70){  // RFC 2046: max boundary length is 70 (SecuraCV backport of upstream bb86048, GHSA-8cmm-3887-r32j)
+            log_e("Invalid boundary length: %u", boundaryStr.length());
+            return false;
+          }
           isForm = true;
@@ -347,6 +352,7 @@ int WebServer::_uploadReadByte(WiFiClient& client) {
 bool WebServer::_parseForm(WiFiClient& client, String boundary, uint32_t len){
   (void) len;
+  if (boundary.length() > 70) return false;  // SecuraCV: the cap _parseRequest applies, held here too, so fastBoundary below can never overflow
@@ -433,7 +439,7 @@ bool WebServer::_parseForm(WiFiClient& client, String boundary, uint32_t len){
             int fastBoundaryLen = 4 /* \r\n-- */ + boundary.length() + 1 /* \0 */;
-            char fastBoundary[ fastBoundaryLen ];
+            char fastBoundary[ 4 /* \r\n-- */ + 70 /* RFC 2046 max */ + 1 /* \0 */ ];  // SecuraCV: fixed size, never sized by the client (GHSA-8cmm-3887-r32j)
```

and, in `library.properties`, `version=2.0.0` became `version=2.0.17+securacv.1`,
so a build log, an SBOM and a reader can tell this copy from the core's.

- The `_parseRequest` hunk is upstream's `bb86048`, except that its log line
  prints the boundary's length rather than the client's string.
- The `_parseForm` guard and the fixed-size buffer are ours. Upstream 3.3.8
  keeps the variable-length array and relies on the cap alone; here the buffer
  the client's bytes are compared against has a size fixed at compile time,
  whatever reaches it.
- The header line records the change, as LGPL-2.1 section 2(b) asks of a
  modified file.

`src/WebServer.cpp` has one more change, not an advisory's: upstream writes
each chunked-transfer size line with `sprintf(chunkSize, "%x%s", <size_t>, ...)`
into an 11-byte buffer. On the ESP32 `size_t` is 32 bits, so 8 hex digits, the
`\r\n` and the NUL fill it exactly and nothing overflows; on a 64-bit host the
same line could write past it, which is why code scanning flags it. Both calls
are now `snprintf(chunkSize, 11, "%x%s", (unsigned) <size>, footer)`: the
bytes on the ESP32 are unchanged, and the write is bounded on any word size.

Nothing else differs from upstream; the test below holds every other file to
its upstream hash.

## What it does not fix

Only GHSA-8cmm-3887-r32j is backported. 2.0.17's WebServer still reads each
request line and header with an unbounded `readStringUntil` (heap, not stack),
and no other upstream WebServer change is carried here. The firmware SBOM
(`sbom/sbom-firmware.cdx.json`) lists this copy as
`vendored:WebServer-2.0.17-securacv`, with a pedigree naming upstream's library
and this advisory, under the products whose builds link it; it still lists
arduino-esp32 2.0.17 under the same products, and
`scripts/check_firmware_advisories.py` still compares every advisory of that
core against it. ESP-IDF 4.4.7, under these images, receives no further fixes;
nothing here changes that.

## How it is checked

- `scripts/tests/test_vendored_webserver_core2.py` (lint.yml): every file here
  against the upstream sha256 below; the three changed files differ from upstream
  by exactly the hunks above; every core-2 PlatformIO env that compiles a
  WebServer user lists this storage, and no core-3 env does; and every Arduino
  CLI core 2.0.17 row that compiles the display sketch passes `--library` with
  this directory.
- `firmware/tests_host/test_webserver_boundary.cpp` (`make -C
  firmware/tests_host`): compiles this `WebServer.cpp` and `Parsing.cpp` against
  host fakes, under AddressSanitizer, and feeds requests through a fake socket. A
  boundary of 71 characters and up is refused before one byte of the body is
  read, on a URI no handler claims and on an upload route; 70 still carries an
  upload byte for byte; an ordinary form field still parses. The same test fails
  against upstream's `Parsing.cpp`, and against a buffer one byte short.
- On devices: CI compiles it into the builds listed above. No device has run it
  on a bench.

## Provenance

Fetched from `https://raw.githubusercontent.com/espressif/arduino-esp32/2.0.17/<path>`
on 2026-11-16. `LICENSE.md` is the repository root's.

| Here | Upstream path at tag 2.0.17 | Upstream sha256 | State |
|---|---|---|---|
| `LICENSE.md` | `LICENSE.md` | `994ce1902a23c74519b5d118c6bbb657edabce91dd6b7cc825d86bb8f5c9780f` | verbatim |
| `keywords.txt` | `libraries/WebServer/keywords.txt` | `ff1806602b36d8417a3c8a1e537c23c62fc76a50421bb5a4956272e6f458f8ff` | verbatim |
| `library.properties` | `libraries/WebServer/library.properties` | `6aafba053d18b8e2a7f5b61db844e75d9e424be0653e1df575debc2f35572c29` | patched |
| `src/HTTP_Method.h` | `libraries/WebServer/src/HTTP_Method.h` | `c68c547c8ee6fc28b14edec748e01472b1f6ad3e6e1edade478181deb2fcbe17` | verbatim |
| `src/Parsing.cpp` | `libraries/WebServer/src/Parsing.cpp` | `122de5397729899ac8600d545f7ed4b8a02298351a4f1b0fa5c7fa73f87a14d0` | patched |
| `src/Uri.h` | `libraries/WebServer/src/Uri.h` | `bde0f8c102c7f1fa10bf3f7aa6240d448dc63a32cb9543fdb03d2c71c177ab49` | verbatim |
| `src/WebServer.cpp` | `libraries/WebServer/src/WebServer.cpp` | `7fe7cc252b2f3cf83b330f52de687fe9fc22cdc71028089eb6b0e177a9ce0aae` | patched |
| `src/WebServer.h` | `libraries/WebServer/src/WebServer.h` | `7770ef659d332dee92947cf7d98097874c39583a9113fd66735ca329459c8e8b` | verbatim |
| `src/detail/RequestHandler.h` | `libraries/WebServer/src/detail/RequestHandler.h` | `c923e1b14fba9b08beb53b2a5b6160d0e2159260cd88454911a0c75ceedcd39f` | verbatim |
| `src/detail/RequestHandlersImpl.h` | `libraries/WebServer/src/detail/RequestHandlersImpl.h` | `1900613216526a0456a969ac2e6679a3fbaacd7b14660b27d5c3ddd481e7dac9` | verbatim |
| `src/detail/mimetable.cpp` | `libraries/WebServer/src/detail/mimetable.cpp` | `b01337d846549531bdec1b9d5d498b84b920dc646b0c8147d1832d64c183d59b` | verbatim |
| `src/detail/mimetable.h` | `libraries/WebServer/src/detail/mimetable.h` | `0746dfa7534c40820f3e597705a7a31178cc2105a27b11fa5904ea319021c186` | verbatim |
| `src/uri/UriBraces.h` | `libraries/WebServer/src/uri/UriBraces.h` | `c9a40cb950154e48c5b4236ed56c31b0930bbd61c873cfcefea0dd640175f8fd` | verbatim |
| `src/uri/UriGlob.h` | `libraries/WebServer/src/uri/UriGlob.h` | `9cb67fe773f7997f0b95d6e1db7c6abf4170c48a75c51f0acea38b5df50e1cb3` | verbatim |
| `src/uri/UriRegex.h` | `libraries/WebServer/src/uri/UriRegex.h` | `7bae628421c0dc498763d4de9422264b6c99e053989313a78e62fec0ec547faf` | verbatim |

The library's `examples/` directory is not copied: nothing here builds it.

## License

GNU Lesser General Public License, version 2.1 or (at your option) any later
version, as each source file's header says. `LICENSE.md` is that license's text
as the arduino-esp32 repository carries it.
