# Home Assistant brand images

The SecuraCV integration's icon and logo live **inside the integration**, at
[`custom_components/securacv/brand/`](../../custom_components/securacv/brand/)
— not in this folder, and not (yet) in the
[`home-assistant/brands`](https://github.com/home-assistant/brands) repository.

| File | Pixels | The brands-repository rule it is measured against |
|---|---|---|
| `icon.png` | 256 × 256 | square, exactly 256 × 256 — meets it |
| `icon@2x.png` | 512 × 512 | square, exactly 512 × 512 — meets it |
| `logo.png` | 255 × 258 | shortest side 128–256 px, landscape preferred — within the size rule, not landscape |
| `logo@2x.png` | 511 × 517 | shortest side 256–512 px — within the size rule |

All four are RGBA PNGs. The logo is the same drawing as the icon in a
slightly different crop; the brands README says that when logo and icon are
one image, submit only the icon files and let the icon stand in for the logo.
Home Assistant's own fallback table does the same (`logo.png` → `icon.png`),
so the two logo files could be dropped without a visible change — a brand
decision left to the maintainer, not made here.

## Where the icon comes from

- **Home Assistant 2026.3 and newer** reads a custom integration's `brand/`
  folder itself and serves it at `/api/brands/integration/securacv/<image>`
  (`icon.png`, `icon@2x.png`, `logo.png`, `logo@2x.png` and their `dark_`
  variants; core PR home-assistant/core#163960, announced in the developer
  blog on 2026-02-24 as "Custom integrations can now ship their own brand
  images"). A local file takes priority over the brands CDN. Nothing is needed
  in `manifest.json` — the loader looks for a `brand` entry in the
  integration directory.
- **HACS's `brands` validation** (the `hacs/action` check both repositories
  run) looks for `brand/icon.png` in the repository tree before it consults
  `brands.home-assistant.io/domains.json`. That is why the HACS mirror has
  carried the folder since 2026-08, and why the monorepo's `validate.yml` no
  longer ignores the check.
- **Home Assistant older than 2026.3** does not read the folder; the
  integration shows the generic placeholder there. The HACS dashboard itself
  still fetched icons from its own data feed when this was written
  (hacs/integration#5171, open), so it may show the placeholder too.

## Status of the brands-repository submission: not submitted

Until 2026-09-08 this folder (then `brands/submission/`) staged the same four
PNGs for a pull request adding `custom_integrations/securacv/` to
`home-assistant/brands`. That pull request was never opened, and the brands
README now says custom components can include their brand icons directly, so
the staged copies were moved into the integration (same bytes) instead of
being kept twice. A submission is still possible and is the only route to an
icon on Home Assistant older than 2026.3; it is a maintainer action:

```sh
gh repo fork home-assistant/brands --clone && cd brands
git checkout -b add-securacv-brand
mkdir -p custom_integrations/securacv
cp /path/to/securaCV/custom_components/securacv/brand/icon*.png custom_integrations/securacv/
git add custom_integrations/securacv && git commit -m "Add SecuraCV custom integration brand"
```

Two things to get right in that PR: copy only the icon files unless a real
landscape logo exists by then (see the table), and do not describe SecuraCV
as being in the HACS default store — it is installed as a custom repository.
