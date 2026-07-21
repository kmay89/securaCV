# Cura profiles

Ready-to-import Ultimaker Cura profiles for the Canary enclosures. These are a
**convenience** — the reasoned, version-proof source of truth is
[`../printing_petg_cura.md`](../printing_petg_cura.md). If a profile and the
guide ever disagree, the guide wins.

| File | For | Notes |
|---|---|---|
| [`securacv_canary_petg_security_0.4n_0.2mm.curaprofile`](./securacv_canary_petg_security_0.4n_0.2mm.curaprofile) | PETG, 0.4 mm nozzle, 0.2 mm layers — the security-build spec (4 walls, 30 % gyroid) | Cura 5.x |

## Import

Cura → **Preferences → Profiles → Import** → pick the file → select it from the
profile dropdown (top-right).

Built against the generic `fdmprinter` base, so it imports onto **any** printer.

## After importing — do this

- **Set your Retraction Distance.** It's printer-specific (~1–2 mm direct-drive,
  ~4–6 mm bowden) and a shared profile can't get it right. Skip this and PETG
  will string.
- **Leave supports off and all three expansion settings at 0.** The models bake
  in tolerance and elephant-foot compensation; re-compensating breaks the
  calibrated fits. See the guide's
  [one idea](../printing_petg_cura.md#the-one-idea-the-model-already-did-the-hard-part).
- **Print the [fit coupon](../canary_fit_coupon.scad) first** to confirm fits on
  your printer.

## Not covered by a profile (on purpose)

TPU gaskets and ASA/PC outdoor parts have different enough cooling, temperature
and flow that a shared profile would mislead. Their settings are in the
[cheat-sheet](../printing_petg_cura.md#per-model-cheat-sheet) and the
[README material table](../README.md#engineering--materials-security-build).
