# Release Process — dev and release channels

The single source of truth for shipping firmware. One mechanism, two
channels, gated **structurally** (at the manifest layer), not by client-side
filtering. Companion to `docs/firmware_ota.md` (engine + signing details) and
`docs/design/usb_evidence_drive.md` (drop-file updates ride the same
artifacts).

## The model in one paragraph

Git tags are the only control surface. A stable tag (`fw-vX.Y.Z`) publishes a
normal GitHub Release: `releases/latest` moves, and every default device —
which polls `releases/latest/download/manifest-<product>.json` — sees the
update on its next daily check. A dev tag (`fw-vX.Y.Z-dev.N` or `-rc.N`)
publishes a GitHub **prerelease**: `releases/latest` does not move (GitHub
guarantees this), so release-channel devices and their Home Assistant update
entities *cannot* learn it exists — there is nothing to hide because nothing
arrives. The same CI run mirrors the prerelease's **manifests** to a rolling
`fw-dev-latest` release, which is the dev channel's one stable address.

Dev builds are **full production builds** — same features, same signing key,
same workflow. The only difference is the version suffix. You test exactly
what users will run.

## Version + tag grammar

| Intent | Source version (e.g. WAP) | Tag | GitHub | Who sees it |
|---|---|---|---|---|
| Stable release | `2.3.0-wap` | `fw-v2.3.0` | Release; `latest` moves | everyone |
| Release candidate | `2.3.0-wap.rc.1` | `fw-v2.3.0-rc.1` | Prerelease | dev channel only |
| Dev iteration | `2.3.1-wap.dev.2` | `fw-v2.3.1-dev.2` | Prerelease | dev channel only |

Rules:
- The numeric triple in the tag and in every variant's source version must
  match — CI's version-string guard greps each binary for its version and
  fails the release if the source wasn't bumped.
- Prerelease markers are the exact segments `dev.N` / `rc.N` (dot-number).
  Variant suffixes (`-wap`) are not prerelease markers. Ordering is enforced
  by `securacv_version_compare`: **stable > rc.N > dev.N** at an equal
  triple, numeric within a band — so promoting a build offers a real update
  to every dev-channel device, and anti-rollback floors stay coherent.

## First-time only — the OTA signing key ceremony

A release cannot be signed until an Ed25519 release key exists and its public
half is embedded in the firmware. If `ota_release_key.h` is all zeros (the
default), OTA is hard-disabled and `firmware-release.yml` fails fast with
`OTA_SIGNING_KEY_PEM secret is not set`. Do this once, on your own machine —
**never** in CI or any shared/cloud shell:

```sh
# From the repo root, on your own machine. Writes releaser.pem OUTSIDE the repo.
firmware/scripts/setup_release_key.sh --key ~/securacv-releaser.pem
```

That helper (idempotent, refuses to write the key inside the repo) generates
the private key, embeds the **public** header in the canonical location, and
syncs the two committed Arduino copies so `check_ota_sync.sh` stays green.
Then, exactly as it prints:

1. Add the private key as the `OTA_SIGNING_KEY_PEM` GitHub Actions secret
   (Settings → Secrets and variables → Actions → New repository secret) — the
   full PEM, `-----BEGIN PRIVATE KEY-----` through `-----END PRIVATE KEY-----`.
2. Commit **only** the public `ota_release_key.h` files (never `releaser.pem`;
   `*.pem` is already gitignored).
3. Cut a release (below) — CI signs with the secret.

The private key is the master signing key: keep it offline, back it up
privately, rotate via `ota_release_key_previous.h` (see `docs/firmware_ota.md`).

## Shipping — the whole ceremony

```sh
# 0) preflight (any release, either channel)
firmware/scripts/check_ota_sync.sh           # engine copies in sync
make -C firmware/projects/canary-wap/tests_host run   # host suites green
# bump FIRMWARE_VERSION / CANARY_FW_VERSION in every variant, update CHANGELOG

# 1) dev iteration
git tag fw-v2.3.1-dev.2 && git push origin fw-v2.3.1-dev.2

# 2) promote the SAME commit to stable once it has soaked
git tag fw-v2.3.1 <same-sha> && git push origin fw-v2.3.1
```

CI does everything else: builds all products (canary, wap, the three vision
host boards, the two sense flavors, and the three display flavors — watch /
dash / dash-modes), verifies the signing key matches the committed public
key, signs every image and manifest, runs `ota_release.py verify` over its
own output, greps binaries for the version string, generates the
browser-flasher factory images + `manifest-flash.json`, and publishes. There
are no manual artifact steps — if you did something by hand, that's the bug.

Promotion is a rebuild of the same commit from the same pinned workflow —
the honest guarantee is "same source, same toolchain, re-verified
signatures". (Bit-identical artifact promotion is a possible future
hardening; don't claim it until it's implemented.)

### The same ceremony, one-click (no local `git tag`)

Everything above also runs from the **Actions tab** — same build, same
signing, same guards, so the two paths can't drift. The button just creates
the tag for you from the version you pick, at the current commit, so tag and
source can never disagree.

- **Actions → "Firmware Release" → Run workflow.** Pick `channel`
  (release / dev) and enter `version`:
  - `release`: a clean triple, e.g. `2.3.1` (blank reads the triple from
    source). Tags `fw-v2.3.1`, `latest` moves, everyone sees it.
  - `dev`: a `-dev.N` / `-rc.N` version, e.g. `2.3.1-dev.2`. Tags
    `fw-v2.3.1-dev.2` as a prerelease; only the dev channel sees it.
  - The channel is cross-checked against the version's grammar (a dev
    version with the release channel — or the reverse — is refused), and an
    already-published tag is refused (bump the headers first). **You still
    bump `FIRMWARE_VERSION` / `CANARY_FW_VERSION` in every variant** to match
    — the version-string guard greps each binary for it and fails closed
    otherwise. The one-click removes the `git tag` step, not the version bump.
- **Actions → "Release — one click (firmware + apps + web)".** The
  whole-pipeline launcher: set `firmware` to `dev` or `release` (with
  `firmware_version`) and it dispatches the firmware release above alongside
  the desktop apps and the site deploy. Leave `firmware: none` to ship only
  the apps/web.
- **Actions → "Update everything (only what needs it)".** The button to reach
  for when you just want the world to be current and don't want to think about
  which targets moved. It reads `.github/release-targets.yml`, compares every
  target's **source version** with what has actually been tagged, and
  dispatches **only** the ones that are genuinely ahead:
  - ahead of its newest tag → released (which is what cuts the tag);
  - never released → the first one is cut;
  - same version but the watched files changed → it says **"bump the version
    first"** and ships everything else, because a release must never carry a
    version that is already published;
  - same version, nothing changed → skipped, so pressing it twice costs
    nothing;
  - the site is redeployed only if the pages it publishes changed since the
    last successful deploy;
  - an Apple target whose `ENABLE_*_BUILD` variable is off is left alone
    rather than spending a macOS runner on a workflow that would no-op.

  Leave `publish` unchecked for build-only smoke runs, tick it for real
  releases, or tick `plan_only` to see the table without dispatching anything.
  It goes red only if a dispatch that was supposed to happen failed — "needs a
  bump" and "already up to date" are answers, not failures. The decision logic
  is `.github/scripts/release_plan.py`, unit-tested in CI along with the
  catalog itself (every workflow it names must exist, every version file it
  points at must be readable).
- **Actions → "Flasher Factory Images"** rebuilds *only* the browser-flasher
  factory images + `manifest-flash.json` for an existing tag (e.g. after a
  packaging-tooling fix) without cutting a new version. Blank tag = the
  current `releases/latest`.

Which products appear (flashable) in the in-browser flasher is decided by
`manifest-flash.json` in the release the page reads — the stable channel's
`releases/latest`, or the rolling `fw-dev-latest` with `?channel=dev`. A
product shows as "unavailable" until a release carries it, so a newly-added
board reaches the flasher the moment its first release is cut.

## Opting a device into the dev channel

The engine already has the mechanism: an NVS-persisted manifest-URL override
(`securacv_ota_set_manifest_url`, surfaced via the device's OTA settings
API). Point it at:

```
https://github.com/kmay89/securaCV/releases/download/fw-dev-latest/manifest-<product>.json
```

Clearing the override returns the device to the release channel. Because the
override lives in the device's own NVS, the choice is local, per-device, and
invisible to every other device. HA update entities announce only what the
device's channel manifest offers.

The Lab flasher's dev toggle (`flash.html?channel=dev`) reads the same
`fw-dev-latest` flash manifest, with a visible banner. The default page is
release-only.

## Rollback (when a stable release goes wrong)

1. Do **not** delete the release or its artifacts — devices mid-download and
   audit trails depend on published assets staying put.
2. Tag the previous good commit as a new, higher version
   (`fw-v2.3.2` = re-issue of 2.3.1's content with a bumped version) — the
   anti-rollback floor on devices refuses versions that go backwards, so
   roll *forward* to old content rather than backwards in version.
3. For the dev channel nothing special is needed: the next dev tag re-points
   `fw-dev-latest`.

## Invariants CI enforces (don't fight them)

- Signing key in CI must match the committed `ota_release_key.h` (rotation
  window via `ota_release_key_previous.h`).
- Every manifest signature is re-verified after generation.
- Binary version strings must match the tag's version.
- `canary-local` catalog drift gates (`gen_flash.py` etc.) keep the flasher
  honest against the firmware tree.
- Prerelease tags can never move `releases/latest` — that's GitHub's
  guarantee, and it is the entire channel-privacy mechanism.
