# Vendored, self-hosted libraries for the browser flasher

The flasher (`../flash.js`) runs entirely in the browser and must keep the
Lab's promise: **works offline, nothing phones anywhere.** So its two
third-party dependencies are vendored here and served from this site — never
pulled from a CDN at runtime. The only network call the flasher makes is
fetching the signed firmware image the user chooses.

| Directory | Package | Version | License | Purpose |
|---|---|---|---|---|
| `esptool-js/` | [`esptool-js`](https://github.com/espressif/esptool-js) | 0.5.4 | Apache-2.0 | Espressif's official in-browser flasher: Web Serial transport, chip detection, read/write flash. `bundle.js` is the upstream single-file ESM build, unmodified. |
| `md5/` | [`blueimp-md5`](https://github.com/blueimp/JavaScript-MD5) | 2.19.0 | MIT | MD5 for post-write verification. `md5.js` is the upstream source converted from UMD to a native ES module (algorithm unmodified) with an added `md5Raw` export that hashes raw bytes without UTF-8 re-encoding — required so the hash matches the device's own flash MD5. |

## Why MD5 at all

`crypto.subtle` (Web Crypto) covers SHA-256 — which the flasher uses to verify
a downloaded image against the release manifest **before** writing — but not
MD5. esptool's on-device read-back verification (`flashMd5sum`) is MD5, so a
small MD5 is needed to confirm, **after** writing, that the flash chip holds
exactly the bytes we sent. Two independent checks, one before and one after.

## Updating

```sh
# esptool-js
npm pack esptool-js@<version>
tar xzf esptool-js-<version>.tgz
cp package/bundle.js  canary-local/assets/vendor/esptool-js/bundle.js
cp package/LICENSE    canary-local/assets/vendor/esptool-js/LICENSE

# blueimp-md5 — re-convert UMD → ESM, re-add the md5Raw export, keep the
# license header. See the header comment in md5/md5.js for the exact edit.
```

After updating, re-run `node --test canary-local/tests/flash.test.js` and the
`bundle.js` should still import cleanly:
`node --input-type=module -e 'import {ESPLoader,Transport} from "./canary-local/assets/vendor/esptool-js/bundle.js"'`.

Provenance of the currently-vendored files is recorded in
`esptool-js/PROVENANCE.txt`.
