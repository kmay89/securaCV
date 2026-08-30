# The help ecosystem layout — one map, every surface

**Status:** shipped on the website (see `securacv_website/docs/LAYOUT.md`);
this doc is the cross-surface standard it implements.

SecuraCV's guidance lives on many surfaces: the website, the Lab
(`canary-local/`), the on-device displays, the desktop apps (`desktop/`,
`desktop-lab/`), and the Home Assistant integration. This standard keeps
them one designed family — explorable by an imaginative ten-year-old,
verifiable by a security-grade maker, and structurally unable to rot.

## The three named workshops

The public story is told through three places, in order:

| Place | Question | Canonical surface |
|---|---|---|
| **The Lab** | "What is a Canary?" | `canary-local/` + `securacv.com/lab` — explore every device; the emulator is the shipping firmware compiled to WASM |
| **The Test bench** | "Can I touch it?" | the on-device peripheral bench (`firmware/projects/canary-display/src/playground/`, [dev playground doc](../hardware/dev_playground_43b.md)) + `securacv.com/playground` |
| **The Factory** | "How is one born?" | `securacv.com/factory` — the factory image (`firmware/scripts/make_factory.py`), first-flash-seeds-NVS, and the SLSA/Rekor birth certificate |

Explore → touch → make. Every surface that mentions one of these should
link it by this name and this order.

(The Test bench was named "the Playground" through mid-2026. The Lab
manifest — `canary-local/build-line.json`, the machine-readable map rule 1
makes authoritative — carries the rename as `wasNamed` and redirects the
old slug, and the public URL keeps its historic `/playground` path. Code
identifiers and filenames — `playground.json`, `FEATURE_PLAYGROUND`, the
firmware `playground/` sources, `gen_playground.py` — deliberately keep
the old name: renaming them buys no clarity and breaks generators.)

## The rules

1. **One map per surface, tested.** A surface's structure lives in one
   machine-readable place, and CI fails if the map and the surface disagree.
   The website's map is `js/site.js` + `tests/site-map.test.mjs`; this
   repo's is `docs/README.md` + `scripts/lint_docs_index.py`. Same idea,
   already enforced on both.
2. **Every page links every page.** The website renders a footer atlas
   (all pages, grouped by journey) on every page — orphans are impossible
   by construction, not by diligence.
3. **Two voices, one page.** Plain warm copy for the ten-year-old; a
   `<details>` "For makers:" fold with exact offsets, paths, and protocol
   names for the maker. Never dumb down the fold; never jargon the body.
4. **Honest by construction.** Every guide page ends by naming the firmware
   files its facts are read from. A fact without a source doesn't ship, and
   a `*-facts` test pins it against ground truth.
5. **Write once, wrap thin.** The web pages are canonical; native apps are
   thin wrappers (Tauri OS WebViews) over the same content. Web is the
   source; Android gets it as a PWA (manifest + shortcuts); macOS/Linux
   (and Windows, on the roadmap) get it via `desktop/`/`desktop-lab/`.
   iOS is the deliberate exception: the companion app (`ios/`) is a
   native SwiftUI witness console, not wrapped web — built and CI-tested,
   App Store availability pending (`canary-local/devices/ecosystem.json`
   keeps the honest status). One help system, not five — and one named
   native exception.
6. **Quiet Glass everywhere.** Visual tokens descend from the display
   firmware's `theme.h` → the Lab's `canary-local/assets/canary-local.css` →
   the desktop shells. The website keeps its own light token set today;
   any convergence should point back at `theme.h` as the root.

## Why this shape

The [UX/UI audit](../ux_ui_audit_2026-06.md) found the surfaces individually
strong but divergently navigated; the
[Lab & Flasher experience RFC](flasher_experience.md) defined the single
bring-up-and-tend arc. This standard is the layout-level companion to both:
the arc gives the story, this gives the story a stable, self-checking home
on every platform.
