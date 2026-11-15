# canary-display — the brief for this tree

Loads when you touch a file here; the root `AGENTS.md` and
[`firmware/AGENTS.md`](../../AGENTS.md) still apply.

## This tree is also the emulator

`canary-local/emulator/build.sh` compiles these sources to WebAssembly, and
the output is committed as `canary-local/emulator/dist/*.js`, one per flavor.
An edit here can leave `dist/` stale and fail `canary-local.yml` on a file you
never opened. What moves it:

- `src/main.cpp`, `src/glass_settings.cpp`, `src/runtime_config.cpp`,
  `src/trust.cpp`, `src/net/mqtt_mgr.cpp`, `src/net/provision.cpp`,
  `src/care/*.cpp`, `src/fleet/*.cpp` and the LVGL faces are compiled in.
- `src/net/glass_web.cpp` and `src/net/discovery.cpp` are **not**; an edit
  there, or to a header only they include, leaves `dist/` alone.
- `firmware/common/color/look_engine.cpp`, `common/network/wifi_join_policy.h`,
  `common/network/provision_core.h` and the `WIFI_RETRY_*` /
  `WIFI_OUTAGE_REBOOT_MS` constants in `include/canary/config.h` are compiled
  in through the emulator's own `canary-local/emulator/src/emu_net.cpp`.
  Everything under `canary-local/emulator/src/` and `shim/` is dist by
  definition.
- `include/canary/version.h`: every bump moves every flavor's dist
  (`mqtt_mgr.cpp` embeds `CANARY_FW_VERSION`, and `build.sh` stamps it into
  each `dist/*.meta.json`). A release commit is always a dist commit.

Rebuilding needs emsdk 6.0.3 exactly: Actions → "Rebuild emulator dist
(pinned emsdk)" on your branch. The order after it, and the four ways its
push misleads, are in [`canary-local/AGENTS.md`](../../../canary-local/AGENTS.md).

## Chains in this tree

- `PORTAL_HTML` in `src/net/provision.cpp` (the first-boot portal) is framed
  by the Lab's `fleet.html`: edit it, run
  `python3 canary-local/tools/gen_display_portal.py`, **then** `gen_csp.py`,
  then the dist rebuild.
- `arduino/canary_display/` is the Arduino parity sketch, generated from this
  PlatformIO tree by `./setup.sh regen`, which also copies `gen_figures.mjs`'s
  `fleet_figures.h` / `fleet_figures_art.h` into the sketch (so it runs after
  the figures generator, never before). Never edit the sketch;
  `firmware/scripts/check_display_arduino_sync.sh` fails CI if it drifts.
- Display env lists are derived from `firmware/flavors.json`
  (`firmware/scripts/flavor_envs.py --check-workflows`), never typed into a workflow.

## Checks

`tests_host/` here; `firmware/scripts/microcopy_lint.sh` for the glass copy;
the Lab's page tests for anything the emulator shows
(`canary-local/AGENTS.md`).
