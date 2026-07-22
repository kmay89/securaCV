# `build-line.json` — the Lab's single source of truth

One file describes the whole Lab as a six-stage build line. **Every surface reads
it** so they can never drift apart:

- the isometric **room** (`lab-3d`) — one station per stage
- the stage **rail** + the **all-pages index** on the Lab landing
- the **Mac app**'s toolbar tab bar (`app.nav: "stage-tabs"` → one tab per stage)
- the marketing site's **`/lab`** page (the linear "stops")

If it's not in here, it doesn't appear on the line. That's the point: nothing gets
orphaned, and the six surfaces stay in lockstep.

## Shape

```
{
  "stages": [ { id, n, name, verb, accent, icon, benches:[…], site:[…], fork?, tracks? } … ],
  "onramp":  { the "Get started" guided path },
  "siteNav": [ the tightened top nav ],
  "footerNav": [ … ],
  "redirects": { "old-slug": "new-slug", … }
}
```

### A bench

```jsonc
{
  "noun": "Boards",            // what people SEE (product noun, not the file name)
  "slug": "boards",            // stable id / URL
  "lab":  "boards.html",       // the canary-local page it opens
  "real": false,               // true = boots the actual firmware (WASM) or real hardware
  "kind": "viewer",            // firmware | hardware | sim | configurator | chooser | setup | crypto
  "status": "rename",          // keep | rename | fold | promote  (matches the page ledger)
  "redirectFrom": "board-room",// old slug → auto-redirect (only when renamed)
  "wasNamed": "The Board Room",// for the changelog / redirect note
  "desc": "Spin every real board in 3D; ‘Wire it’ stages the harness.",
  "depths": [ … ]              // optional basic/advanced tabs (see below)
}
```

### Stage 4 forks

Stage 4 (`Sense`) sets `"fork": true` and uses `tracks` instead of `benches` —
one track per sensor (camera vs radar). Both tracks rejoin at stage 5 (Home).

### Depths = one node, two tabs

Where two pages were near-duplicates, they become **one bench with `depths`** (a
basic↔advanced or understand↔do toggle) instead of two competing entries:

- `sense.html` + `senselab.html` → **The Sense** (Basic ↔ Advanced)
- `house.html` + `scenes.html` → **The Canary House** (+ "Watch it work")
- `vault.html` + `operator.html` → **The Vault** (Understand it ↔ Set up break-glass)

## HTML documentation inventory

The manifest also carries `htmlDocumentation.groups`: standalone HTML docs,
runbooks, fixtures, and operator pages that are **not** primary Lab benches. The
visible [`site-map.html`](site-map.html) reads that list next to the six-stage
build line so iPad/native/web navigation, docs handoffs, and HTML-only support
pages stay discoverable without turning every support fixture into a stage.

When adding any new committed `.html` page:

1. If it is part of the user's build journey, add it as a `bench` or `depths`
   item under the right stage.
2. If it is supporting documentation, a native/mobile runbook, an evidence page,
   or a test/dev harness, add it under `htmlDocumentation.groups`.
3. If it replaces an old page or slug, add the old slug to `redirects`.

This keeps the sitemap expandable with one manifest edit and preserves the
"same pixels, native frame" iPad promise: the shell renders by width from the
same source of truth instead of maintaining a separate tablet map.

## Add a bench in 3 steps

1. **Add one entry** to the right stage's `benches` (or a `depths` item) here.
2. **Drop the page** — add the HTML, or let `tools/gen_*.py` generate its
   `devices/*.json` from firmware, exactly as today.
3. **Done.** It appears in the room, the rail, the index, the Mac app tab, and
   `/lab` — correctly named and placed. If you renamed a slug, add it to
   `redirects` so the old URL still resolves.

## Naming rule

`noun` is what a first-time visitor already pictures; the file name is plumbing.
Where they diverged (`homeassistant.html` = "The Hub", `wap.html` = "First boot"),
the manifest carries the real noun and a redirect — see the vocabulary audit.
