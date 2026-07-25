# C2PA Content Credentials for export bundles — design

Status: **Implemented (v1)** — shipped behind the `c2pa-export` cargo feature
(`src/c2pa_export.rs`, `export_events --c2pa`, `export_verify --c2pa-manifest`).
Intended Status: Informative (architecture decision record)
Last Updated: 2026-07-25

> One sentence: when you export a witness bundle, SecuraCV can now also sign
> an **industry-standard C2PA Content Credentials manifest** over the exact
> bundle bytes, so a court clerk, newsroom, or insurance adjuster can verify
> the artifact with *any* Content Credentials tool — without installing
> SecuraCV, and without SecuraCV giving up an inch of its own trust model.

## 1. Why

The evidence world is converging on C2PA. ONVIF and the C2PA have announced a
collaboration on signed surveillance video; Sony ships C2PA camcorders; the
Pixel 10 and Galaxy S25 sign every capture; US courts are actively debating
authentication standards for digital video (proposed FRE 707); the NY Stop
Deepfakes Act mandates C2PA-conformant provenance. Our strategy docs have
carried "C2PA / Content-Credentials interop" as a roadmap item for a while
(`docs/review/02-roadmap.md`, strategy §7.1) with the observation that the
export envelope + dual-verifier design is "already 80% of a C2PA-interoperable
story." This RFC is the remaining 20%.

What C2PA buys us is **verification without our software**. What it must NOT
do is replace the witness chain: a C2PA manifest signs an *asset*; it cannot
prove a *timeline* (that no events were deleted, that the record is
continuous, that a disclosure was receipted). The hash-chained, Ed25519-signed
log remains the root of trust (Principle 4: evidence verifiable without
ERRERlabs — or any third party). C2PA is the diplomatic passport; the chain
is the birth certificate.

## 2. Shape of the integration

### 2.1 Sidecar manifest, exact bytes

An export bundle is a JSON file, not a C2PA-supported media container, so the
manifest is a **sidecar** (`<bundle>.c2pa`, raw JUMBF): its `c2pa.hash.data`
hard-binding covers the **entire bundle file, no exclusions**. The bundle
bytes are identical with or without C2PA — old verifiers, scripts, and
receipts are unaffected, and the witness receipt's `artifact_hash` and the
C2PA data-hash commit to the same bytes.

### 2.2 Keys: derived, domain-separated, never stored

Both C2PA keys derive from the device key seed via HKDF-SHA256 with
versioned domain separation (`securacv:c2pa:signing:v1`,
`securacv:c2pa:ca:v1`), using the device identity key as input keying
material. Consequences:

- No new key material to provision, back up, or leak — same custody story as
  every other kernel key.
- The C2PA keys are cryptographically independent of the chain-signing key
  (domain separation), so cross-protocol signature confusion is impossible.
- Rotating the device seed rotates the C2PA credential with it.

### 2.3 A reproducible device-local credential chain

The C2PA certificate profile requires an X.509 chain; ours is generated
locally: a **device CA** (self-signed, CA=true, pathlen 0) issues a
**signing leaf** (CA=false, `digitalSignature`, `emailProtection` EKU, AKI —
exactly the profile shape the CAI validator enforces). The chain is
**byte-reproducible from the seed**: fixed validity window, serials derived
from the key, deterministic Ed25519 signatures. There is no cert store and
nothing to lose — `export_verify` can re-derive the trust anchor from the
seed at any time, and `export_events --c2pa-anchor-out` writes a convenience
PEM copy for handing to third parties.

### 2.4 Trust model: sovereign anchor, honest about the public list

We deliberately do **not** join a public CA hierarchy in v1:

- **Verifier holds the device CA (or the seed)** → validation reports
  `Trusted`. This is the local/court workflow: the owner hands over
  `device_ca.pem` alongside the export, or the verifier derives it.
- **Verifier uses only the public C2PA trust list** → the manifest validates
  as *well-formed and unmodified* but *signer not on the trust list*. That is
  the honest statement of what a self-sovereign device can claim, and it
  still proves integrity + key continuity across exports.
- A future paid attestation tier (strategy §7.1) can add a CA-issued
  credential *next to* the device chain without changing this design.

### 2.5 The `org.securacv.witness` assertion — two seals, cross-bound

Each manifest carries a custom assertion embedding the signed export-receipt
entry hash, the receipt's artifact hash, the device verifying key, the
authorization mode (`self_export` / `break_glass`), ruleset id, and kernel
version. `export_verify --c2pa-manifest` enforces the cross-binding: the
receipt entry named in the manifest must be one it just verified in the
tamper-evident log under the trusted device key, and the artifact hashes
must agree. A forged-but-internally-valid manifest therefore cannot be
grafted onto a different device's export, and a replayed manifest cannot
vouch for a bundle its receipt never covered.

### 2.6 Fully offline (Principle 2)

The `c2pa` crate is built with `default-features = false` +
`rust_native_crypto`: **no OpenSSL, no HTTP backend compiled in, no
timestamp authority contacted**. Signing and verifying are pure local
computation. (RFC-3161 timestamping of the *chain* remains the separate,
optional `tsa` feature.)

## 3. Usage

```sh
# Export with Content Credentials (feature: c2pa-export)
export_events --db-path witness.db --self-export \
  --output export.json --c2pa --c2pa-anchor-out device_ca.pem
# → export.json, export.json.c2pa, device_ca.pem

# Full verification: receipts + bundle + C2PA sidecar + cross-binding
export_verify --db witness.db --bundle export.json \
  --c2pa-manifest export.json.c2pa            # anchor re-derived from seed
export_verify --db witness.db --bundle export.json \
  --c2pa-manifest export.json.c2pa --c2pa-anchor device_ca.pem

# Third parties: any C2PA/Content Credentials verifier, given the sidecar +
# bundle (+ device_ca.pem for full trust).
```

## 4. What this deliberately does not do (v1)

- **No C2PA on the ESP32s.** Signing happens on the hub at export time; the
  Canaries' witness events already carry their own signatures. Capture-time
  C2PA on-device would add cert plumbing to firmware for no additional trust
  (the kernel is the disclosure boundary).
- **No embedded manifests in media.** When sealed-clip export ships
  (evidence pack productization), the same module signs embedded manifests
  for JPEG/MP4 — the sidecar path was chosen so that step is additive.
- **No ONVIF media-signing verification on ingest yet.** The ONVIF×C2PA
  work is announced but not deployed in prosumer cameras; when cameras that
  sign appear, verifying their signatures at ingest and recording the result
  as a witness event is the natural phase 2.
- **No public trust list membership.** See §2.4.

## 5. Security review notes

- **New dependencies**: `c2pa` (Adobe/CAI official SDK, MIT/Apache-2.0) and
  `rcgen` (rustls project, MIT/Apache-2.0) — both maintained, widely
  deployed, and compiled in only under the off-by-default `c2pa-export`
  feature. No default-build footprint change.
- **No new outbound connections**: no HTTP feature is compiled into `c2pa`;
  the signer's TSA parameter is `None`.
- **Key derivation uses domain separation** (versioned HKDF info strings);
  no key material is logged, exported, or persisted — the leaf key PEM lives
  in a `Zeroizing` buffer for the duration of a sign call.
- **No downgrade path**: C2PA is additive. `export_verify`'s existing checks
  run unchanged; `--c2pa-manifest` only *adds* failure modes (invalid
  manifest, unbound receipt, mismatched artifact hash ⇒ `TAMPER`).
- **Tests** (`cargo test --features c2pa-export`, wired into CI): credential
  determinism + domain separation, sign→verify round-trip to `Trusted`,
  tamper rejection, wrong-anchor rejection.
