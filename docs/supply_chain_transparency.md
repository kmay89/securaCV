# Supply-chain transparency — provenance you can verify

Every firmware binary and browser-flasher factory image SecuraCV publishes now
carries **signed build provenance**: a public, tamper-evident record of *which
commit* was built, *by which workflow*, on *whose infrastructure*, into *exactly
these bytes*. You don't have to trust our word that a download is the real thing
built from the public source — you can check it, and so can anyone else.

This is the "how do I know the binary matches the source?" layer. It sits
alongside the two integrity checks that were already here:

| Layer | What it proves | Who checks it | Where |
|---|---|---|---|
| **Ed25519 OTA signature** | This firmware was released by the holder of the pinned release key | **the device**, before installing | [firmware_ota.md](firmware_ota.md) |
| **SHA-256 (`sha256sums.txt`)** | The bytes you downloaded are the bytes we published | you, in one command | on every release |
| **Build provenance** *(new)* | These bytes were built from commit `X` by our release workflow — logged publicly | **you / any third party**, before trusting a binary | this doc |

The first two answer *"is this authentic and intact?"*. Provenance answers the
question underneath it — *"was it actually built from the open source, in the
open, or did someone slip a binary in?"* — and answers it for **anyone**, not
just the devices holding the pinned key.

## What we publish

On every `fw-v*` release, [`firmware-release.yml`](../.github/workflows/firmware-release.yml)
signs a [SLSA](https://slsa.dev) build-provenance attestation over every artifact
it publishes — the firmware `.bin`s, the browser-flasher **factory images** it
builds in the same run, the manifests, and `sha256sums.txt` — using
[`actions/attest-build-provenance`](https://github.com/actions/attest-build-provenance).
The attestation is:

- **Signed with a short-lived key** tied to the workflow's GitHub OIDC identity
  (no long-lived signing secret to leak) via [Sigstore](https://www.sigstore.dev/).
- **Recorded in the [Rekor](https://docs.sigstore.dev/logs/overview/) public
  transparency log** — an append-only, independently-auditable ledger. We can't
  quietly attest one thing for you and another for someone else; every
  attestation is public and permanent.
- **Stored in this repo's attestation store** *and* attached to the release as a
  `provenance-*.sigstore.jsonl` bundle, so it's verifiable **online or fully
  offline**.

The predicate records the source repository, the exact commit SHA, the workflow
file and run, and the builder — the full "chain of custody" from tag to binary.

## Verifying a download

You need the [GitHub CLI](https://cli.github.com/) (`gh`, ≥ 2.49).

**Online** — checks against our attestation store and Rekor:

```console
$ gh attestation verify canary-2.3.0.bin --repo kmay89/securaCV
Loaded digest sha256:… for file://canary-2.3.0.bin
✓ Verification succeeded!
  - Source: kmay89/securaCV @ <commit>
  - Built by: .github/workflows/firmware-release.yml
```

**Offline / air-gapped** — the release's `provenance-firmware-<ver>.sigstore.jsonl`
bundle carries the signed statement, but verifying it with *no* network also
needs Sigstore's **trusted root**: the Fulcio / Rekor / timestamp anchors the
bundle is checked *against*. `gh` caches that root after any online use, so a
machine that has verified anything before already has it; for one that has
**never** been online, export the root once on a connected machine and carry it
in with the bundle:

```console
# once, on a networked machine:
$ gh attestation trusted-root > trusted_root.jsonl

# in the air-gapped room — the .bin, the bundle, and trusted_root.jsonl, no network:
$ gh attestation verify canary-2.3.0.bin \
    --bundle provenance-firmware-2.3.0.sigstore.jsonl \
    --custom-trusted-root trusted_root.jsonl \
    --repo kmay89/securaCV
```

That is the same fully-offline posture as the flasher's **Advanced → flash a
local file** and the OTA engine's offline verify. A mismatch — a binary that
wasn't built by our workflow, or was altered after the build — fails loudly.
That's the point.

## Reproducible builds — where we actually are

Provenance tells you *who built it and from what*. The complementary question is
*"if I rebuild the source myself, do I get the identical bytes?"* — reproducible
builds. Here we choose honesty over a checkbox:

- **Full bit-for-bit reproducibility is not guaranteed today.** The ESP32
  toolchains we build with (PlatformIO's `espressif32` platform and, for the WAP
  variant, `arduino-cli`) embed build timestamps and absolute paths and are not
  fully hermetic, so two builds of the same commit can differ in a handful of
  non-functional bytes. Claiming otherwise would be the kind of overstatement
  this project exists to avoid.
- **What is pinned** is captured *in* the provenance: the platform/toolchain
  versions (`firmware/*/platformio.ini`), the exact commit, and the workflow
  environment. So even without byte-identical rebuilds, the *inputs* are
  recorded and auditable.
- **Why provenance is the stronger guarantee here.** Reproducible builds let you
  trust a binary by rebuilding it yourself; provenance lets you trust it by
  verifying a signed, publicly-logged statement of how it was built. For an
  embedded toolchain that resists hermeticity, the second is both achievable
  *today* and harder to quietly defeat (you'd have to forge an entry in a public
  append-only log).

**Rebuild-and-compare** (best-effort, for the curious): check out the release
tag, build with the toolchain versions the provenance names, and diff. Expect
timestamp/path differences; the meaningful checks are that the **functional
content matches** and that the **provenance names the same source commit** you
built from.

```console
$ git checkout fw-v2.3.0
$ cd firmware/canary && pio run -e release_ha      # see firmware-release.yml for exact steps
$ sha256sum .pio/build/release_ha/firmware.bin     # compare bytes; expect minor toolchain noise
```

Reducing that noise toward true reproducibility (`SOURCE_DATE_EPOCH`, hermetic
toolchains) is on the roadmap; this doc will get more confident, not more vague,
as it lands.

## Scope

Attested today: everything the **signed firmware release**
([`firmware-release.yml`](../.github/workflows/firmware-release.yml)) publishes —
the firmware binaries **and** the browser-flasher factory images it builds in the
same run — with provenance whose source is exactly the release tag.

The out-of-band [`flasher-release.yml`](../.github/workflows/flasher-release.yml)
(a manual rebuild that compiles *tagged* firmware with *today's* packaging
tooling) deliberately does **not** attest. That build has two different sources —
the firmware's tag and the tooling's commit — and a single provenance statement,
whose source ref comes from the workflow run itself, can't honestly name both; a
statement that quietly recorded the dispatch commit as "the source" of
tag-built firmware would be exactly the kind of misleading claim this doc exists
to avoid. So that path keeps the SHA-256 + same-origin trust it always had
([browser_flasher.md § Trust model](browser_flasher.md)); the factory images it
serves are the ones the normal release already attests.

The **desktop flasher** ([`desktop-flasher-release.yml`](../.github/workflows/desktop-flasher-release.yml))
and the **Vision AI model** ([`vision-model-release.yml`](../.github/workflows/vision-model-release.yml))
publish executables/artifacts too and are the next to get the firmware release's
treatment — tracked as a follow-up so this claim stays honest about what's
covered *now*.

## See also

- [Firmware OTA](firmware_ota.md) — the Ed25519 release signature devices verify
- [Browser flasher § Trust model](browser_flasher.md) — SHA-256 + in-browser Ed25519 + the flasher's own CSP/SRI
- [Release process & channels](RELEASE_PROCESS.md) — how tags become releases
- [Security docs](security/README.md) — the threat model and the Ten Principles
