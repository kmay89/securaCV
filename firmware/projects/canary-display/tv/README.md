# Canary TV — securaCV on the television you already own

A 10-foot ambient security surface. It reads the **same `/api/glass`
snapshot** the wall glass and phone mirror read, so it needs **no hub**: a
television on your home WiFi, pointed at a Canary, is a conformant
[Open Ambient Security Display](../../../../docs/standard/AMBIENT_DISPLAY_STANDARD.md).

There are no camera feeds here — securaCV witnesses without watching. The TV
shows the fleet's worst state at a glance, a roll-call of every Canary, and
degrades honestly when a witness goes silent or the signal drops.

For the product rationale, competitive picture, and the "Canary TV" dongle
proposal, see [`docs/hardware/tv_display_design.md`](../../../../docs/hardware/tv_display_design.md).

## Three ways to put it on a TV (cheapest first)

1. **A smart TV's own browser.** Open `http://<canary>.local/tv` (or the
   Canary's IP + `/tv`). Zero extra hardware. Best when the TV has a usable
   browser app.
2. **A cast / HDMI stick.** Any ~$30 stick that can open a URL in kiosk /
   fullscreen mode → point it at the same address. Works with TVs that have
   no decent built-in browser.
3. **A "Canary TV" dongle (our version).** Ships this page and boots straight
   into it, auto-discovers the fleet over mDNS, and stays a dumb renderer so
   it lasts for years. Design in the doc above.

## Try it right now (no hardware)

Open `index.html?demo=1` in any browser — it renders a synthetic fleet,
loudly labeled **DEMO · NOT A LIVE FLEET** so it can never be mistaken for a
real one (per [`display_modes.md`](../../../../docs/hardware/display_modes.md)).

```
firmware/projects/canary-display/tv/index.html?demo=1
```

## Query options

| Param | Default | Meaning |
|-------|---------|---------|
| `src=http://192.168.1.42` | same origin | Data origin. Omit when the page is served **by** the Canary (`/tv`) — then it's same-origin and needs no config or CORS. Set it for a stick that serves the page locally and points at a Canary's IP. |
| `demo=1` | off | Render a synthetic fleet, labeled DEMO. |
| `rooms=1` | off | Group the roll-call by room instead of by severity. |
| `poll=2000` | 2000 | Snapshot poll interval, milliseconds. |

## How it honors the Ambient Display Standard

- **Silence ≠ safety** — each Canary's `age_s` re-derives a liveness label
  client-side: quiet past 3 min → amber "Quiet a while", past 10 min → red
  "Silent", always tagged with "last seen …". A lost connection banners
  "Signal lost — showing last known state from …" within one render cycle.
- **Color never alone** — every state carries a word + glyph + a border
  position, not just a hue (WCAG 1.4.1).
- **No overclaiming** — a browser can't verify Ed25519 on its own silicon, so
  the page never says "verified". It states its source honestly ("reading a
  Canary directly — no hub").
- **Calm** — the surface is still at rest; the only motion is a slow
  sub-perceptual burn-in drift and, while an alert is *unacknowledged*, one
  breathing pulse on the hero. Night flag (or a local clock fallback)
  red-shifts and dims. No ads, ever.
- **Resilient** — polling backs off when a Canary is unreachable but keeps
  the last-known frame on screen under the honest banner.

## Editing

`index.html` is the source of truth — edit it, open it in a browser to check,
then regenerate the header the firmware serves:

```
python3 gen_tv_html.py     # writes ../arduino/canary_display/tv_html.h
```

`tv_html.h` is generated; don't hand-edit it. The Canary serves it at `GET
/tv` from `glass_web.cpp`, exactly as it serves the phone mirror at `/`.
