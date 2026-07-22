# Opt-in hardware root of trust for the Canary (ESP32-S3) — design

Status: **Accepted (v1)** — defaults decided (§8): the reversible tiers (1–2) are
the path for every Canary; the irreversible tiers (3–4) are opt-in only.
Intended Status: Informative (architecture decision record)
Last Updated: 2026-07-22

> This RFC records *policy and protocol decisions*, and adds **no code** itself.
> Every operation it discusses at Tier 3+ is **irreversible silicon** — an eFuse
> burn cannot be undone, and a wrong step bricks the board permanently. None of
> the irreversible tiers is wired into the build, the firmware, or the flasher.
> The one in-repo thing that *can* burn eFuses —
> `firmware/provisioning/provision_canary.sh` (§2.3) — is **manual, dry-run-first
> operator tooling** a human runs deliberately; it is never invoked automatically,
> and §5.2 specifies the gating it still needs (enforced key backup +
> surrendered-guarantees prompt) before any fleet locks a board.

## 1. Summary

SecuraCV already has most of the *ingredients* for a hardware root of trust on
the ESP32-S3: a self-provisioned per-device Ed25519 identity, an Ed25519-signed
OTA/release chain with a pinned key, and a **prepared-but-not-enabled** Phase-2
lockdown kit (`firmware/provisioning/`, documented in
[`secure_provisioning.md`](../secure_provisioning.md)) that stages Secure Boot v2,
flash encryption, anti-rollback, and JTAG lockdown.

What's missing is not more tooling — it's the **decision layer**:

1. A **hardware root of trust is fundamentally at odds with the Canary's loudest
   promise** — *"you cannot brick your Canary"* — because the same eFuses that
   make firmware un-tamperable also make the board un-recoverable. No document
   reconciles the two. This RFC does, by making lockdown a deliberate, tiered,
   **opt-in** trade rather than a hidden "Phase 2".
2. There is no **attestation protocol**: a device holds an identity key but has
   no defined way to *prove* to a verifier "I am a genuine Canary running trusted,
   un-tampered firmware." This RFC specifies one, offline-verifiable by design.
3. The identity key lives in **plaintext NVS** today; a physical attacker can
   read it out. This RFC proposes binding it to hardware (the ESP32-S3 DS
   peripheral) for deployments that need it.

The decision (§8, now settled) is deliberately conservative: **every Canary gets
the reversible value** (anti-rollback, boot safe-mode, flasher awareness, software
attestation) — tamper-*evidence* and rollback safety while staying un-brickable;
the **irreversible** tamper-*resistance* (full Secure Boot + flash encryption +
JTAG-off) is **opt-in only** — explicit, heavily-warned, key-backup enforced — for
the deployments that genuinely need it, and is never the default.

## 2. Current state — what is already built

### 2.1 Per-device identity (done)

On first boot the firmware self-provisions an Ed25519 keypair from the hardware
RNG (`esp_fill_random`), stores it in NVS, and derives a device fingerprint from
the public key ([`secure_provisioning.md` § Phase 1, Step 3](../secure_provisioning.md)).
The device advertises this identity via the signed self-manifest (`j`, schema
`securacv.canary.manifest/v1`) that the browser flasher and
[`self_star_roadmap.md`](self_star_roadmap.md) already read back. Trust in that
key is **pinned** (stricter than TOFU — [`device_trust.md`](../device_trust.md)).

### 2.2 Release + OTA signing (done)

Releases are Ed25519-signed over `uint32_le(size) || sha256(image)`
(`firmware/scripts/ota_release.py`); devices verify against a public key compiled
into `firmware/common/ota/src/ota_release_key.h` before installing over the
pull-OTA engine ([`firmware_ota.md`](../firmware_ota.md)). This is an
*application-layer* signature: it proves a release is authentic, but it is checked
by *firmware that is itself unverified at boot*. That gap is exactly what a
hardware root of trust closes.

### 2.3 The Phase-2 lockdown kit (prepared, **not** enabled)

`firmware/provisioning/` ships real tooling — `generate_keys.sh` (RSA-3072 Secure
Boot key + XTS-AES flash-encryption key), `sdkconfig.defaults.secure`,
`partitions_secure.csv` (encrypted partitions + `nvs_keys`), `provision_canary.sh`
(virgin-verify → burn → flash → post-verify, with `--dry-run`), and
`verify_device.py` (reads the security eFuses). [`secure_provisioning.md`](../secure_provisioning.md)
documents the eFuse target state. The firmware full audit
([`audit/esp32s3_firmware_full_audit_2026-07.md`](../audit/esp32s3_firmware_full_audit_2026-07.md))
records these as **"exist as a reference … not wired up"** — a staged capability,
not a live one.

### 2.4 The un-brickable promise (done — and the thing in tension)

[`browser_flasher.md`](../browser_flasher.md) and `canary-local/devices/flash.json`
state it plainly:

> **"You cannot brick your Canary from here.** The ESP32's first-stage bootloader
> lives in mask ROM — it can't be erased or overwritten over USB. If a flash is
> interrupted or an image is wrong, the board just drops back into download mode
> and you flash again."

The recovery matrix's last row is *"Absolute worst case → USB flash — always
possible."* **Secure Boot + flash encryption + JTAG-off deletes that row.** A
locked board with a bad signed image or a lost signing key is a paperweight. Any
honest hardware-RoT design must own this, not bury it.

## 3. The gap, restated as three problems

1. **Recoverability vs. tamper-resistance is an unmade choice.** "Phase 2" is
   framed as an eventual default, but enabling it silently revokes the
   un-brickable guarantee the whole first-flash/recovery story rests on. It must
   be an explicit, per-deployment decision with the trade stated up front.
2. **No attestation.** The device can *sign*, but there's no protocol for a
   verifier to confirm genuineness + firmware integrity, and no measurement to
   sign over. "Boot attestation record" today is local and unused.
3. **Key extraction.** The identity key is plaintext-at-rest. Without flash
   encryption or a hardware-bound key, physical possession = key compromise,
   which undermines both attestation and the pinned-identity model.

## 4. Goals & non-goals

**Goals**

- Turn the staged lockdown into a **coherent, tiered, opt-in** root of trust,
  with each tier's *irreversibility and recovery cost* stated exactly.
- **Reconcile with un-brickability**: define precisely what each tier removes,
  keep the **default un-brickable**, and specify how locked devices recover
  (signed OTA + A/B + boot safe-mode replacing "USB reflash").
- An **offline-verifiable attestation protocol** bound to the root of trust.
- A path to a **hardware-bound identity key** (non-extractable) via the ESP32-S3
  DS peripheral, for deployments that need it.

**Non-goals**

- **Making any of this the default or mandatory.** The base Canary stays Tier 0
  (un-brickable, user-sovereign). Principles 4 & 10 (offline, sovereignty) forbid
  shipping an irreversible lock a user didn't choose.
- **Replacing Ed25519 OTA signing.** Secure Boot v2 (RSA-3072 / ECDSA) protects
  the *boot chain*; the Ed25519 release signature protects the *release*. They are
  complementary layers, not substitutes.
- **A mandatory attestation *service* / phone-home.** Attestation must be
  verifiable locally and offline (Principle 2: zero phone-home). No cloud is in
  the trust path.
- **Nation-state physical security.** Flash encryption defends key-at-rest and
  casual readout, not decapping/fault-injection by a well-funded lab. We say so.

## 5. Proposed design

### 5.1 The opt-in ladder (irreversibility increases down the rungs)

| Tier | What it adds | Reversible? | Un-brickable? | For |
|---|---|---|---|---|
| **0** (default) | Ed25519 identity in NVS; signed OTA | — | **Yes** | Everyone |
| **1** | Anti-rollback (A/B + boot self-test/safe-mode) | Yes (software) | Yes | Everyone — should become default |
| **2** | Software **attestation** (signed challenge-response + firmware self-measurement) | Yes | Yes | Anyone wanting a signed, replay-proof health report |
| **3** | Flash encryption (**development** mode) — identity key protected at rest; DS-bound key optional (§8 #4) | Partly (still reflashable) | Mostly | Physical-theft threat models |
| **4** | Secure Boot v2 + flash encryption (**release**) + JTAG-off | **No — eFuse** | **No** | High-assurance deployments only |

Tiers 1–2 are the "every Canary should have this" band — pure software, no eFuse,
fully recoverable. Tiers 3–4 are the opt-in tamper-resistance band, where the
un-brickable guarantee is knowingly traded away.

### 5.2 Reconciling with the flasher & recovery (the crux)

Once Secure Boot + flash encryption are on (Tier 4), the mask-ROM download path
still exists but the ROM **refuses to boot an unsigned image**, and flash reads
come back ciphertext — so the browser flasher's "write any factory image and boot
it" no longer works, and JTAG is gone. Recovery shifts, and the design must make
that shift safe and legible:

- **The flasher must detect a locked device and refuse loudly, not half-flash
  it.** `verify_device.py`'s eFuse logic ported to a read-only check the page can
  run (or a serial probe) → if `SECURE_BOOT_EN` / `FLASH_CRYPT_CNT` are set, show
  *"This Canary is hardware-locked; it updates only via signed OTA"* instead of
  attempting a write that the ROM will reject. This is a Tier-2 deliverable and is
  worth doing **even if Tier 4 never ships**, so a hand-provisioned unit isn't met
  with a confusing failure.
- **Locked devices recover via the OTA net, not the USB net.** Signed OTA still
  installs (the secure bootloader verifies each app), and the A/B partitions +
  anti-rollback (Tier 1) + boot safe-mode ([`self_star_roadmap.md`](self_star_roadmap.md)
  "coming-soon: boot safe-mode / A/B rollback") become the safety net that
  *replaces* "USB reflash — always possible." The recovery matrix gains a new last
  row for locked units: *"bad app → A/B rollback + safe-mode; the USB row is
  intentionally gone."*
- **The point of no return is a gated ceremony.** `provision_canary.sh --phase 2`
  stays `--dry-run`-first, refuses to proceed without a verified **offline backup
  of the signing key** (lost key = un-updatable fleet), and prints the exact
  guarantees being surrendered. It is operator tooling, never a one-click button
  and never anything the browser flasher can trigger.

### 5.3 Attestation protocol (offline-verifiable)

**Goal:** a verifier (Home Assistant, an operator, a peer Canary) confirms a
device is a genuine Canary and — *at the measured tier (see below)* — that it is
running trusted firmware, with no cloud.

**Challenge–response.** Verifier sends a nonce; the device returns

```
attestation = {
  device_pub:   <Ed25519 identity pubkey>,
  fw_measure:   <sha256 of the running app (esp_app_desc / esp_ota_get_app_description),
                 and — at Tier 4 — the Secure Boot public-key digest from eFuse>,
  boot_state:   { secure_boot: bool, flash_enc: bool, rollback_ver: u32 },
  nonce:        <verifier nonce, echoed>,
  sig:          Ed25519_sign(identity_key, "securacv.attest/v1" || nonce || fw_measure || boot_state)
}
```

The verifier checks: (a) `sig` against the **pinned** `device_pub`
([`device_trust.md`](../device_trust.md)); (b) `fw_measure` against the expected
release measurement (derivable from the signed release manifest); (c) `boot_state`
matches the tier the operator enrolled. This extends the existing
`securacv.canary.manifest/v1` self-manifest with a *signed, fresh, nonce-bound*
statement — turning "the device says it's healthy" into "a device holding the
pinned key attests it, freshly and un-replayably." How much the *firmware* half
of that claim is worth then depends on the tier:

**What each tier's attestation actually proves.** A measurement is only as
trustworthy as whatever produced it. Without Secure Boot (Tiers 0–2) the *same,
unverified app* both computes `fw_measure` and signs it — so a malicious image
flashed over USB that preserves NVS keeps the identity key **and** can simply
report the expected hash; a verifier cannot tell it from honest firmware. So at
Tiers 1–2 the attestation is a **signed self-report**, not proof of trusted
firmware: it reliably catches a clone that *lacks* the pinned key, flags benign
version drift (an honest device reporting an old measurement), and binds
freshness via the nonce (no replay) — but it does **not** catch a modified image
that lies about itself. Only once Secure Boot measures the app in the boot chain
(Tier 4) does `fw_measure` become trustworthy and *"proves it's running trusted
firmware"* actually hold. Tier 3's non-extractable key (§5.4) additionally stops
the identity key being lifted out of a stolen board, closing the
clone-with-stolen-key gap.

### 5.4 Hardware-bound identity via the DS peripheral (Tier 3+)

The ESP32-S3 **Digital Signature (DS)** peripheral signs with an RSA private key
that is stored **AES-encrypted under an HMAC key burned read-protected into
eFuse**; the plaintext private key never appears in memory or flash, and signing
happens in hardware. Binding an *attestation* key this way means a stolen board
can't yield a usable identity key even with full flash readout.

Caveat worth stating in the RFC, not hiding: **DS is RSA/ECDSA, the existing
identity is Ed25519.** Options: (a) add a *separate* DS-bound RSA attestation key
alongside the Ed25519 identity (two keys, clear roles); (b) keep Ed25519 but
protect it via flash encryption + NVS encryption only (extractable by a
decapping-class attacker, not a casual one). §8 asks which.

### 5.5 Hardware reality — what the ESP32-S3 actually gives us

Mirroring the honesty of the vault RFC's §5.5
([`vault_operator_ux_v1_1.md`](vault_operator_ux_v1_1.md)):

- **Secure Boot v2** — ROM verifies the bootloader signature, bootloader verifies
  the app; up to 3 revocable key digests in eFuse. Real, ROM-anchored. On the
  **ESP32-S3 (and C3) the only scheme is RSA-3072-PSS** — those chips have no
  ECDSA secure-boot path; the fleet's ESP32-C6 members (canary-sense) additionally
  have an ECDSA-capable ROM.
- **Flash Encryption** — XTS-AES-128/256, key in read-protected eFuse.
  *Development* mode still allows re-flashing with the key present; *Release* mode
  disables plaintext serial programming. Protects at-rest, **not** a live chip.
- **DS + HMAC peripherals** — hardware-bound signing key as above.
- **eFuse** — one-time-programmable. Every security bit here is a one-way door.
- **No discrete secure element / TPM.** The root of trust is the SoC itself; there
  is no anti-tamper mesh, no certified key store. We inherit the ESP32-S3's
  published resilience and its known limits (fault-injection research exists).

## 6. Invariant & principle preservation

- **Principle 2 (zero phone-home):** attestation verifies offline; no cloud in the
  trust path.
- **Principles 4 & 10 (offline / sovereignty):** the default stays Tier 0 and
  un-brickable; lockdown is opt-in; the **owner holds the Secure Boot key** — the
  project never does, and never gains a remote lock over a user's device.
- **Recoverability is preserved by substitution, not assertion:** Tiers 1–2 add
  the A/B + safe-mode net *before* any tier removes the USB net, so no unit is
  ever left with neither.
- **Attack surface:** the secure profile keeps Bluetooth compiled out
  (`CONFIG_BT_ENABLED=n`, already in `sdkconfig.defaults.secure`); no new default
  service is exposed.
- **Naming:** a group of Canaries is a **fleet**, here and in all provisioning
  tooling/UI.

## 7. Phased task breakdown

- **Phase 0 — this RFC. ✅ Done** (§8 signed off, 2026-07-22). Nothing
  irreversible; no code that burns eFuses merges.
- **Phase 1 — reversible safety net (highest value, no eFuse).** Wire
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` + boot self-test/safe-mode (the audit
  §"partition table + sdkconfig" recommendation, and the `self_star_roadmap`
  coming-soon item). Every Canary gets rollback safety; still un-brickable.
- **Phase 2 — attestation + flasher awareness (software).** Implement
  `securacv.attest/v1` (challenge-response + measurement) on the existing identity
  key; add the read-only "is this device locked?" probe so the flasher refuses
  locked units cleanly. No eFuse.
- **Phase 3 — key-at-rest protection.** Flash encryption (development mode) so the
  Ed25519 identity is protected at rest (§8 #4 default); a DS-peripheral-bound RSA
  key is added *only* where a deployment needs non-extractability against a full
  flash readout. Validated on dev boards; still reflashable.
- **Phase 4 — full lockdown ceremony (opt-in, irreversible).** Secure Boot v2 +
  flash encryption (release) + JTAG-off via the gated `provision_canary.sh` flow,
  with enforced key backup and the surrendered-guarantees printout. Ships only for
  operators who explicitly choose it.

## 8. Decisions (accepted — v1, 2026-07-22)

Signed off by the maintainer. Each decision keeps the reversible, sovereign
default and pushes every irreversible choice into the opt-in band. "Decided"
here means the **default** architecture; a deployment that opts into Tiers 3–4 is
choosing the alternative knowingly, per §5.1–5.2.

1. **Offer the irreversible lockdown (Tier 4) at all? — Yes, but opt-in only.**
   The default Canary never leaves Tiers 0–2 (un-brickable). Tier 4 exists solely
   for deployments that accept losing field-recoverability in exchange for
   physical tamper-resistance, behind the gated ceremony in §5.2.
2. **Secure Boot scheme — RSA-3072-PSS.** It is the only scheme the ESP32-S3 and
   ESP32-C3 ROM support (and what `generate_keys.sh` already produces), so one key
   type serves one signing ceremony across the fleet. ECDSA-P256 is recorded only
   as a future per-chip possibility for the ESP32-C6 members (canary-sense) — not
   a scheme choice now.
3. **Flash-encryption mode — development mode at Tier 3; release mode only inside
   full Tier 4.** Dev mode is the sweet spot: it protects WiFi/keys at rest from a
   thief while the device stays field-recoverable. Release mode (which disables
   plaintext re-flashing) belongs only with the full, opt-in lockdown.
4. **Attestation key binding — Ed25519 identity key under flash/NVS encryption
   (§5.4b) is the default;** the DS-peripheral-bound RSA key (§5.4a) is added
   **only** where a deployment needs non-extractability against a full flash
   readout. Simpler, and it reuses the identity the device already self-provisions.
5. **Anti-rollback floor — app-level A/B (reversible) is the default;** the
   irreversible `SECURE_VERSION` eFuse counter is burned **only at Tier 4.** A
   fused floor is permanent — a bad version bump could strand a device forever — so
   it stays out of the default path.
6. **Flasher policy for a locked device — hard-refuse with guidance** ("this
   Canary is hardware-locked; it updates via signed OTA"). The owner's signing key
   is **never** placed in the browser — that would defeat the lock and be a
   catastrophic key-exposure risk.
7. **Key relationship — keep the RSA-3072 Secure Boot key and the Ed25519 OTA
   release key fully separate.** Different roles, different ceremonies, isolated
   blast radius: a compromise of one does not compromise the other.

**Build order that follows from these decisions** (§7): **Phase 1** — Tier 1
(anti-rollback + boot safe-mode, pure software, no eFuse) — is the next thing to
build; **Phase 2** — Tier 2 (the `securacv.attest/v1` protocol + the flasher's
locked-device detection) — follows. Phases 3–4 (the eFuse-burning path) stay
design-only until a specific deployment requests them.

## 9. Appendix — references & the measurement payload

- eFuse target state: [`secure_provisioning.md` § Security eFuse Reference](../secure_provisioning.md).
- Provisioning tooling: `firmware/provisioning/` (`generate_keys.sh`,
  `provision_canary.sh`, `verify_device.py`, `sdkconfig.defaults.secure`,
  `partitions_secure.csv`).
- ESP-IDF: [Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/secure-boot-v2.html),
  [Flash Encryption](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/flash-encryption.html),
  [Digital Signature](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/ds.html).
- Prior art on attestation posture: the firmware full audit's references (Secure
  Enclave / Find My network) — [`audit/esp32s3_firmware_full_audit_2026-07.md`](../audit/esp32s3_firmware_full_audit_2026-07.md).
- Related: [`firmware_ota.md`](../firmware_ota.md), [`device_trust.md`](../device_trust.md),
  [`browser_flasher.md`](../browser_flasher.md), [`self_star_roadmap.md`](self_star_roadmap.md),
  [`supply_chain_transparency.md`](../supply_chain_transparency.md) (the *build*-side
  companion to this *device*-side root of trust).
