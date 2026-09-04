# The Witness Wall autopipeline — self-heal, self-publish, self-update, no rot

The founding ask, quoted verbatim in `docs/design/raspberry_pi_hub_flashing.md`:

> "…make sure it never rots, works for years, and can self-heal and update."

The Mac apps and the Canary firmware already answer that ask three different
ways. This is the same answer, shaped for Apple TV. The governing rule is the
one from `docs/RELEASE_PROCESS.md`: **git tags are the only control surface —
CI does everything else. If you did something by hand, that's the bug.**

## The full mapping

| The ask | Mechanism on Apple TV | Inherited from |
|---|---|---|
| **Self-publish** | Push `tvos-v1.4.0`. CI builds the SwiftUI app, links the shared Rust witness core, signs with the App Store Connect API key, and uploads to TestFlight / App Store Connect. No human touches an artifact. | `desktop-release.yml` (`app-v*`), `firmware-release.yml` (`fw-v*`) — tag in, signed release out. |
| **One app, two editions** | Home (the Witness Wall) and Business (the Witness Board) are the *same binary*: the room profile (Home / Business / Apartment) is a viewer-chosen setting, cycled from the header chips or the settings panel and persisted on the TV (`WallView.swift`, `SecuraCVWallProfile`). A profile changes layout and emphasis, never the data. Two experiences, **one** release surface: no second app to tag, sign, review, or let rot. (Choosing the edition from a kernel-reported venue profile is a design idea, not built — the kernel reports no such profile today.) | The Lab's "one frontend, N platforms" capability layer. |
| **Self-update** | **Inherit, don't rebuild.** Apple's phased App Store release rolls the new build to every Apple TV automatically; there is no bespoke updater to maintain. (The desktop app owns a Tauri updater because it ships outside a store; the TV doesn't need one.) | The hub's "inherit HAOS/RAUC, don't own an image that rots." |
| **Self-heal (on device)** | Every poll cycle the app re-fetches the fleet and asks its source for a sealed log to run the Rust verification core over (`GET /api/sealed-log`); a served chain's verdict — pass or fail — is what the banner renders, and now that the repo-root kernel serves `/api/sealed-log` *and* `GET /api/fleet` for the Wall to discover it through (the remaining gap is the TV's token-and-pairing story: the sealed-log route is token-gated, the TV holds no token yet, and "Verified" additionally waits on a key pinned at pairing) the integrity words are phrased as the fleet's own report, never as a verification the TV performed. Drift is shown **loudly and calmly**, never silently rendered as fine: a vanished hub marks the wall stale in its loudest element rather than letting old data read as now — the same posture as firmware's key-drift hard-fail and its "the update did not pass its health check, so the previous software was restored" message. Discovery + backoff reconnect heal network blips; a Doctor card turns any problem into one action. | Firmware A/B boot self-test + `PENDING_VERIFY` rollback; browser flasher "never get stuck." |
| **Self-heal (pipeline)** | The *upload* **no-ops cleanly** until `ENABLE_TVOS_BUILD=true` and signing secrets exist — a curious "Run workflow" click or an early `tvos-v*` tag succeeds with a helpful message instead of hard-failing. The **build** is never gated: `tvos.yml` compiles the app and runs its unit tests on the tvOS Simulator on every PR, with no signing and no account, so the code is verified long before anyone can publish it. | `desktop-mobile-release.yml`'s `ENABLE_IOS_BUILD` gate; the flasher's ephemeral-key fallback. Gate the credential, not the compiler (`.github/RELEASE_LESSONS.md`, Principle 6). |
| **Prove it before you publish** | `publish: false` builds, signs, and exports a real `.ipa` without uploading it — the smoke test. A publish additionally runs `altool --validate-app` first, then retries the upload with backoff, so a transient 5xx can't fail a release that would otherwise have landed. | `desktop-flasher-release.yml`'s `dry_run`; the 2026-07-24 upload-5xx entry in `RELEASE_LESSONS.md`. |
| **The tag can't lie** | `release-tvos.sh` reads `MARKETING_VERSION` out of `WitnessWall/project.yml` and refuses to build if a `tvos-v*` tag claims a different version. | The firmware release's version-string guard. |
| **No rot (CI can't forget)** | `tvos-release.yml` obeys the machine-enforced CI ground rules (`.github/CI.md` R1–R7): explicit `permissions`, `timeout-minutes`, a `concurrency` group that **never cancels a tag build mid-upload**, and SHA/tag-pinned actions. A workflow that skips a rule fails its own PR. | `workflows-lint.yml` + `ci_policy_check.py`. |
| **No rot (app can't drift)** | What is enforced today, on every PR: the SwiftUI app builds and its unit tests run on the tvOS Simulator (`tvos.yml`) — including the honesty rules (stale never renders as fine, "Verified" only from this TV's own verdict) and the FFI wrapper against the real linked core; the wire contract's documented example is pinned in both the Rust and Swift tests; `scripts/lint_apple_parity.py` fails the build when a shared iPhone device-vocabulary file isn't compiled by the Wall; and `tvos/scripts/check_app_icon.py` gates the icon catalog. **Not built yet:** golden-screenshot tests and a zero-commit nightly re-run that auto-files an issue (the website's `nightly-check.yml` is the model when someone wants it). | `lint_apple_parity.py`; website `nightly-check.yml` "anti-rot suite" (the model for the missing piece). |
| **No new attack surface** | The core is an independent implementation of the kernel's **pinned bytes**, proven byte-for-byte against the kernel's own domain-separation fixtures in CI on every PR and on the release path (`tvos/witness-core/tests/vectors.rs`) — a divergence fails the build rather than shipping a TV that disagrees with the source of truth. The app is read-only, holds no authoritative state, and never touches raw media (`spec/invariants.md`, Invariant I). | The fixture-pinning precedent set by the offline JS verifier (`viewer/verify_core.js`). |

## The one-button release, end to end

1. Bump the version, merge to `main`. The Canary Reviewer leaves its one
   advisory comment; deterministic gates decide correctness (`claude-review.yml`).
2. `git tag tvos-v1.4.0 && git push origin tvos-v1.4.0`.
3. CI (`tvos-release.yml`) builds the witness core for `aarch64-apple-tvos`,
   builds + signs the app, and uploads to App Store Connect.
4. Apple's phased release carries the build to every Apple TV. Nobody
   sideloads anything.

The only human action in the whole loop is choosing the version number. That is
the bar the rest of SecuraCV already clears — this brings Apple TV up to it.

## What still needs a human, once

The same one-time setup every Apple platform needs, and no more: an Apple
Developer account, the App Store Connect API key, and flipping
`ENABLE_TVOS_BUILD=true`. The checklist is in [`../../tvos/README.md`](../../tvos/README.md).

Until then, only the **upload** is waiting. The app is built and unit-tested on
the tvOS Simulator on every PR (`tvos.yml`), and the verification core is
checked against the kernel's own domain-separation fixtures on every PR *and*
again on the release path — so what's blocked is Apple's permission to publish,
not our confidence in the build. That distinction is deliberate: a pipeline
gated so thoroughly that it verifies nothing is the failure mode recorded in
`.github/RELEASE_LESSONS.md` (2026-07-24, "wired to an app that did not exist").

---

**Trademarks.** Apple, Apple TV, tvOS, and Xcode are trademarks of Apple Inc.,
registered in the U.S. and other countries and regions; App Store and App Store
Connect are service marks of Apple Inc. SecuraCV is an independent project by
Errer Labs, not affiliated with or endorsed by Apple Inc.; references are
nominative only.
