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

- Only `devices[].name` is required; `online` defaults to `true`. `chain`,
  `product`, `hw`, and `hub` are optional and shown when present. A bare JSON
  array of devices is also accepted.
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
("Your fleet reports verified through …") and reserves the word "Verified"
for a chain it actually walked. The day the TV holds a token, its
verification lights up with no app change. Everything above about
`/api/fleet` being coarse and unauthenticated is exactly why this endpoint
is separate — and gated: the sealed log is how a *display* gets to say
something cryptographic instead of repeating the wire, and the full coarse
record is more than "anyone who asks" should hold.

### CORS is the whole trick

Because a browser page and the kernel are different origins, the kernel MUST
send:

```
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, OPTIONS
```

and answer a `OPTIONS /api/fleet` preflight with the same. Without CORS the
browser refuses to read the response even when the kernel replies.

## Where it actually works (the honest part)

Browsers deliberately can't do mDNS/Bonjour discovery or scan a LAN, and a
strict page policy blocks reaching other hosts. So discovery works when:

| Scenario | Works? | Why |
|---|---|---|
| Emulator served **from the kernel/hub** itself (same origin) | ✅ | Same origin — no CORS, no mixed content, no CSP hop |
| Emulator opened **on the LAN over `http://`**, kernel on `http://` | ✅ | Same scheme; kernel just needs CORS |
| The **desktop app** (native mDNS, no browser sandbox) | ✅ | It discovers `canary.local` natively and hands the list to the UI |
| Kernel serves **`https://` with a trusted cert**, page is `https://` | ✅ | Same scheme; CORS allows the read |
| The **public `https://securacv.com`** page → your `http://` LAN device | ❌ | Mixed-content is blocked, and the site's `connect-src` policy only allows itself |

That last row is why the public demo ships a **same-origin live demo kernel**
(`/demo-fleet.json`) so you can watch a real fetch populate the fleet in the
browser — and why the *real* fleet shows up once the page is served next to the
kernel (hub, LAN, or desktop app).

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
- **The hub/kernel** (Rust) is the natural aggregator home — add the `/api/fleet`
  route with the CORS headers above next to its existing HTTP surface; it can
  reuse the same open/append/close shape to list its peers.
- It is **coarse and unauthenticated-read** by design — presence, health,
  and the optional coarse wellbeing WORDS above, nothing finer — documented
  public in the canary-wap route-security allowlist. Anything that touches
  sealed evidence stays behind the Bearer-gated `/api/fleet-scan` and
  break-glass paths, never here.
