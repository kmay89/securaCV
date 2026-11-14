---
paths:
  - "**/*.scad"
  - "devices/*/device.json"
  - "canary-local/devices/*.json"
  - "canary-local/tools/gen_*.py"
  - "canary-local/tools/**/gen_*.mjs"
  - "docs/hardware/enclosure/gen_*.py"
  - "firmware/projects/canary-display/**"
  - "firmware/projects/canary-vision/**"
  - "firmware/common/**"
  - "firmware/flavors.json"
  - "firmware/build_matrix.json"
  - "firmware/**/platformio.ini"
  - "firmware/**/library.json"
  - "firmware/**/library.properties"
  - "firmware/**/version.h"
  - "firmware/projects/canary-wap/arduino/canary_wap/web_ui.h"
  - "firmware/projects/canary-wap/arduino/canary_wap/csi_dashboard_html.h"
  - "firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h"
  - "spec/witness_dictionary.json"
  - "spec/device_details_vocabulary.json"
  - "AGENTS.md"
  - ".github/release-targets.yml"
---
# You touched an input of a generated, committed file

CI regenerates forty-odd outputs and byte-diffs them, and the failure is
often nowhere near the edit (a one-word fix in a hub tool once broke a sha256
pin checked by a Rust test three layers away). Regenerate in the same commit.
The account, with the stories: [`docs/GENERATED_FILES.md`](../../docs/GENERATED_FILES.md).

| You edited | Run |
|---|---|
| `AGENTS.md` (the brief block) | `python3 scripts/gen_agent_entrypoints.py` — six vendor files |
| `spec/witness_dictionary.json` | every copy `python3 scripts/lint_dictionary_sync.py` names; a `homekit_projection` change also `python3 scripts/gen_apple_home_docs.py` (the Apple Home signal table is a privacy promise) |
| `spec/device_details_vocabulary.json` | `python3 scripts/gen_device_vocabulary.py` — `custom_components/securacv/device_vocabulary.py`, read by the diagnostics dump's `TO_REDACT`; its `--check` also fails a firmware edit that drops a key a row names, and the old key moves into `diagnostics.py`'s `INTEGRATION_REDACT_KEYS` before the row does (the generator will not drop a redacted key) |
| a `.scad`, or `cad.params` in `devices/<slug>/device.json` | `python3 scripts/regen_cad.py --previews <dir>` — the whole chain in order; previews go to the requester |
| `canary-local/devices/*.json` (a catalog) | the `canary-local/tools/gen_*.py` that reads it; `python3 scripts/lint_device_manifests.py` proves the joins |
| a WAP portal page (`web_ui.h`, `csi_dashboard_html.h`, `companion_pwa.h`) | `gen_web_assets_gz.py` beside it, then `gen_wap.py`, **then** `gen_csp.py` |
| `PORTAL_HTML` in `canary-display/src/net/provision.cpp` | `gen_display_portal.py`, **then** `gen_csp.py`, then the dist rebuild |
| anything `canary-local/emulator/build.sh` compiles (display `src/`, the Vision core, `firmware/common/`, a `version.h`) | the dist rebuild, Actions → "Rebuild emulator dist (pinned emsdk)" on your branch, **before** any `gen_*.py` catalog (`canary-local/AGENTS.md`) |
| `firmware/flavors.json`, a `platformio.ini`, a library manifest, a `version.h` | `python3 scripts/gen_firmware_sbom.py`; `python3 firmware/scripts/flavor_envs.py --check-workflows` |
| anything `gen_hub_provision_bundle.py` embeds (it pins by sha256; a Rust test in `desktop/hub-io` cross-checks) | `python3 canary-local/tools/gen_hub_provision_bundle.py` |
| `.github/release-targets.yml` | `python3 .github/scripts/test_release_plan.py` |

When more than one applies, the order is: edit → `firmware/projects/canary-display/setup.sh regen`
(only when the fleet figures moved) → dist rebuild → pull → the `gen_*.py`
catalogs → commit. The catalogs read `fw_version` from the rebuilt
`dist/*.meta.json`; run before it, they bake in the old stamp.

Derive the list of generators and gates; never type it:

```sh
grep -rhoE "python3 [a-zA-Z0-9_/.-]*gen_[a-zA-Z0-9_]+\.py|node [a-zA-Z0-9_/.-]*(gen_[a-zA-Z0-9_]+|make-[a-z-]+)\.mjs" .github/workflows/*.yml | sort -u
grep -rhoE "[a-zA-Z0-9_/.-]+\.(py|mjs) [^|;&]*--check" .github/workflows/*.yml | sort -u
```
