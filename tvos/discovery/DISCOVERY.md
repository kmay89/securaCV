# Fleet discovery — how the Witness Wall finds your Canaries from a browser

The emulator's **Connect your fleet** panel (and the real tvOS app) shows your
actual Canaries when they're reachable. This is the small, honest contract that
makes that work — and the browser rules that decide *where* it can work.

## The contract

A SecuraCV **kernel/hub** (or a Canary that fronts the fleet) answers one
endpoint:

```
GET /api/fleet        →  200 application/json
```

```json
{
  "kernel": "kitchen-hub",
  "verified_through": "4:02 PM",
  "devices": [
    { "name": "Front Door", "online": true,  "chain": "ok", "product": "canary-wap", "hub": "ok" },
    { "name": "Studio",     "online": true,  "chain": "ok", "product": "canary" },
    { "name": "Driveway",   "online": false, "chain": "ok", "product": "canary-vision", "hw": "xiao-esp32c3" }
  ]
}
```

- Only `devices[].name` is required; `online` defaults to `false` (a silent
  field is never a presence claim — pinned by
  `tvos/witness-core/tests/fixtures/fleet_contract_vectors.json`, which the
  core's `fleet_contract.rs` test replays). `chain`, `product`, `hw`, and
  `hub` are optional and shown when present. A bare JSON array of devices is
  also accepted.
- `hw` names the **board** the device compiled against (each pins header's
  `CANARY_FIGURE_HARDWARE`, e.g. `"waveshare-esp32s3-lcd7"`). It is the only
  field exact about the device's *shape* — several products share one
  `product` string — so it is what resolves the figure a client draws.
  Devices on firmware older than the field simply omit it.
- `hub` is where the device stands with its hub, as a word: `"none"` (nobody
  configured one), `"down"` (configured, unreachable), `"ok"`. Absent means
  the device did not say — which a client must never render as "fine".
- **Optional wellbeing keys** (a row may carry them; most rows never will):
  `presence` (`"clear"`/`"present"`), `occupants` (`"0"`/`"1"`/`"2+"`),
  `breathing` (`true`/`false` — a breathing lock held or lapsed), and
  `seeing` (`"person"`/`"vehicle"`/`"animal"`/`"package"`, optionally with
  `seeing_score` 0–100). Words on purpose, so an unknown value falls back to
  unknown instead of misreading. Every key is **absent unless the device can
  honestly say** — a stale reading omits rather than lies, and a client must
  render absence as "cannot say", never as an empty calm room. These are the
  same coarse facts the fleet already tells anyone in radio range over the
  BLE presence beacon (the v2 detect class) and the display's glass (`wb`/
  `br`); this surface deliberately carries **nothing finer** — no range, no
  lux, and no vital-sign numbers (BPM is P1-gated on the device itself).
- **No secrets, no raw media** — this is coarse fleet *presence and health*,
  exactly what the Witness Wall renders. It is not an evidence API.

### The optional verification endpoint (the kernel serves it, token-gated)

The native tvOS app also asks its source, every poll cycle, for

```
GET /api/sealed-log   →  200 application/json   (optional)
```

— the sealed-log document its Rust core verifies (`{ "verifying_key",
"checkpoint_head"?, "entries": [...] }`, see
`tvos/witness-core/include/securacv_witness_core.h`). **The repo-root
kernel now serves this endpoint** — a checkpoint-anchored, size-capped
tail, entries' `payload` byte-identical to storage, with **no query
surface of any kind** (Invariant VII: the log is non-queryable, so there
is nothing to filter, select, or search) — behind the same capability
token as its other authenticated routes. One rule binds every consumer:
the document's `verifying_key` is the endpoint's **claim** about itself,
so a client may say "Verified" only after comparing it (or, across a key
rotation, the signed lineage in `rotation_records`) against a key **pinned
at pairing** — the repo-wide verified-means-Ed25519-vs-pinned-key
discipline — and proves continuity across polls by remembering the last
head it walked. A walk that trusts the served key verifies internal
consistency, not provenance, and must not wear the word. No firmware serves it, and the TV sends no token
yet, so for the Wall its absence (or a 401) remains an answer, not an
error: the Wall phrases the fleet's status as the devices' own report
("Your fleet reported in through <this TV's own receipt time>", with the
device's self-stamped `verified_through` shown only as "Device reports …",
because the firmware fills that field with the literal word "now") and
reserves the word "Verified" for a chain it actually walked against a key
pinned at pairing. The day the TV holds a token, its
verification lights up with no app change. Everything above about
`/api/fleet` being coarse and unauthenticated is exactly why this endpoint
is separate — and gated: the sealed log is how a *display* gets to say
something cryptographic instead of repeating the wire, and the full coarse
record is more than "anyone who asks" should hold.

### CORS is the whole trick — for browsers, and only known ones

A browser page and the kernel are different origins, so without an
`Access-Control-Allow-Origin` the browser refuses to read the response even
when the source replies. Native readers — the Witness Wall on tvOS and the
iPhone app, both `URLSession` — send no `Origin` header and need no CORS at
all. So the header exists for browser pages, and the two kinds of source
answer it differently today:

- **Firmware boards** (`canary-wap`, `canary-display`) still answer with
  `Access-Control-Allow-Origin: *` and the matching `OPTIONS` preflight, so a
  single Canary is readable from any page on the LAN. The display already
  narrows what that audience gets: a cross-site `Origin` is served liveness,
  product and hub state only, and the room/breathing words ride only on
  requests with no `Origin` or a same-site one (`glass_web.cpp`).
- **The kernel** (`src/api/mod.rs`, `FLEET_ALLOWED_ORIGINS`) answers from an
  origin allow-list instead of a wildcard, because the aggregated document
  says who is home and a drive-by page a household member happens to open
  must not learn that:
  - no `Origin` (native apps, curl) → served, no CORS header;
  - an allowed `Origin` → served with that origin echoed back (never `*`),
    `Access-Control-Allow-Methods: GET, OPTIONS`, and `Vary: Origin`; the
    `OPTIONS` preflight answers the same way;
  - any other `Origin` → served, but with no `Access-Control-*` header at
    all, so the browser blocks the read.

  Allowed today: `https://kmay89.github.io` (the in-browser Lab and flasher,
  the `flasher.url` origin in `firmware/build_matrix.json`),
  `https://securacv.com` (the public site's emulator), and `http://localhost`
  / `http://127.0.0.1` on any port (a local checkout of either site — the
  "try it in ~30 seconds" path below). The list is a constant with a comment
  per entry; there is no config key for it because nothing else in the
  kernel's config carries origins. Note the two `https` origins only matter
  for a kernel served over TLS with a trusted certificate — a plain-`http`
  kernel is mixed content for an `https` page whatever the CORS headers say
  (the table below). `tvos/discovery/mock-kernel.py` still answers `*`; it is
  a mock.

## Where it actually works (the honest part)

Browsers deliberately can't do mDNS/Bonjour discovery or scan a LAN, and a
strict page policy blocks reaching other hosts. So discovery works when:

| Scenario | Works? | Why |
|---|---|---|
| Emulator served **from the kernel/hub** itself (same origin) | ✅ | Same origin — no CORS, no mixed content, no CSP hop |
| Emulator opened **on the LAN over `http://`**, source on `http://` | ✅ firmware · ❌ kernel | Same scheme, and a firmware board's `*` allows it; the kernel's allow-list does not know a LAN page's origin (`http://192.168.…`), so it serves the row but the browser cannot read it — serve the page on `localhost` instead, or from the hub itself |
| Emulator on **`http://localhost:<port>`** (a local site checkout), kernel on `http://` | ✅ | Same scheme; `localhost` / `127.0.0.1` are on the kernel's allow-list |
| The **desktop app** (native mDNS, no browser sandbox) | ✅ | It discovers `canary.local` natively and hands the list to the UI |
| Kernel serves **`https://` with a trusted cert**, page is `https://` | ✅ | Same scheme; CORS allows the read |
| The **public `https://securacv.com`** page → your `http://` LAN device | ❌ | Mixed-content is blocked, and the site's `connect-src` policy only allows itself |

That last row is why the public demo ships a **same-origin live demo kernel**
(`/demo-fleet.json`) so you can watch a real fetch populate the fleet in the
browser — and why the *real* fleet shows up once the page is served next to the
kernel (hub, LAN, or desktop app).

### What the Wall does with an advert (the native tvOS app)

The Apple TV has no browser sandbox, so beyond probing `canary.local` it
browses the `_securacv._tcp` Bonjour service every Canary announces and reads
the TXT `host` key — the salted per-unit mDNS hostname the firmware writes.
An advert is a claim anyone on the LAN can make, so two rules bound it:

- **The host is validated before it is ever dialed** — the same private-host
  rule the iPhone applies to every base URL (`DeviceAPI.isPrivate`),
  restated once in Rust (`tvos/witness-core/src/host.rs`) and called from
  Swift: a bare DNS label (qualified to `<label>.local`), a `.local` name of
  well-formed labels, or a private IPv4 address (10/8, 172.16/12,
  192.168/16, 169.254/16, 127/8). A public name or address is logged and
  skipped; it never becomes a source.
- **Discovered sources expire.** The Wall stores when each source last served
  a real fleet and drops any that has not answered in 30 days on the next
  load, so a Canary that moved out — or a stranger's box that announced
  itself once — is not polled by a television forever. A typed hub address
  is one deliberate entry and is not aged out.

## Try it in ~30 seconds

Run the reference kernel and point the emulator at it:

```sh
python3 tvos/discovery/mock-kernel.py         # serves http://localhost:8099/api/fleet (CORS on)
```

Then open the emulator on the **same origin scheme** — the simplest is to serve
the site locally over http and browse to it:

```sh
# from the securacv_website checkout
python3 -m http.server 8080
# open http://localhost:8080/witness-wall.html, and in "Connect your fleet"
# enter  http://localhost:8099  → Connect
```

The board switches to **● LIVE** and lists the reference Canaries. Swap in your
real kernel's address the same way.

## Making a real Canary answer this

**The firmware answers it now.** `GET /api/fleet` ships in the firmware, served
identically by every networked board because the wire shape is built by one
shared header — `firmware/common/fleet_selfreport/fleet_selfreport.h` (host-
tested in `firmware/tests_host/test_fleet_selfreport.cpp`). See
[`docs/FLEET_PARITY.md`](../../docs/FLEET_PARITY.md) for the "parity by
architecture" doctrine that makes a fleet-wide capability like this a
one-header change instead of a per-board copy-paste.

- **`canary-wap`** (ESP-IDF `esp_http_server`) answers `GET /api/fleet` and the
  CORS `OPTIONS` preflight, and already advertises `canary.local` — so a single
  device is discoverable with no hub. This is exactly what the Flasher's
  post-flash `witness_discover` hits.
- **`canary-display`** (Arduino `WebServer`) answers the same contract from the
  *other* server style — the parity core means both emit byte-identical JSON. A
  display holds no witness chain of its own, so it honestly reports
  `chain: "unknown"`.
- **The hub/kernel** (Rust, `src/api/mod.rs`) serves `GET /api/fleet` and its
  `OPTIONS` preflight with the allow-list CORS above, open and rate-limited
  like the rest of its surface but with no token. Its first row is itself —
  `product: "witness-kernel"`, and `chain` only once a verify pass has
  actually run (the key is absent before that — a silent key is never a
  claim). **The rows after it are the Canaries the hub has heard**, which is
  what makes the Wall light up against a kernel:
  - The kernel never speaks MQTT; `event_mqtt_bridge --fleet-peers-path
    <file>` does (both processes also read the same environment variable,
    `WITNESS_FLEET_PEERS_PATH`, so one export points them at one file). With
    that flag the bridge subscribes to each Canary's
    `securacv/<device_id>/{availability,status,health,chain,state,meta}`
    topics — never `events`, `sensing` or `counts` — pins the public key from
    the first `health` it sees per device id (trust on first use, as the
    display's NVS pin store does; here the pin lives in the summary file and
    survives a bridge restart with it), verifies the Ed25519 signature on
    every `chain` publish against that pin, and writes that summary file
    (`securacv/fleet_peers/v1`) with an atomic rename. The kernel reads that
    file on every request and projects it through an allowlist
    (`src/fleet_peers.rs`, `fleet_rows`): the file is local bookkeeping and
    nothing in it — the pinned key, the device id, timestamps — reaches the
    wire unless the projection names it. A file rather than a kernel
    endpoint because the event API's parser is header-only by design (no
    request bodies), and because the token already crosses between the two
    processes as a path on the same host.
  - `name` is the owner's name from the retained `meta` topic, else the
    device id (exactly what a Canary calls itself in its own self-report);
    `product` is the announced `device_type`.
  - `online: true` is a **proof, not a heartbeat**: a LIVE (not
    broker-retained) `chain` publish whose signature verified against the
    pin within the last 180 s (`FLEET_PEER_RECENT_SECS`, the display's own
    `stale_after_ms`), with no LWT `offline` since. `status`/`availability`
    heartbeats are unsigned — anyone on the broker can publish them — so they
    never prove presence. Because Canaries sign on each record they seal
    rather than on a timer, a quiet Canary honestly reads `online: false`,
    which this contract defines as "not claimed present", never as "claimed
    absent".
  - `chain` is `"ok"` when the last signed chain publish verified against
    the pin, `"degraded"` when a signature failed or a second key appeared
    for a pinned id (sticky; only deleting the summary file clears it), and
    absent when nothing signed has been checkable. This is verification
    against a key pinned on first sight, which is weaker than a key pinned at
    pairing; the Wall must not call it more than that.
  - The wellbeing words (`presence`, `occupants`, `breathing`) ride on a
    peer's row only while it is proven online **and** the reading is fresh
    (a live `state` publish within the same window); a retained `state` is
    history, not a reading. Only the contract's words cross — an unknown
    word is dropped at the bridge, never republished. `seeing` has no MQTT
    producer yet, same as on firmware.
  - Pinned to the shared vector: the kernel's two-row bytes (itself plus one
    Canary) are the `input` of a vector in
    `tvos/witness-core/tests/fixtures/fleet_contract_vectors.json`, checked
    from both sides (`src/api` tests and the core's `fleet_contract.rs`).
- It is **coarse and unauthenticated-read** by design — presence, health,
  and the optional coarse wellbeing WORDS above, nothing finer — documented
  public in the canary-wap route-security allowlist. Anything that touches
  sealed evidence stays behind the Bearer-gated `/api/fleet-scan` and
  break-glass paths, never here.
