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
    { "name": "Front Door", "online": true,  "chain": "ok", "product": "canary-wap" },
    { "name": "Studio",     "online": true,  "chain": "ok", "product": "canary" },
    { "name": "Driveway",   "online": false, "chain": "ok", "product": "canary-vision" }
  ]
}
```

- Only `devices[].name` is required; `online` defaults to `true`. `chain` and
  `product` are optional and shown when present. A bare JSON array of devices is
  also accepted.
- **No secrets, no raw media** — this is coarse fleet *presence and health*,
  exactly what the Witness Wall renders. It is not an evidence API.

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

- **The hub/kernel** (Rust) is the natural home — add the `/api/fleet` route
  with the CORS headers above next to its existing HTTP surface.
- **`canary-wap`** already serves a web UI; the same route + `MDNS.begin("canary")`
  (advertising `canary.local`) makes a single device discoverable with no hub.
- Keep it **coarse and unauthenticated-read** (presence/health only). Anything
  that touches sealed evidence stays behind break-glass, never here.
