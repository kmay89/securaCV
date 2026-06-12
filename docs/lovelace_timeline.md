# Verified-✓ Timeline (Home Assistant)

The SecuraCV integration ships a Lovelace card that turns the entities it already
exposes into a **verified-✓ event timeline with a hash-chain status header** — the
"single pane of glass" payoff, in the dashboard, with no extra backend state.

It reads only coarse, privacy-preserving claims the entities already carry
(`event_type`, `zone_id`, the coarse `time_bucket`, `confidence`) plus the
integration's per-entity verification attributes. It never invents a precise
timestamp and never surfaces identity data.

> Not to be confused with the **offline evidence viewer** (`viewer/evidence_viewer.html`)
> or the **canary-vision web SPA** timeline. Those render and cryptographically
> verify an exported *evidence envelope* end-to-end. This card is the live,
> in-HA view built from the integration's sensors — complementary, not a
> replacement.

## What it shows

- **Chain-status header** — pills for chain integrity (`binary_sensor … Chain Valid`),
  chain length + short head hash (`sensor … Chain Length`), and tamper status
  (`binary_sensor … Tamper`).
- **Event timeline** — newest-first list of recent witness events from the
  recorder history of your last-event sensor(s). Each row shows the event icon +
  friendly label, a **sensing-modality chip** (when known), zone, coarse
  time-bucket window, confidence, a verification badge, and — for Track B
  claims — an **attestation chip**.

### Sensing modality at a glance

Different canaries witness with different physics: a camera person-detection and
a 60GHz radar presence claim can map to the *same* coarse `event_type`
(`presence_in_restricted_zone`) yet mean very different things. When an event
carries modality metadata, the card shows a small chip beside the label so a
mixed fleet reads clearly:

| Chip | Modality | Typical source |
|------|----------|----------------|
| 📷 Camera | `camera` | canary-vision (on-module image inference) |
| 📶 WiFi CSI | `wifi-csi` | canary-wap (WiFi channel-state distortion) |
| 📡 Radar | `radar` | canary-sense / MR60BHA2 (60GHz mmWave) |
| ⌷ Contact | `contact` | reed/contact switch (door/window) |
| · Other sensor | `other` | a known-but-uncategorized medium |

Modality is resolved from the event's `modality` attribute, falling back to the
device's advertised `device_type` (`canary-sense` → radar, `canary-vision` →
camera, `canary-wap` → wifi-csi, `canary-contact` → contact). **Events with no
resolvable modality render exactly as before — no chip is shown** — so this is
fully backward compatible with existing canaries.

### Honest verification badges

A **✓ "Signature verified"** badge is shown **only** when the event's Ed25519
signature actually verified (the integration's `verified` attribute). The card
deliberately distinguishes weaker states by label (the source of truth, since theme
colours can vary) so the **"Signature verified"** badge never overclaims —
signed-but-unverified reuses the ✓ glyph with a "Signed (unverified)" label, as noted below:

| Badge | Meaning |
|-------|---------|
| ✓ Signature verified | Device signature checked and valid against a pinned/TOFU key |
| ✓ Signed (unverified) | Entry is marked signed, but no independent check was available |
| · Logged | Present via the kernel HTTP API; no per-event signature surfaced here |
| ⚠ Verification failed | A check ran and failed (e.g. fingerprint mismatch) |

### Attestation provenance (Track A vs Track B)

The verification badge answers *did the signature check out*; the **attestation
chip** answers the orthogonal question *who signed the claim*. Native canary
firmware (Track A) signs on-device, so claims are **device-attested** — that is
the default, and such events show **no extra chip** (their green ✓ already means
"a device cryptographically vouched for this"). Stock-kit deployments (Track B,
e.g. an MR60BHA2 bridged via HA `mqtt_statestream`) are signed by the
kernel/adapter at ingest, *not* by the device — so the card renders a distinct,
honest chip rather than implying a device signature it never had:

| Chip | `attestation` value | Meaning |
|------|---------------------|---------|
| *(none)* | `device` (default) | Device-signed at source (Track A) |
| ⬡ Adapter-attested | `adapter` | Kernel/adapter-signed at ingest (Track B) |
| ⌂ HA-bridged | `ha-bridged` | Claim transited Home Assistant before ingest (statestream path) |

Only an event that *explicitly* carries `attestation: adapter` or
`attestation: ha-bridged` gets a chip; everything else stays device-attested and
unchanged.

## Add the card

The integration serves the card and auto-registers it as a frontend module, so
in most setups you can add it straight away (no manual Lovelace *resource* step).

Edit a dashboard → **Add Card** → search **"SecuraCV Verified Timeline"**, or add
it in YAML:

```yaml
type: custom:securacv-timeline-card
title: Verified Timeline
hours: 24          # how far back to pull recorder history (default 24)
max_events: 50     # cap the number of rows (default 50)
```

With no entity options the card **auto-discovers** your SecuraCV entities. To pin
them explicitly (e.g. multiple devices, or to disambiguate):

```yaml
type: custom:securacv-timeline-card
title: Front gate — verified events
event_entities:
  - sensor.securacv_canary_frontgate_last_event
chain_valid_entity: binary_sensor.securacv_canary_frontgate_chain_valid
chain_length_entity: sensor.securacv_canary_frontgate_chain_length
tamper_entity: binary_sensor.securacv_canary_frontgate_tamper
```

> If the card doesn't appear in the picker, hard-refresh the browser (the module
> is cached). If your install blocks auto-registered modules, add the resource
> manually under **Settings → Dashboards → ⋮ → Resources**:
> URL `/securacv_www/securacv-timeline-card.js`, type **JavaScript Module**.

## Pure-YAML fallback (no custom card)

If you prefer not to use a custom JS card, build an equivalent view from built-in
cards over the same entities. The timeline becomes HA's native **logbook**, and
verification/chain status are surfaced as entity rows and a templated summary:

```yaml
title: SecuraCV
views:
  - title: Witness
    cards:
      - type: markdown
        content: >
          ## Chain status
          {% set cv = states('binary_sensor.securacv_canary_frontgate_chain_valid') %}
          {% set tp = states('binary_sensor.securacv_canary_frontgate_tamper') %}
          - Chain: {{ '✅ intact' if cv == 'on' else '⚠️ broken / unknown' }}
          - Tamper: {{ '⚠️ detected' if tp == 'on' else '✅ none' }}
          - Length: {{ states('sensor.securacv_canary_frontgate_chain_length') }} blocks
          - Head: `{{ state_attr('sensor.securacv_canary_frontgate_chain_length', 'latest_hash')[:8] }}…`

      - type: entities
        title: Verification
        entities:
          - entity: binary_sensor.securacv_canary_frontgate_chain_valid
            name: Chain valid
          - entity: binary_sensor.securacv_canary_frontgate_tamper
            name: Tamper
          - type: attribute
            entity: sensor.securacv_canary_frontgate_last_event
            attribute: trust_reason
            name: Last event trust

      - type: logbook
        title: Event timeline
        hours_to_show: 24
        entities:
          - sensor.securacv_canary_frontgate_last_event
```

Adjust the entity IDs to match yours (**Developer Tools → States**, filter
`securacv`). The custom card and the YAML fallback read the same entities, so you
can switch between them freely.

## Testing

The card's pure data-shaping helpers (event metadata, modality + attestation
resolution, verification-badge resolution, recorder-history de-duplication,
entity discovery) are unit-tested without a browser, alongside the
evidence-viewer parity tests:

```bash
node --test custom_components/securacv/www/securacv-timeline-card.test.js
```

This runs in CI in the **Evidence Viewer → viewer** job.
