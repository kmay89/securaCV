# Maintainers

SecuraCV is small and honest about it: today most subsystems route through
the founding maintainer (Errer Labs / @kmay89). That is the bottleneck we are
actively trying to remove — **several seats below are open, and this file is
partly a job posting.** If you've done good work in an area, ask to own it.

The GitHub team `@securaCV/maintainers` is what [`.github/CODEOWNERS`](.github/CODEOWNERS)
resolves to for constitutional paths (`spec/`, `kernel/`, `src/`). Being
listed here for a subsystem means your review is expected on changes to it.

## Subsystem map

| Area | Paths | Maintainer(s) | Status |
| --- | --- | --- | --- |
| **Witness kernel** (Rust) | `src/`, `kernel/`, `spec/` | @kmay89 | constitutional — maintainer review required |
| **Firmware** (ESP32 / C++) | `firmware/` | @kmay89 | **seat open** |
| **Board ports** | `firmware/boards/`, `boards/` | @kmay89 | low-friction; see [`firmware/PORTING.md`](firmware/PORTING.md) |
| **Home Assistant integration** (Python) | `custom_components/securacv/`, `integrations/` | @kmay89 | **seat open** |
| **The Lab** (WASM demos) | `canary-local/`, `canary-vision/` | @kmay89 | **seat open** |
| **Desktop / flasher** (Tauri) | `desktop/`, `desktop-lab/` | @kmay89 | **seat open** |
| **Enclosures** (OpenSCAD) | `docs/hardware/enclosure/` | @kmay89 | **seat open** |
| **Supply chain / BOM** | `docs/hardware/bom_*.csv`, `scripts/lint_bom.py` | @kmay89 | **seat open** |
| **Docs & guides** | `docs/` | @kmay89 | **seat open** |

## How maintainership works here

- **Owners are earned, not appointed.** The path is the same as the product's
  own trust model: demonstrated, reviewable work. Land a few solid PRs in an
  area, then open an issue asking to co-maintain it.
- **The kernel bar is constitutional.** Changes to `spec/`, `kernel/`, and
  `src/` preserve the invariants or they don't merge — see
  [`CONTRIBUTING.md`](CONTRIBUTING.md). That bar is not negotiable even for
  maintainers.
- **Everything else is meant to be welcoming.** Board ports, enclosures,
  guides, the Lab, and translations are where a first-time contributor should
  start. See the "Start here" section of [`CONTRIBUTING.md`](CONTRIBUTING.md).
- **Automation is a maintainer.** Much of the review bar is enforced by CI
  (feature-dashboard guard, BOM drift gate, fact-tests) and by the advisory
  [Canary Reviewer](.github/workflows/claude-review.yml). Humans decide;
  machines catch the mechanical stuff first.

## Contact

Security-sensitive reports go through [`SECURITY.md`](SECURITY.md), never a
public issue. Everything else: open a GitHub issue with the right template.
