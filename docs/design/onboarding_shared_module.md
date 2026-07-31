# One onboarding, every board — extracting the WAP's network stack into `firmware/common/network/`

**Status:** design / inventory (no code moved yet)
**Owner intent:** the Canary WAP got the most onboarding work — SoftAP with a
captive portal, the `canary.local` name, the join wizard, the careful
after-join choreography. Every board should get that experience instead of
re-implementing it (or shipping without it).

## Where we actually are: three portals, two orphans

| Project | Onboarding today |
|---|---|
| `firmware/projects/canary-wap` | The reference: SoftAP + captive DNS + per-OS probes + companion wizard + `canary.local` catch-all. `esp_http_server`. |
| `firmware/canary` | An **older fork of the same design** (`securacv_setup`, `securacv_network`, ~3,800 lines). Hardcodes `MDNS_HOSTNAME "canary"` for every unit — the collision the WAP later solved — and its captive DNS still answers every QTYPE with an A record, the exact Android-stall bug the WAP fixed (LESSONS_LEARNED "Captive DNS redirector must answer A queries only"). |
| `firmware/projects/canary-display` | A **hand-port** of the WAP lessons (`src/net/provision.cpp`, 654 lines) on a *different* webserver (Arduino `WebServer`). Good behavior, zero shared code — every WAP fix must be re-ported by hand. |
| `firmware/projects/canary-sense` | **No portal.** NVS-seeded credentials from the flasher, or compiled defaults. A Sense flashed without baked Wi-Fi has no recovery path but re-flashing. |
| `firmware/projects/canary-vision` | Same as Sense. |

Three divergent implementations of the same feature, on two webserver stacks,
and two products with no onboarding at all. Every past incident in
`firmware/LESSONS_LEARNED.md` §Networking was fixed in *one* of them and only
sometimes copied.

## What moves, in dependency order

### Phase 1 — the pure logic (already host-tested, zero coupling; move verbatim)

| From `canary-wap/arduino/canary_wap/` | Lines | What it is |
|---|---|---|
| `captive_dns.h` | 88 | DNS-hijack packet builder; A→AP-IP, everything else NODATA |
| `captive_probe.h` | 126 | Apple/Android/Windows probe classifier + exact responses |
| `provisioning_logic.h` | 115 | AP-drop grace, deferred reboot, scan-cache, BLE hold-off policy |
| `catchall_logic.h` | 77 | `canary.local` claim/stagger/tie-break decision table |
| `wifi_provisioning_auth.h` | 28 | pair-token vs admin auth vocabulary |

Destination `firmware/common/network/`, next to the already-shared
`wifi_join_policy.h`. Their host tests
(`tests_host/test_captive_dns.cpp` etc.) move to `firmware/tests_host/`.
The WAP includes flip to the common path; behavior change: none.

### Phase 2 — parameterize the glue

- `mdns_identity.h` (new, ~90 lines): `sanitize_mdns_label` +
  `generate_mdns_hostname` + `generate_ap_ssid`, taking the identity
  fingerprint as a parameter (the WAP versions read `g_device` globals).
  This is what kills `firmware/canary`'s hardcoded hostname.
- `wifi_store.h` (new, ~150): the NVS credential keys (`wifi_ssid`,
  `wifi_pass`, `wifi_en`, `wifi_ap_only`, `setup_ok`, `dev_name`) behind one
  interface — today the WAP, `firmware/canary`, and the display each spell
  these slightly differently. Must keep `WiFi.persistent(false)` (the
  Arduino-core double-write corruption note in the WAP).
- `portal_page.h` (new, ~90): the static captive landing HTML with hostname /
  product name as template slots.
- Adopt `wifi_join_policy.h` in the WAP itself (it predates the shared header
  and still has inline failure strings) — the cheapest parity win on the list.

### Phase 3 — the webserver seam (the real work)

The WAP is on `esp_http_server`, the display on Arduino `WebServer`. The
shared `portal_api` (scan / connect / status / ap-only, ~600 lines) needs a
minimal request/response trait both can implement — worth it, because this is
where the security-sensitive handlers live (pair-token gating, SSID/pass
validation) and where drift hurts most.

### Phase 4 — adopt

1. **canary-sense / canary-vision** first: they have *nothing*, so the shared
   module is pure gain and the integration is greenfield (their
   `wifi_mgr.cpp` already consumes `wifi_join_policy.h`).
2. **firmware/canary**: replace `securacv_setup`/`securacv_network`'s portal
   guts with the common module (deletes the stale-DNS and hostname-collision
   bugs by construction).
3. **canary-display** last: it works today; migrate when the Phase-3 seam is
   proven on the others.

### Explicitly NOT moving

Mesh/Opera (`mesh_*`, hub election, channel hop — though `mesh_channel_policy`
needs a "current AP channel" query exposed by the shared module), the usbdrive
build env, QR provisioning (optional camera add-on behind the same
`/api/wifi/*` surface), pair-token/session machinery, witness/vault/chirp.

## Invariants the shared module must carry (each one was paid for)

- Captive DNS answers **A queries only**, NODATA for AAAA/HTTPS — and runs
  for the whole life of the AP, draining multiple packets per loop pass.
- Probes stay on **plain HTTP port 80**, never behind the HTTPS redirect.
- Apple probes get instruction HTML (not the Success token); Android gets a
  real 204; Windows gets the exact NCSI bodies.
- AP SSID / hostname derive from the **pubkey fingerprint, never the MAC**.
- After a successful join: AP stays up through the grace window, captive DNS
  restarts, reboot is deferred and extendable — an instant reboot makes every
  successful provision look failed on the phone.
- Wi-Fi password fields are **masked text** (`.pw-masked` +
  `autocomplete="off"`), never `type="password"`; Show/Hide flips the class.
  The OS must never offer to *invent* a key for an existing network. (Now
  also enforced outside firmware by
  `canary-local/tests/desktop_parity.test.js`.)
- Scan is async + cache-first; a live sweep knocks the provisioning phone off
  the AP.
- BLE discovery holds off until the AP is down.

## Flasher tie-in

Both flashers already seed credentials over USB (NVS), which stays the fast
path. The portal is the *recovery and no-computer* path — once Sense and
Vision gain it, the flashers' "leave Wi-Fi empty and the board broadcasts its
own setup network" copy becomes true for every product, not just some. The
`families[].pick` copy in `devices/flash.json` should say so per family.
