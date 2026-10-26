# SecuraCV Canary — Security & Privacy Model

> This document is included in every evidence export. It explains what
> the device does, what it can prove, and what its limitations are.
> It is written for a technically literate non-engineer: a journalist,
> a lawyer, a policy maker, or anyone evaluating evidence from this device.
>
> For a shorter companion focused on exports — why there is no "download
> the clip", why times are coarse, and how to handle a failed verification —
> see [Why SecuraCV exports work this way](../why_secure.md).

---

## What This Device Does

SecuraCV Canary is a witness device. It creates tamper-evident records of
events — nearby wireless devices, GPS location, environmental changes —
and chains them together cryptographically so that any alteration or
deletion is detectable.

Each record is:
1. **Hashed** into a chain (like blockchain, but on a single device)
2. **Signed** with the device's unique cryptographic key
3. **Sequenced** so gaps or missing records are visible

If any record is altered, removed, or inserted after the fact, the chain
visibly breaks and anyone checking the evidence can see the tampering.

---

## What It Can Prove

- That a specific device recorded specific events
- That events were recorded in a specific order
- That the chain has not been tampered with since creation
- That the GPS receiver reported a specific location at a specific time

## What It Cannot Prove

- That the device was physically present at the GPS-reported location
  (GPS signals can be spoofed by a sophisticated adversary)
- That the events described are "true" in an absolute sense — only that
  they were recorded by this specific device at this specific time
- The identity of the person operating the device
- That no events occurred between recorded intervals

---

## What It Collects

The device records only:

- **Timestamps** — rounded to ten-minute buckets (never precise)
- **GPS coordinates** — if available, with configurable precision coarsening
- **Count of nearby WiFi devices** — not their identities
- **Device health data** — memory, storage, battery status
- **User-triggered events** — camera preview snapshots (on demand only)
- **Environmental changes** — motion state transitions (stationary/moving)
- **Coarse optical signals** (vision Canary) — occupancy as a bucket
  (none/one/two/several), a posture class (upright/ambiguous/horizontal), and a
  proximity band (far/mid/near), derived on-device from person bounding-box
  geometry. Ordinals only — never coordinates, gait, or identity

---

## What It Does NOT Collect

The device does not and cannot collect:

- WiFi network names (SSIDs) of nearby networks
- Device identifiers — MAC addresses are hashed before any storage and
  the originals are immediately discarded
- Audio or continuous video recording
- Skeletons, keypoints, gait, or body measurements — the vision Canary emits
  coarse ordinals derived from bounding boxes, never a biometric
- Exact per-person counts or occupancy histories — occupancy is a coarse
  bucket, not a running tally
- Browsing history or app data from nearby devices
- Any data from your phone when connected to the dashboard
- Usage analytics, telemetry, or crash reports
- Any information about what you do with the evidence

---

## How Privacy Is Protected

### No Phone-Home

No Canary contacts ERRERlabs, a cloud service, an analytics provider or a
crash reporter — the code to do so does not exist. A Canary WAP used as its
own WiFi Access Point makes no outbound connection at all: it is a server,
and it never speaks first. The products that join your home network open
exactly the paths disclosed in the next section and nothing else: the MQTT
broker you point them at (plain by default; CA-verified or SHA-256-pinned
TLS once you provision it, refusing to connect rather than downgrading), a
daily jittered check of a small signed update manifest on the products with
pull-OTA, SNTP on every product without GPS (the display line, and since the
shared Wi-Fi supervisor also sense, vision and the sentinel), and on the
display line the opt-in standalone forecast.
The third-party paths carry no identifier; the broker link carries your
device's own id and name to the broker you run, in every topic. The
forecast and the update fetch's URL policy are pinned by host tests
(`firmware/tests_host/test_wx_core.cpp`,
`firmware/common/ota/test_ota_logic.cpp`); SNTP's two hosts are literals
in the display firmware's `tz_auto.cpp` and, for the headless boards,
`firmware/common/time/clock_sync.h`'s `NTP_PRIMARY` / `NTP_SECONDARY`
(pinned by `firmware/tests_host/test_clock_sync.cpp`; the supervisor names
them at its call site so `regression_check.sh`'s phone-home scan matches
them against this list).

This means:
- A WAP on its own access point is invisible to anyone watching your
  network; a networked Canary is visible exactly as its disclosed paths
  are, and no more
- No ERRERlabs server exists to be compelled: the only servers a Canary
  talks to are the broker and hub you run and the update host, which sees
  an anonymous manifest fetch
- Every product keeps witnessing without internet; only the disclosed
  paths go quiet

#### The networked products' disclosed outbound paths

Every product that joins a home network — the displays (Dash / Nightstand /
Watch Station) to render the fleet, the sense and vision Canaries and the
`firmware/canary` flagship to publish, a WAP once you join it to your WiFi —
carries only the outbound paths listed here. The third-party paths carry no
identifier (the broker link carries your device's own id and name, to the
broker you run) and none is required for the device to witness. A new
outbound path is a change to this list and to `THREAT_MODEL.md` Principle 2,
with a host test pinning its request shape the way
`firmware/tests_host/test_wx_core.cpp` pins the forecast.

1. **The MQTT broker you chose** — sense, vision, the flagship and the
   WAP's bridge publish to it; the displays subscribe. The socket is plain
   by default and TLS once you provision it — verified against a CA you
   supply on every product, pinned to the broker certificate's SHA-256
   fingerprint on the display line (not the plain-only nightstand-c6),
   sense, vision and the flagship; an incomplete setup refuses to connect
   rather than falling back ([`docs/FIRMWARE_VARIANT_AUDIT.md`](../FIRMWARE_VARIANT_AUDIT.md)).
   Compile-tested by CI, decision host-tested, not bench-tested against a
   TLS broker.
2. **Signed update checks** — on every product with pull-OTA (the
   flagship, WAP, vision, sense and the display line): a daily, jittered
   HTTPS GET of a small signed JSON manifest from the release host
   (`docs/firmware_ota.md`; the desktop Flasher can also ask the glass to
   run one over the LAN). No identifiers ride on it, and nothing is
   installed until the manifest verifies against the pinned Ed25519
   release key and you press Install — or turn on the per-device Auto
   Update switch, which is off by default. No setting turns the check on
   or off from the network, and it carries no location. Disclosed on the
   glass's own network page (`docs/hardware/display_settings.md`).

Every product without GPS adds the first of these; the display line adds
the other two:

3. **Time (SNTP)** — always on when networked: UTC from two public time
   sources (`pool.ntp.org`, `time.nist.gov`), with the hub as a third
   source once one is provisioned. On the display line this is what keeps
   a bedside clock honest. Sense, vision and the sentinel sync the same
   way through the shared Wi-Fi supervisor
   (`firmware/common/network/wifi_supervisor.cpp`), so their `/api/status`
   can say whether the clock is believable (`clock: ntp` or `none`); their
   records still stamp `ts_ms` from uptime. (The flagship and the WAP take
   their time from GPS and make no SNTP request.)
4. **Timezone lookup** — compile-time opt-in only (`CD_TZ_WEB_LOOKUP` in
   `secrets.h`); off in every shipped image. Without it the zone comes
   from configuration or the app.
5. **Standalone weather** — the one *opt-in* path: runtime opt-in, off
   by default, and gated three ways (`firmware/.../net/wx_direct.h`): the
   owner must switch it on **on the glass itself**, a coarse location must
   be stored, and **no hub may ever have been configured**. A home with a
   hub keeps the hub as its single egress point — the fetcher never
   becomes a fallback when that hub is down. The switch is a hand on the
   glass, not a network call: the display's LAN write API (`POST /api/set`)
   refuses `wx_direct` and `wx_loc` for every caller, token or not, with
   `403 {"ok":false,"err":"on_glass_only"}` (one host-tested table,
   `net/settings_policy.h`), so a host on the home WiFi cannot flip the
   device's one opt-in outbound path or plant a location for it. `GET
   /api/settings` reports the opt-in's on/off to any caller, and whether a
   location is stored only to callers that are not cross-site. The query
   is an anonymous HTTPS forecast request (Open-Meteo, pinned root CA) over
   a 0.1° grid point (~11 km); the exact request shape is pinned by a host
   test (`firmware/tests_host/test_wx_core.cpp`) so it cannot quietly grow an
   identifier. The device never serves or republishes the stored grid
   point. The location is entered **on the glass itself**: the two 7"
   flavors that carry the standalone forecast (`dash7`, `nightstand7`)
   have a Location page under settings → weather — hemisphere, degrees
   and tenths wheels per axis, committed by one explicit *Use This
   Location* — whose helpers can only produce a point on the 0.1° grid
   inside the range the loader accepts (host-tested,
   `tests_host/test_display_settings.cpp`). There is no place-name lookup:
   entering a location makes no network request at all. **The stored cell
   is displayed on the glass** — on the Weather page's Location row and as
   the page's live caption — and that is a deliberate in-room disclosure:
   anyone who can stand at the glass and open its settings can read the
   ~11 km cell, exactly as they could read the forecast it produces. The
   disclosure stops at the glass: the LAN page and `GET /api/settings`
   still never carry the grid point, only whether one is stored. Closing
   the network path closed the phone app's `wx_loc` post for good; a grid
   point stored by an earlier build keeps working, *Forget Location* and a
   settings reset both clear it. Compile-tested by CI on the two 7" builds,
   wheel helpers host-tested, not yet bench-tested.

### No Tracking Identifiers

- The WiFi network name ("Canary-XXXX") reveals no manufacturer identity,
  serial number, or information linking back to ERRERlabs or the owner.
  The radio's hardware address is a separate disclosure: nothing in the
  firmware sets a derived or random MAC, so the access point's BSSID and
  the BLE advertising address are the chip's factory addresses and carry
  Espressif's OUI, as every ESP32's do — they say which vendor made the
  radio, not which device or owner this is
- BLE (NimBLE) is compiled into the shipped Canary WAP build
  (`BUILD_PROFILE_FULL`) for owner pairing and provisioning, the GATT
  status service, signed BLE firmware updates, the Scout scanner that
  attributes rooms to the owner's own paired beacons by hashed MAC, and
  two passive scanners: BLE presence, a listen-only feed into the same
  presence pipeline as the WiFi one that keeps no MAC, OUI or name from
  the scanner, and Nearby discovery, which recognizes other Canaries by
  the SecuraCV service UUID and only counts everything else. The MINIMAL
  profile compiles it out; DEV keeps the pairing channel (with the BLE
  OTA and the presence listener that ride it) and the status service.
  The `firmware/canary` flagship's `release` and `release_ha` images
  build it out; its `full` env compiles the Scout scanner and the GATT
  status service (`securacv_ble_scan`, `securacv_ble_status`), the only
  BLE code in that tree. Its advertisements carry the device's own name
  and the SecuraCV service UUID; the fleet beacon and Chirp add a
  manufacturer-data field under the Bluetooth SIG's reserved test id
  `0xFFFF` — type, flags, battery, health, chain height and the two
  fingerprint bytes already in the device's name (`fleet_beacon.h`) —
  never a serial. Nothing classes or tracks other people's devices. The
  S3 / C3 the BLE-bearing builds run on have no Classic (BR/EDR) radio;
  the classic-ESP32 boards have one and no build compiles a stack for it
- Service discovery (mDNS `_securacv._tcp`) is advertised only on the
  network the device is on — its own access point, or the home network you
  joined it to — never beyond it
- MAC addresses from nearby devices are cryptographically hashed — the
  device counts nearby devices without knowing or storing who they are

### Keys Never Leave the Device

The device's cryptographic identity (an Ed25519 private key) is:
- Generated on the device's own hardware random number generator
- Stored only on the device's internal flash memory
- Never transmitted over any interface (WiFi, USB, or otherwise)
- Not backed up, escrowed, or recoverable by anyone — including ERRERlabs

There is no "export private key" function. There is no backdoor, master
key, or recovery mechanism. If the device is lost or destroyed, the
evidence it already exported remains verifiable, but new evidence
requires a new device with a new identity.

**Why no backup?** Because a key that can be exported can be compelled.
A court order, a warrant, or physical coercion — if the key CAN leave
the device, an adversary with sufficient power WILL extract it. So there
is no software path that will hand it over: no API, no export, no
diagnostic, no debug interface, and nothing to compel us to hand over
because we never had it.

**The limit of that promise.** It is a statement about software. Someone
who physically takes the device and reads the flash chip directly is a
different adversary, and on a default-configured Canary they can recover
the key. That is a deliberate trade — the alternative permanently bricks
devices whose owners lose a key — and it is spelled out, with what it does
and doesn't let an attacker do, under
[Physical extraction and the flash-encryption default](#physical-extraction-and-the-flash-encryption-default).

---

## Who Can Access Your Data

Whoever can reach the device's network port. That boundary is the thing to
understand, product by product, because on most of them nothing stands
behind it. The `firmware/canary` flagship adds one check of its own: once it
is set up, a page loaded over your home network arrives without the API
token unless you tap the BOOT button first, and a request over the
flagship's own access point is told apart from one over your home network.

**On the device's own access point** — the Canary WAP, the `firmware/canary`
flagship, the headless products' setup portals — the boundary is the AP
password, unique per device. Anyone holding the password is inside. On the
flagship that includes the API token: a request that provably arrived over
its own access point gets the page with the token in it. On the WAP and the
flagship the password is an HMAC of the device's private key, never derived
from anything the device publishes; the headless setup portals draw theirs
at random. Until sweep F350 the flagship's was computed from its public-key
fingerprint, which the device publishes (its setup network's name spells
two of the fingerprint's bytes, and every signed event it sends over MQTT
carries all of it), so anyone who had seen the fingerprint could compute
the password. The flagship keeps that access point up while it has no
home network and again whenever the home link drops, and takes it down
once the home link has held for a grace period.

**On your home network, the boundary is your network.** The flagship's
release images serve its dashboard and API as plain HTTP on port 80. Once
first-boot setup is done, a home-network load of `GET /` or `/setup` gets
the page without its API token; the dashboard says how to unlock it (a BOOT
tap, the flagship's own access point, or the token from your recovery kit).
The token rides the page only while the first-boot wizard is running, for a
request that already carries the bearer token, for a request over the
flagship's own access point, or for the one page load that spends a short
BOOT tap: the tap opens a 30-second gate that exactly one consumer spends —
that page load or one provisioning-receipt fetch, whichever asks first.

A request whose `Host` header cannot name this device — an IP address, the
`.local` name, a single label and a private-use suffix all can; a public
name re-pointed at your network, the DNS-rebinding trick, cannot — gets the
page with no token whatever else applies and never spends your BOOT tap, and
every API route, the provisioning receipt included, refuses a call under that
name with `403 {"error":"host"}` before it looks at the token or the tap. Only
a request over the flagship's own access point is exempt,
because there the name is the device's by construction. A household that
reaches the flagship by a public split-horizon name gets the same token-less
page and has to use the IP, the `.local` name or a private-suffix alias
instead.

The gate is not encryption. The token is still a bearer secret on the wire,
so on a release image a host that can watch your network reads it out of
someone else's session — an unlocked page load, or any API call the page or
the app makes — as it can read the broker password below, and whoever asks
inside those 30 seconds, with a page load or a receipt fetch under a name
that means the device, can spend your tap before you do. The decisions
(`firmware/common/network/provisioning_gate.h`'s `page_token_policy` and
`receipt_decide`, and `host_guard.h` beside them) are host-tested, the handler glue is compiled by
CI, and none of it is bench-tested yet.

The flagship's bearer lockout is the WAP's table,
`firmware/common/network/auth_backoff.h`, keyed by the request's socket peer
(`request_peer_ipv4()`, never a header) (sweep F340). Every bearer it
compares is accounted: the API routes through `auth_check()`, and the
provisioning receipt and the page-token decision through the grant
`auth_bearer_grant()`. A malformed or wrong bearer is a strike against its
address. While that address's backoff runs, its API calls are refused `429`
with `Retry-After`, and its bearer on the receipt is not compared and reads
as none, so the receipt goes on to the BOOT tap: served if one is open,
else its `403`, which the page reads as "tap BOOT". A request with no
`Authorization` header is no strike, so a dashboard tab loaded without the
token, which polls with no header, arms nothing. A BOOT tap that hands an
address the token, on the receipt or on the page load it unlocks, ends that
address's backoff as a right bearer would (sweep F341). The dashboard
drops a token the device refuses and shows its unlock banner again, so a
mistyped paste is dropped at its first refusal instead of being sent on
every poll. A host's wrong
bearers hold only that host's address (F343). A browser that mistyped the
token waits at most about 60 s, or taps BOOT, which clears its address
(F341). A host that strikes from 9 or more addresses within 60 s holds every
untracked address while the shared overflow slot runs, and a tap does not
clear that; then wait, change Wi-Fi from the iPhone's bonded Bluetooth lane,
or hold BOOT 10 s. The flagship has no Bluetooth lane that changes its
Wi-Fi, so there the fallbacks are to wait or the 10 s hold. Clients behind
one address share a backoff. It does not close a deliberate
denial from the LAN: the device-wide request limit (120 requests a minute
across the rate-limited routes) is still one per device, so two open Status
tabs reach it by themselves (F351), and ARP spoofing and httpd's connection
purge stay open. Host-tested on the real `securacv_auth.cpp`; not
bench-tested.

The WAP's sensing dashboard draws the line elsewhere: its landing page
mints a one-tap pairing link for whoever loads it, and the tap becomes a
24-hour session cookie that signs that browser into the dashboard. That
session is proof of being on the LAN and nothing more: the WAP's bearer
token itself (with the SoftAP password and the TLS pin) is handed out only
in the provisioning receipt — to a caller that already presents the token
as a bearer, to a phone spending the setup door's claim, or for thirty
seconds after a short BOOT tap — and also in the pairing QR an operator
fetches with the token, or on the serial console.

Since sweep F344 the WAP asks the flagship's Host question too, in fewer
places and with no exemption. After setup, the landing page `/`, its one-tap
link (`/?cv_pair=`), `/admin`, `/settings` and `/companion` answer only a
Host that can name this device (`host_guard.h`: an IP address, a `.local`
name, a single label, or a `.lan`, `.internal` or `.home.arpa` name). Any
other Host, or none, gets `403` and one fixed `text/plain` sentence ("This
Canary answers only its own names. Open it at its .local name, its IP
address, or the bare name your router gives it."): no pair token is handed
out, no session is minted, the owner's link stays unspent, and a live cookie
sent under that name is refused too. The provisioning receipt asks the same
question first, in every mode, setup included, and answers
`403 {"error":"host"}` before it reads the bearer, takes the claim or judges
the tap, so such a request is never a strike (F343), never an asker for the
tap (F342), spends nothing and clears nothing (F341). A DNS-rebinding page
under a public name is then handed no session and no receipt (what no Host
question can tell apart is under "What it leaves" below). Nor can it use a
session carried in: any host on the LAN can take a 24-hour session at the
landing, and a page can set that value for its own name with
`document.cookie`, so the cookie validator behind every cookie route
(`session_validate_cookie()`, which `api_auth.h`'s checks and
`CSI_AUTH_OR_RETURN` reach through `cv_session_validate()`) asks the same
Host question before it reads the cookie (F344's third review). Before, it
read no Host, and such a value reached the dashboard, the live camera, the
vault download, every session admin route and `GET /api/pair/token` (whose
pair token then authorized `/api/wifi/connect`) from the rebinding page for
its 24 hours. Two differences from the flagship. There is no SoftAP
exemption, because the WAP's captive DNS sends every name to the device; its
wizard runs at `canary.local` or `192.168.4.1`, which both pass, and the
setup branch and the wizard token's re-issue now match exactly those two
names, in any case and with any port, rather than any name that contains one
of them. And the question is asked by the places that mint a session or a
pair token for a caller holding no credential (`check_route_security.py`'s
`MINT_SITES` lists every place that mints, each with the gate it must pass
first), the pages that start a sign-in and the cookie, not by every API
route as the flagship asks it: outside the receipt a bearer asks no Host, so
a rebinding page that holds the bearer token itself is F57's open question,
and a pair token's value carried off the LAN is read with no Host on
`/api/wifi/connect`, `/api/wifi/ap-only` and `/api/wifi/qr-scan` (start,
status and stop; a started scan acts only on a code held to the camera) for
its ten minutes. F334, still open, asks whether Wi-Fi changes after setup
take a bearer, so that the pair token serves only the first-boot wizard; its
ledger entry names connect and ap-only, not the scan. And a standalone unit
with a camera keeps scanning for a Wi-Fi QR code after setup
(`qr_auto_scan_tick()` runs whenever no Wi-Fi is saved), so a carried token
reads that scan's status (the network name the code held, the error text)
and stops it. Refused after
setup: a router name under another suffix (`.fritz.box`, `.localdomain`,
`.home`), a name with a trailing dot, a public split-horizon name, and a
reverse proxy that forwards a public Host; use the `.local` name, the IP or
the bare router name instead (widening the shared list is F356). What it leaves: the Host question tells a public name from a name
only the local network answers; it cannot tell this device from another
server that answered such a name (`canary.local`, `192.168.4.1`, the
device's own LAN address, any `.local` name, a single label or a private
suffix) on a network the browser used before, such as a hostile hotspot or
mDNS spoofed on a shared Wi-Fi. A page loaded there runs same-origin with
the device when the browser comes home (every WAP claims `canary.local`) or
joins the SoftAP (whose captive DNS answers every name with the device),
whether it was left open or kept in the browser's HTTP cache: a cached
answer is served again, with no request to the device, for as long as the
server that sent it said, even after the browser has quit. In headless
Chromium 141, clearing that name's cookies and stored data did not remove
such a copy: the site's own "Delete data" for `canary.local`, and "Delete
browsing data" over "All time" with "Cookies and other site data" but not
"Cached images and files", each cleared that name's storage and left the
cached page to be served again, while "Delete browsing data" with "Cached
images and files" over "All time", which empties every site's cache,
removed it. Other browsers were not tried. It can take the landing's pair token, a 24-hour session
through `?cv_pair=`, during setup the wizard's pair token (from `/`'s
redirect to the wizard or from the re-issue, at either setup name), and,
around a BOOT tap the owner presses from that browser, the
receipt: one browser is one address to the tap rule (F342), and here the
answer is readable. Only HTTPS with an
identity the browser can check closes it, and neither F15 nor F256 gives a
browser one: F15 would bring HTTPS with a self-signed certificate, whose
warning the owner accepts, and only a client that pins the certificate checks
it (the iPhone app pins the receipt's `tls_cert_fp`; F256 asks whether it may
learn one over mDNS). F15 would still help: once a unit serves HTTPS after
setup, port 80 answers only the captive probes, `/api/fleet` and a redirect,
so a page planted over plain HTTP is cross-origin to the device and takes
none of these (it reads nothing; but `SameSite=Strict` compares sites, so in
a browser that does not count the scheme in the site, as Chromium does, its
requests still carry the session cookie and it can write blind with the
owner's session: a reboot, or any other write a session may make); a page
served over https at that name whose certificate
warning the owner accepted still does, and so does any page during the
first-boot setup or on a unit whose HTTPS start failed, which serve plain
HTTP. And a cross-site page
can still send requests at the IP address (an IP literal passes). It cannot
read what the landing, the exchange, the pages, the wizard token's re-issue
(`/api/wifi/pair-token`) or the receipt serve, since none sends a CORS
header with it (the receipt's `500` and `503` error codes do, through
`http_send_error()`, and carry nothing else). The re-issue answered through
`http_send_json()`, with `Access-Control-Allow-Origin: *`, until F344's
second review: during setup a page on another site, in the owner's own
browser on the SoftAP, read a pair token at `192.168.4.1` (a setup name,
which the exact match passes) and could spend it on `/api/wifi/connect` or
`/api/wifi/ap-only`. It now sends no CORS header, and
`check_route_security.py` refuses a mint site that answers through
`http_send_json()`. Its requests spend no claim, from another address close
an open tap rather than take it (F342), from the owner's own browser can
still use up an open tap, can churn pair tokens, and can stop the boot
Wi-Fi QR scan of a unit with no Wi-Fi saved each time it asks: `POST
/api/wifi/qr-scan` yields that scan before it reads the body or checks the
pair token, and the scan starts again about five seconds after its task
unwinds (older than F344; noted under it in the ledger). A denial of
service, not a way to the token. Three public routes do answer it readably:
`/api/wifi` (the home network's name, the station address, the SoftAP's name
and client count), `/api/wifi/scan` (nearby networks) and `/api/fleet`
(coarse presence) send `Access-Control-Allow-Origin: *` (`http_send_json()`;
`/api/fleet`'s by its documented contract), so any page can read them at the
IP, rebinding or not, and the Host question cannot close that. The other
public routes (`/api/selftest`, `/api/device-info`, `/api/help-qr`,
`/enroll`, `/api/device/enroll`) send no CORS header (`/api/device-info`'s
`500` and `503` error codes do, through `http_send_error()`, and carry
nothing else), so only a rebinding page reads them, and they ask no Host. Both are F357. Host-tested; not run
against a browser or on a device.

Since sweep F342 the tap goes to the one address asking for it: when a
second address asked for the receipt in the ten seconds before the tap or
during it, the tap is closed and an asker gets `409 tap_contended` while an
address other than it asked in the last ten seconds, up to the closed tap's
thirty seconds. So the `409` ends ten seconds after the other device stops;
if that device keeps polling, its own asks end it ten seconds after the
owner's page stopped, the owner's retry hears `403`, and the next tap is
closed with `409` again (the poller case below). The `409` tells the client
to start its request first and keep it asking, then press BOOT; the `403`,
the dashboard and the companion give that order too, their `409` text
included (the click, or the tap on Save, before the press), and both pages
say to press only once the device has answered their first ask
(host-tested, not bench-tested). The setup door's claim is spent only by
a match, so a wrong `?claim=` from a host on the network no longer ends the
phone's pairing (F342). The tap's asker is the request's socket peer
(`request_peer_key()`, the same read the per-address lockout keys on, F343),
never a header. What the tap rule cannot see: a tap no client of the
owner's was already asking for (an accidental tap, a page or `curl` started
after it, a tap pressed per a recipe or an older page that says to press
first, BLE OTA break-glass) still goes to the first asker; hosts behind
one address (a NAT, an ARP spoofer, a page in the owner's own browser) are
one asker; a host that keeps the WAP's one web server busy for more than
ten seconds before the tap can hide the owner's client; and a host that
asks for the receipt at least once every ten seconds (polling included)
makes every tap the owner's client is already asking for refuse, loudly (a
bare re-tap with no client of the owner's asking is the first case, and
goes to that host). Then the recovery kit's token and the USB serial
console (the receipt prints at every boot) are the ways in, and neither
opens anything to anyone else. Holding BOOT 10 s, then setting up again
from the iPhone through the setup door, is the last one: the hold keeps the
token and reopens the first-boot setup door for 30 minutes, and that door
goes to whichever Bluetooth client connects first. A client that connects
before the phone (a host on the network that is also in radio range, say)
can join the Canary to a network it is on, spend the claim minted for its
own link and keep the token, which nothing rotates today. So the hold is
for a host known to be on the network only, with the phone ready, and the
phone ends paired unless something in radio range reached the door first.

Until sweep F289 the session cookie
bought the receipt and the QR too, so any host on the home network could
read the token with no tap; both routes now take the `Authorization:
Bearer` header alone (host-tested, not bench-tested). Since sweep F289's
third road the session also cannot point the MQTT bridge at a new host,
port, TLS mode or CA over `POST /api/mqtt/config` while keeping the stored
broker password — that save takes the bearer or the password typed again
(`403 password_needed`), so the password `GET /api/mqtt/config` keeps
write-only cannot be read through a broker the session names that way
(host-tested). The QR hub-provision path is the road that rule leaves open:
a scanned hub code re-points the broker with the stored password, in a scan
window the session can open (it mints the wizard's pair token over
`GET /api/pair/token`), but only for a code in front of the camera
(`FEATURE_QR_PROVISION`) — accepted as proximity-bound, named in the
ledger. Since F289's follow-up (#1832) a wrong bearer on the receipt counts
toward the same per-address lockout as the admin routes (F343), and the
session cookie itself is `HttpOnly`, `SameSite=Strict`, and `Secure` and
named `__Host-cv_session` whenever it is set over HTTPS (the first-boot
setup runs plain HTTP, where a `Secure` cookie would be dropped, and sets
`cv_session`; two names, F337, so that once a WAP serves HTTPS a browser
holding its cookie can still sign in to a unit it meets over plain HTTP at
canary.local); host-tested, the ESP32 compile is CI's, not
bench-tested. The WAP's lockout is per source address. A host's wrong
bearers hold only that host's address (F343). A browser that mistyped the
token waits at most about 60 s, or taps BOOT, which clears its address
(F341). A host that strikes from 9 or more addresses within 60 s holds every
untracked address while the shared overflow slot runs, and a tap does not
clear that; then wait, change Wi-Fi from the iPhone's bonded Bluetooth lane,
or hold BOOT 10 s. A wrong bearer on the receipt is a strike even when the
setup-door claim or the BOOT tap then serves it, and the claim or the tap
that serves ends that address's backoff, as a right bearer would (sweep
F341). The key is the request's socket peer address, never a
header, and clients behind one address share a backoff. It does not close a
deliberate denial from the LAN: a landing session can still reboot the unit
(`POST /api/reboot`, `POST /api/safe-mode/retry`), and ARP spoofing, httpd's
connection purge and a cross-site `?token=` image that holds the visiting
browser's own address stay open
([`magic_pairing.md`](../design/magic_pairing.md) §3.7). The reboot also
empties the table, kept in RAM, and so resets the guess budget: about 30
guesses a minute between reboots from any number of addresses, and more for
a guesser that reboots the unit every few seconds (about 100 a minute if it
is back on the LAN 10 s later; `THREAT_MODEL.md` scenario 10). The token's
entropy stays the bound. Host-tested; not run on a device. Every bearer
compare on the WAP counts toward that per-address lockout, and none is
compared while its address is held (sweep
F339): the receipt and the pairing QR; the admin routes; the CSI routes, the
MQTT bridge's routes and its broker move; the QR scan's admin fallback; and
the camera's and the vault's `?token=` links, which until F339 never read
the lockout at all (a right `?token=` was served inside a backoff and
cleared it). A held address gets `429` with `Retry-After` on the CSI and MQTT
routes and those links; the QR scan's status and stop keep their `403`. The
route check (`check_route_security.py`) fails any file of the sketch but
`api_auth.h` that calls a raw compare, so the passkey field F346 plans for
`GET /api/bluetooth` can only compare through the same grant. The pair token's
compares stay outside it on purpose: the landing page hands that token to
any caller, and the CSI dashboard and `/companion` ride it or the cookie and
send no bearer. Host-tested; not run on a device. The
displays treat the LAN as their trust boundary by design — no bearer
credential at all, a per-boot CSRF token that keeps cross-site pages away
from writes, and the on-glass-only settings above that no network caller
can flip.

**Encryption in transit is not the default, and no stock unit serves
HTTPS today.** Both trees have an HTTPS server built in that runs once a
certificate is stored, and no stock build stores one (F15): the on-device
certificate generator compiles out on every pinned core (its guard tests
`mbedtls/x509write_crt.h`, a header mbedTLS never ships) and nothing else
stores a certificate. So the Canary WAP serves plain HTTP after setup as
during it, and the flagship's `dev`, `dev_ha`, `usb-onboard` and `full`
builds (`FEATURE_HTTPS=1`) serve plain HTTP and say why in `/api/status`
`tls_mode_reason` ("no stored certificate and this core cannot generate
one"). Once a certificate is stored, the WAP would serve HTTPS on port 443
from a certificate minted on the device after setup has completed, and
plain HTTP during first-boot setup and when the HTTPS server fails to
start, saying so on the serial log; the flagship's HTTPS builds would take
the same shape with a self-signed ECDSA P-256 certificate generated on the
device and kept in its NVS, serving HTTPS on 443 from the first boot after
setup completes and 307-redirecting port 80 there, except the connectivity
probes phones and laptops send, and a build, core or certificate step that
cannot do TLS serves plain HTTP and says why in `tls_mode_reason`. CI
compiles `dev` and `full`; `dev_ha` and `usb-onboard` inherit the flag from
`dev` and no workflow builds them; none of the four has run on hardware.
The flagship's release images (`release`,
`release_ha` and the board envs built on them) stay plain HTTP on port 80
until the size budget shows HTTPS fits and the maintainer turns it on, and
the displays' LAN page and the headless products' setup portals are plain
HTTP on your own network. The hub's API has the same shape — plain HTTP,
TLS through the kernel's `api-tls` build feature — and where it listens
depends on the product: the SecuraCV app's kernel listens inside its own
container with its host port disabled, and the Docker sidecar's on every
interface with port 8799 published from the first start, so Home Assistant
can pair over the LAN (what that opens, and what crosses the wire, is
[below](#home-assistant-pairing-and-what-crosses-your-network)). The
iPhone app pins the certificate fingerprint in the receipt's `tls_cert_fp`
when a unit hands one out (the WAP's pairing receipt and QR, and the
flagship's receipt on its HTTPS builds, which `/api/status` also reports;
no stock unit has one to hand out today), refuses an https Canary it
cannot check, and offers a plain-http credential push only when nothing
encrypted reaches the device and only behind a disclosure you switch on.
Compile-tested by CI; whether a given unit is serving HTTPS is on its serial
log and in its pairing receipt (and, on the flagship, in `/api/status`).

**One link is yours to encrypt: the MQTT broker.** A Canary that publishes
to a broker opens that socket in plain text by default, so the broker
username and password cross your LAN unencrypted until you provision TLS —
verified against a CA you supply, or pinned to the broker certificate's
SHA-256 fingerprint (which products support which is in
[`docs/FIRMWARE_VARIANT_AUDIT.md`](../FIRMWARE_VARIANT_AUDIT.md)). An
incomplete TLS setup refuses to connect rather than quietly downgrading, and
the unverified "lab" mode exists only as a mode chosen by name that warns on
every connect. Provisioning the broker password over
`POST /api/mqtt/config` crosses the LAN in the clear like the rest of the
device API on every stock unit of either tree today, since none serves HTTPS
(F15); the flagship's release images stay plain HTTP after that too, until
HTTPS is turned on for them. A `POST /api/mqtt/config` that moves
the broker (a new host or port) while a password is stored and the body
names none is refused `400 password_required_for_new_host` from any caller
(`mqtt_tls_fields::credential_carry`; the flagship has no dashboard session,
and a TLS-mode or CA change alone is not a move there). On the WAP, the same
save from the dashboard session alone is refused `403 password_needed` when
it moves the host, port, TLS mode or CA; the bearer, or the password typed
again, moves it (sweep F289). This is compile-tested, host-tested, not yet
bench-tested.

**One read on the hub is open on purpose: the fleet roll-call**
(`GET /api/fleet`). It answers anyone who can reach the kernel's port with
the coarse words the Witness Wall paints — each Canary's name, whether it is
online, its chain verdict and product, and the presence / occupants /
breathing words while it is proven online — never an event, a zone or a key.
The origin allow-list only stops other websites' scripts, not a device on
your LAN, so the port is open only where the product needs it: the
SecuraCV app ships its 8799 host port disabled (the integration reaches the
kernel over the Supervisor's internal network, and the Witness Wall is the
reason to enable it), and the Docker sidecar — which publishes 8799 so Home
Assistant can pair — serves **only the kernel's own row** there unless you
set `SECURACV_FLEET_ROLLCALL=1`: who is home is yours to opt into, never a
side effect of pairing. And "online" there is a verified, chain-advancing
signature seen within three minutes — stronger than a heartbeat, but not a
liveness proof against someone who can publish on your MQTT broker
(`tvos/discovery/DISCOVERY.md` spells out the difference).

### Home Assistant pairing and what crosses your network

The SecuraCV integration reads the witness kernel over HTTP and MQTT. What
each link carries, product by product, and the bounds on each:

**The Docker sidecar publishes port 8799 by default** and the kernel binds
every interface in its container, because that is how Home Assistant pairs
with a kernel that has no Supervisor. Anyone who can reach the host gets
three things: `GET /health` (liveness), `POST /api/pair` (below) and
`GET /api/fleet` (the kernel's own row only, unless you opt in). Every data
route — events, the digest, the sealed log, the export, the identity, the
status — needs a token: the rotating capability token, which never leaves
the container, or the paired token Home Assistant earned. There is no
"LAN policy" switch; the tokens are the boundary, and
`SECURACV_API_BIND=loopback` (with the port mapping deleted) is the opt-out
that also ends pairing.

**Pairing is a 6-digit code with a hard budget.** `docker exec securacv
pair` prints the code once (never the daemon log), with the kernel key's
fingerprint for you to compare on the Home Assistant side. The kernel keeps
only the code's sha256, its 10-minute expiry and a budget of five attempts;
each wrong guess spends one durably **before** the refusal is sent, the fifth
burns the code, and an expired code is deleted on first touch. Every refusal
— wrong, absent, burned, expired, or a kernel with pairing off — is the
identical `401 invalid_pairing_code`, so a guesser learns nothing from the
answer, and each one is a strike toward the kernel's per-address lockout.
At one million codes, five tries and ten minutes, the odds against a guess
are what they look like; the lockout is the second bound. One honest limit:
behind Docker Desktop's userland proxy, or any reverse proxy, every client
shares one source address, so the lockout a guesser earns also holds Home
Assistant's polling at `429` for its length.

**The paired token is a bearer secret in plaintext until TLS lands.**
Home Assistant holds the 32-byte token the pairing minted; the kernel holds
its sha256 (`paired_tokens.json`, `0600`), re-read per request, so a
revocation (`docker exec securacv unpair <id>`, `DELETE /api/pair/<id>`, or
removing the integration entry) takes effect on the next call. The sidecar
serves plain HTTP today, so that token crosses your LAN in the clear on
every request a host that can watch your network could read — the same
exposure as the capability token and the Wall's viewer token. Keep the port
on your own network and never forward it; TLS for the sidecar (a self-signed
certificate whose fingerprint `pair` prints and Home Assistant pins) is the
tracked follow-up. The app has no such exposure: its token file is read
over the Supervisor's internal network and its host port is disabled.

**Verified means the integration checked.** Home Assistant pins the kernel's
Ed25519 verifying key only over the authenticated HTTP channel — the
`/api/pair` answer, or `/api/identity` through the app's token file — never
from the broadcast announcement. "Verified ✓" on an event means the
integration itself fetched the sealed log and checked every hash link and
signature against that pin; a key that changes raises a Repairs issue that
asks you to compare fingerprints before it rotates the pin, and is never
rendered as a failure or quietly adopted.

**The announcement on your broker names the kernel.** The kernel's MQTT
publisher retains a signed announcement (`witness/<kernel id>/announce`):
the kernel's id, its public verifying key, where its API listens, the
pairing posture, and a bucket-start timestamp — never a token, a code or the
seed, and never a host unless you set `SECURACV_ANNOUNCE_HOST` (a host is a
network identifier, and Home Assistant redacts it in diagnostics). The
signature lets a pinned integration refuse an announcement the kernel did
not sign. This is fleet-internal reconciliation on a broker you run — the
same Invariant III posture as the Canaries' own retained status topics —
not telemetry: nothing leaves your network, and the broker's own access
control is the boundary. The integration's claim on the same broker
(`witness/claim/<kernel id>`) carries a random nonce and a bucket start,
never an entry id.

**The app holds no Supervisor role.** The SecuraCV app calls only its own
options, the shared MQTT service, the add-on info reads its preflight
needs, the discovery endpoint and Home Assistant's own API; "Install it for
me" is a my.home-assistant.io link, and the one Core restart happens on your
click. The integration the app carries is copied only on that click (first
install) or, on app start, into a copy that carries the app's own marker and
a matching sha256 manifest — never a copy HACS or you installed, never a
downgrade, never followed by an automatic restart (a notification asks for
one). A custom AppArmor profile confines each process to the paths it needs.

**Diagnostics are projections, not copies.** A Home Assistant diagnostics
dump of the integration carries shapes — the kernel URL's scheme and port,
a four-hex prefix of the kernel id, fingerprints cut to eight characters,
availability flags, the latest event's type, window and attestation, digest
counts, per-Canary firmware and online state — and is then swept by
Home Assistant's redaction over tokens, the pairing token id, hosts, URLs,
device ids, keys, hashes, entry ids, nonces, IP and MAC addresses, SSIDs,
names, coordinates and the seed, so a value that reaches the dump through
a path the projection did not foresee is still replaced.

**Evidence exports sit behind Home Assistant's authentication.** An export
is one zip under `<config>/securacv/evidence` (directory `0700`, files
`0600`, never under `www/` or `/local`), served only by an authenticated
Home Assistant view through a link signed for 24 hours — in the export
notification and under Media → SecuraCV evidence — and each export is a
receipted disclosure on the kernel. The pack carries the coarse sealed
record, the offline viewer and the public witness key, nothing a stranger
could use against the kernel; the receipt names the credential that asked
(`auth_mode: paired` with the token id for a paired Home Assistant).

---

## Who Cannot Access Your Data

| Who | Why Not |
|-----|---------|
| **ERRERlabs** (the manufacturer) | We have no remote access capability. No backdoor exists. |
| **Law enforcement** (without the physical device) | There is no ERRERlabs server to subpoena. A Canary talks only to the broker and hub you run and to the update host, which sees an anonymous manifest fetch; the evidence lives on the device and on your hub. |
| **Network observers** | They see the disclosed paths and nothing else: a broker session (readable on the wire until you provision TLS), a daily signed-manifest fetch and SNTP from every product without GPS. No path carries an identifier beyond what your own broker login already is. On the hub they also see the kernel's plain HTTP where it crosses the LAN — the Docker sidecar's paired token on every Home Assistant request until TLS lands for it — and the coarse claims and the signed announcement on your own broker. |
| **Other WiFi users** | Each device has a unique, randomly derived password. |
| **Remote attackers** | Nothing listens beyond your network and nothing is exposed to the internet; the outbound paths are client-initiated to hosts you chose. Encryption on the device's own API is not the default: the WAP turns HTTPS on by itself after setup once a certificate is stored, the flagship serves it only in its `FEATURE_HTTPS` builds, from the first boot after setup (neither Canary serves it on any stock unit today, F15), and the kernel only with its `api-tls` build feature; and the MQTT broker link is TLS only once you provision it (plain by default). |
| **ERRERlabs under court order** | We cannot comply because we have nothing — no keys, no data, no access. |

---

## Cryptographic Design

The device uses well-vetted, standard cryptographic primitives:

| Purpose | Algorithm | Library |
|---------|-----------|---------|
| Device identity & record signing | Ed25519 | Arduino Crypto (Rhys Weatherley) |
| Chain integrity & domain separation | SHA-256 | mbedTLS (ESP-IDF) |
| API token derivation | HMAC-SHA256 / HKDF | mbedTLS (ESP-IDF) |
| Transport encryption (the WAP's HTTPS, automatic after setup once a certificate is stored; the flagship's `FEATURE_HTTPS` builds from the first boot after setup — neither Canary serves it on any stock unit today, F15; the kernel's `api-tls` feature) | TLS 1.2+ (RSA-2048 self-signed on the WAP; ECDSA P-256 self-signed on the flagship's `FEATURE_HTTPS` builds, CI-compiled in `dev` and `full` and never run on hardware) | mbedTLS (ESP-IDF); rustls (kernel) |
| Broker link (MQTT over TLS, when provisioned) | TLS 1.2+; CA chain verification on all five products; SHA-256 certificate pin and a named unverified lab mode on canary-display (not the plain-only nightstand-c6), canary-sense, canary-vision and the `firmware/canary` flagship; canary-wap is CA-only (esp_mqtt has no pin hook). Compile-tested, host-tested, not bench-tested | mbedTLS via WiFiClientSecure (display / sense / vision / flagship) and esp-tls (canary-wap) |
| At-rest event database (kernel) | SQLCipher (AES-256), key derived from the device seed | SQLCipher via rusqlite |

No custom cryptographic implementations are used. All primitives come
from established, audited libraries.

---

## Tamper Evidence

The witness chain is append-only:

- **Records can be added** — each new record extends the chain
- **Records cannot be modified undetectably** — changing any record breaks the chain
- **Records cannot be deleted undetectably** — missing sequence numbers are detectable
- **The chain break itself is evidence** — tampering is visible to anyone
  who checks

To forge a record, an adversary would need:
1. The device's Ed25519 private key (which never leaves the device)
2. To re-hash and re-sign every subsequent record in the chain

To hide a deletion, an adversary would need to re-sign the entire chain
from the deletion point forward — which again requires the private key.

---

## Evidence Verification

Evidence exported from this device can be verified by **anyone**,
**anywhere**, without contacting ERRERlabs or any other service:

### Option 1: Built-in Verifier
Open the included `verification.html` file in any web browser.
It runs entirely offline — no internet connection required.

### Option 2: Online Verifier
Visit verify.securacv.com and upload the export bundle.

### Option 3: Manual Verification
Using any Ed25519 implementation:
1. The device's public key is in `device_identity.json`
2. Each record's Ed25519 signature can be checked independently
3. The SHA-256 hash chain can be recomputed from the first record
4. Sequence numbers can be checked for gaps

**No ERRERlabs service, account, or software is required for
verification.** If ERRERlabs ceased to exist tomorrow, all previously
exported evidence would remain fully verifiable.

---

## Known Limitations

We believe transparency about limitations is as important as describing
capabilities. The following are known constraints:

### Hardware Trust Boundary
The Canaries are built on Espressif ESP32-family microcontrollers (S3,
C3, C6 and the classic ESP32 — `firmware/boards/boards.json`).
Espressif is a Chinese semiconductor company. The WiFi firmware contains
proprietary binary blobs from Espressif. A sufficiently resourced
state-level adversary could theoretically compromise these blobs.
The mitigation that always applies is architectural: the firmware opens
no undisclosed outbound path — the few it has are named above, two of
them pinned by host tests — so a compromised blob would have to
originate traffic of its own, on a network whose expected traffic is
short and known, and a WAP on its own access point gives it no route
out at all. Secure boot and flash encryption are additional
mitigations, but they are **off unless you turn them on** — see
[Physical extraction and the flash-encryption default](#physical-extraction-and-the-flash-encryption-default).

### Physical Access
Physical possession of the device enables forensic analysis of the
SD card and flash storage. On a device in its default configuration this
includes recovering the device's private key; the section below states
exactly what that does and does not let an attacker do.

### GPS Spoofing
The device cannot independently verify GPS signals. A sophisticated
adversary with GPS spoofing equipment could cause the device to record
false location data. The device records what its GPS receiver reports,
not objective ground truth.

### Clock Accuracy
Timestamps come from GPS satellites when a fix is available. Without
GPS fix, the device uses its internal clock, which may drift. All
timestamps are coarsened to ten-minute buckets regardless of source.

### Evidence Scope
The device records metadata about events, not comprehensive multimedia
evidence. It proves that *something was recorded at a time and place*,
not necessarily *what happened* in full detail.

### Physical extraction and the flash-encryption default

**The honest statement: on a Canary in its default configuration, someone
who takes the device away and opens it up can read the private key out of
the flash chip.**

This is a deliberate default, not an oversight, and the trade is worth
understanding because it is the one place where "keys never leave the
device" needs an asterisk.

ESP32-family chips support Secure Boot and flash encryption, which together
make the firmware image unreadable and stop unsigned firmware running.
(Flash encryption alone does not cover NVS, where the key lives; that
takes NVS encryption on top — see the end of this paragraph.)
They are also **irreversible**: they are burned into one-time fuses. A
device with them enabled and a lost key is a brick, permanently, with no
recovery path — not for you, and not for us. We decided that a default
that can permanently destroy an owner's device is the wrong default for a
device people are supposed to be able to keep, repair, and re-flash. So
[the tiered design](../design/hardware_root_of_trust.md) puts the
reversible protections in the default path and leaves the irreversible
lockdown as an explicit, key-backup-enforced opt-in. The settings are
staged and commented in `firmware/provisioning/sdkconfig.defaults.secure`.
The PIO canary image (`firmware/canary`) does not leave you to assume
which case yours is: it reports where its identity key sleeps as
`key_at_rest` (`plaintext-nvs`, `nvs-encrypted` or
`nvs-encrypted+secure-boot`) in `/api/status`, the health export, the `f`
console card and the `j` self-manifest. Today that answer is always
`plaintext-nvs`, on a fused board too: ESP-IDF's flash encryption encrypts
the app, OTA-data and NVS-key partitions but not NVS itself, and the NVS
encryption that would cover it is not available in that image's
Arduino-framework build. No other firmware tree (canary-wap,
canary-sense, canary-vision, canary-display, ...) reports `key_at_rest`
yet.

**What key recovery gets an attacker.** They can sign new records as that
device. From that point on, a chain they produce is cryptographically
indistinguishable from one the real device produced.

**What it does not get them.** It does not rewrite the past. The log is
hash-chained, so altering an existing record breaks every record after it;
and where chain heads have been anchored to an external timestamp
authority (`log_anchor`, RFC 3161), the history up to each anchor is
pinned by a signature that is not the device's and that key theft does not
provide. Forging *forward* from a stolen key is achievable. Forging
*backward* past an anchor means defeating the timestamp authority as well.
It also does not decrypt sealed material, and that holds on both sides
for the same structural reason — the decryption key is somewhere the
device isn't. Sealed snapshots on the device are encrypted to an
**operator-held X25519 public key whose private half never touches the
device at all** (see [`../sealed_snapshot_vault.md`](../sealed_snapshot_vault.md)),
so there is nothing on the flash to recover that would open them. Vault
material on the hub is protected by a separate hub-held key under the
break-glass policy. Neither is the device identity key, so stealing the
identity key opens neither.

**It is also detectable, after the fact.** A cloned identity produces two
divergent chains from the same device key. Two chains that share a prefix
and then disagree is not something the real device can do, so if both ever
reach a verifier, the clone is provable.

**If your threat model includes device seizure**, this is the case the
opt-in tiers exist for. Turn them on deliberately, back up the key first,
and understand that you are trading recoverability for extraction
resistance. Read [`../design/hardware_root_of_trust.md`](../design/hardware_root_of_trust.md)
before you burn a fuse — that decision cannot be walked back.

**If your threat model is an intruder, a landlord, or a domestic abuser
who does not take the device to a lab**, the default is the right one for
you, and the tamper-evidence properties are unaffected.

---

## The Design Standard

This device is designed for people whose safety may depend on it:
tenants documenting unauthorized entry, journalists protecting source
meetings, activists recording police presence, domestic abuse survivors
documenting patterns, whistleblowers who need tamper-evident records.

Every design decision is evaluated against the most vulnerable user.
If the device fails for a journalist in a hostile state, it does not
matter that it works for an insurance claim.

The security model is based on **architecture, not promises**:

```
Promise-based security:       "We won't look at your data."
Architecture-based security:  "We CAN'T look at your data."

SecuraCV Canary implements the latter.
```

---

## Open Source

The firmware source code is publicly available at:
https://github.com/kmay89/securaCV

Anyone can:
- Audit the code for backdoors or vulnerabilities
- Build the firmware from source and compare it to what ships on the device
- Verify that the security claims in this document match the implementation
- Fork the project and modify it for their own needs

---

## Contact

To report a security vulnerability:
https://github.com/kmay89/securaCV/issues (use the Security Report template)

This document is versioned alongside the firmware. The version that
matches your device's firmware is the authoritative reference.
