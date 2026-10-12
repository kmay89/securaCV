# The SecuraCV dashboard and the Verified-✓ Timeline (Home Assistant)

The SecuraCV integration ships a **dashboard strategy** and a Lovelace
**timeline card**. Together they turn the entities the integration exposes
into the "single pane of glass": a chain-status header, today's sealed events
with an honest badge on every row, one tile per camera, the daily digest,
health — with no extra backend state and no entity-id surgery.

They read only coarse, privacy-preserving claims the entities already carry
(`event_type`, the zone, the coarse `time_bucket`, `confidence`) plus the
integration's verification attributes. They never invent a precise timestamp
and never surface identity data.

> Not to be confused with the **offline evidence viewer**
> (`viewer/evidence_viewer.html`, [its README](../viewer/README.md)) or the
> **canary-vision web SPA** timeline. Those render and cryptographically verify
> an exported *evidence pack* end-to-end. The dashboard is the live, in-HA view
> built from the integration's entities — complementary, not a replacement.

## The dashboard (strategy)

The strategy generates the whole dashboard from whatever SecuraCV entities
exist when the page opens, in this order:

| Section | What it shows |
|---|---|
| **Header** | the chain state in the product's words (below), **Verify now** and **Export evidence**, one line explaining the coarsening ("person or vehicle", a 10-minute window, a zone — never who or exactly what) and the one-row-per-window rule, and the degraded states: *SecuraCV can't reach the witness kernel since 14:02*, *Frigate not heard for 35 min — check Frigate → Settings → MQTT* |
| **Today** | the timeline card, bound to every witness event entity and the last-event sensor; a quiet install reads *No witness events yet. Frigate heard 2 min ago ✓ — the next person, car, dog or cat Frigate reports appears here sealed* |
| **Cameras** | one tile per camera (`event.securacv_<camera>`): the last sealed event, its window and badge, the count over the last 24 h; Frigate's own picture above it when the Frigate integration owns `camera.<camera>`; an **Open Frigate** link |
| **Last 24 hours** | the kernel's digest by period and by camera, with the chain line |
| **Health** | Frigate link, kernel online, chain length, the witness key fingerprint (to read out by phone), storage |
| **Canaries** | only when any: each Canary's integration entities together with the entities its firmware registers over MQTT, grouped by the shared id |

### Add it

- **Home Assistant 2026.5 and later:** Settings → Dashboards → Add dashboard →
  **Community dashboards** → **SecuraCV**. That is the one click left after
  the integration connects — and the one-time notification **SecuraCV: add
  the dashboard** says exactly that when a kernel entry first loads (it is
  posted only when the strategy module was really served, so an install
  without it is never told about a picker entry it does not have).
- **From YAML** (any version the integration supports): a dashboard whose
  whole configuration is

  ```yaml
  strategy:
    type: custom:securacv
    # frigate_url: https://frigate.local:8971/   # optional, see below
    # hours: 24                                 # how far back Today reads
  ```

  — paste it into a new dashboard's Raw configuration editor, or register
  [`homeassistant/lovelace/securacv-dashboard.yaml`](../homeassistant/lovelace/securacv-dashboard.yaml)
  as a YAML-mode dashboard (the file shows the `configuration.yaml` block;
  `scripts/install.sh` does exactly that).

`frigate_url` is where **Open Frigate** goes. It defaults to the Frigate
app's ingress path on Home Assistant OS (`/hassio/ingress/ccab4aaf_frigate`);
for Frigate elsewhere set its full URL. Without the Supervisor and without
the option there is no link — never a dead one. The link opens Frigate at the
camera, not a clip: the sealed log carries no clip id, by design.

[`homeassistant/lovelace/securacv-dashboard-plain.yaml`](../homeassistant/lovelace/securacv-dashboard-plain.yaml)
is the same dashboard built from stock cards only, for installs that block
auto-registered modules or prefer no custom card. It needs one copied block
per camera.

### Before the integration claims the kernel

On a fresh add-on install the kernel's bridge registers its own MQTT entities
(`sensor.pwk_last_event`, `binary_sensor.pwk_chain_problem`,
`button.pwk_verify_now`, per-zone counters). The strategy and the card show
them too — labeled **Chain OK (kernel-reported)**, never *verified*, because a
kernel's report about itself is not a check Home Assistant ran. Once the
integration connects it claims the kernel, those entities are withdrawn, and
the `securacv_*` entities take over; Settings → Repairs lists the old → new
ids once, on an instance where the `pwk_*` entities existed
([the migration table](homeassistant_setup.md#entity-ids-pwk_--securacv_)).

## The timeline card

The card renders newest-first rows from the recorder history of the witness
event entities and last-event sensors. When that history cannot be read (the
recorder is off, or the history query fails), the card says so in a notice
above the list and shows only each entity's *current* event — it never
presents that snapshot as "the last N hours".

Each row: the event icon and label, a **sensing-modality chip** (when known),
the zone (`front_door`, never `zone:front_door`), the coarse time-bucket
window (`14:00–14:10 (~10 min)`), confidence, the verification badge and —
for Track B claims — an **attestation chip**. A tap on a row opens Home
Assistant's more-info dialog for its entity. Kernel rows carry an **Export**
action that asks `securacv.export_evidence` for a signed pack covering the
row's window; the download link arrives as a notification and under Media →
SecuraCV evidence.

The two boundary claims read **Person or vehicle** and **Animal or small
object**: a person and a car are one claim in the witness log, by design — the
honest coarsening, not a classification bug. The dictionary ids
(`boundary_crossing_object_large`, `boundary_crossing_object_small`) are
unchanged.

### The chain-status header

The header says what the **integration's own chain walk** found: it fetches
the sealed log and checks every Ed25519 signature and hash link against the
key it pinned when you connected. Its copy is fixed:

| Header | Meaning |
|---|---|
| **Chain verified ✓ 12 min ago · 1,204 entries · key pinned** | the last walk passed, against the pinned key |
| **Verifying…** | a walk is running (Verify now, the `securacv.verify` action, the 15-minute backstop, or 60 s after new entries went idle) |
| **Verification failed at entry #1,190** | the walk rejected the chain; later entries can't be trusted — export now, entries before the break still verify |
| **Not verified yet** | no walk has run |
| **Witness key changed — confirm it in Settings → Repairs** | the kernel's key no longer matches the pin (a restore to new hardware, a replaced app); never rendered as a failure |
| **Chain OK (kernel-reported) · 1,204 entries** | an unclaimed bridge: the kernel's own report, not a check Home Assistant ran |

A second pill shows the head hash; a third the tamper state when a Canary
tamper sensor exists.

### Honest verification badges

Kernel (Frigate) rows get exactly three badges, driven by the integration's
`verification` attribute on the event:

| Badge | Meaning | Tooltip |
|---|---|---|
| **Sealed ✓** (muted) | signed into the witness chain, newer than the last chain check | *Signed into the witness chain (entry #1,204). The chain check runs every 15 minutes and a minute after new entries stop arriving; this entry is newer than the last one.* — the integration's cadence, so a row is Sealed for at most that long ([the cadence](homeassistant_setup.md#what-sealed--verified--and-failed--mean)) |
| **Verified ✓** (green) | covered by a chain check the integration ran against the key it pinned | *Entry #1,204 is covered by the chain check at 15:00 against the key pinned when you connected.* |
| **Failed ⚠** | the chain check rejected the chain | *The chain check rejected the chain at entry #1,190; later entries can't be trusted. Export now: entries before entry #1,190 still verify.* |

A kernel self-report never earns the green ✓, and a key change is never shown
as "Failed" — the header points at Repairs instead.

Canary rows keep the per-publish signature vocabulary: a **✓ Signature
verified** badge is shown **only** when the event's Ed25519 signature actually
verified (the integration's `verified` attribute); weaker states are distinct
by label so the badge never overclaims.

| Badge | Meaning |
|-------|---------|
| ✓ Signature verified | Device signature checked and valid against a pinned/TOFU key |
| ✓ Signed (unverified) | Entry is marked signed, but no independent check was available |
| ○ Unsigned | The publish carried no signature (pre-PKI firmware, or a topic the firmware never signs); neutral, not a failure |
| · Logged | Present in the log; no per-event signature surfaced here |
| ⚠ Verification failed | A check ran and failed (e.g. fingerprint mismatch, replay) |

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
resolvable modality render exactly as before — no chip is shown.**

### Attestation provenance (Track A vs Track B)

The verification badge answers *did the check pass*; the **attestation chip**
answers the orthogonal question *who signed the claim*. Native canary firmware
(Track A) signs on-device, so claims are **device-attested** — the default,
with no extra chip. Kernel-sealed Frigate events and stock-kit deployments
(Track B, e.g. an MR60BHA2 bridged via HA `mqtt_statestream`) are signed by
the kernel/adapter at ingest, *not* by a device — so the card renders a
distinct, honest chip rather than implying a device signature it never had:

| Chip | `attestation` value | Meaning |
|------|---------------------|---------|
| *(none)* | `device` (default) | Device-signed at source (Track A) |
| ⬡ Adapter-attested | `adapter` | Kernel/adapter-signed at ingest (Track B, Frigate events) |
| ⌂ HA-bridged | `ha-bridged` | Claim transited Home Assistant before ingest (statestream path) |

### Add the card by itself

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

With no entity options the card **auto-discovers** your SecuraCV entities —
through the entity registry's translation keys when Home Assistant exposes
it, else by the integration's attribute signatures — including an unclaimed
bridge's `pwk_*` entities. To pin them explicitly (several kernels, or to
disambiguate):

```yaml
type: custom:securacv-timeline-card
title: Front door — verified events
event_entities:
  - event.securacv_front_door
  - sensor.securacv_last_event
chain_valid_entity: binary_sensor.securacv_chain_valid
chain_length_entity: sensor.securacv_chain_length
```

> If the card doesn't appear in the picker, hard-refresh the browser (the module
> is cached). If your install blocks auto-registered modules, add the resources
> manually under **Settings → Dashboards → ⋮ → Resources**:
> `/securacv_www/securacv-timeline-card.js` and
> `/securacv_www/securacv-strategy.js`, type **JavaScript Module**.

## Testing

The card's and the strategy's pure helpers (event metadata, modality and
attestation resolution, the three kernel badges and their tooltips, the
header copy, recorder-history de-duplication, entity discovery, the
strategy's model and every generated section) are unit-tested without a
browser, alongside the evidence-viewer parity tests:

```bash
node --test custom_components/securacv/www/securacv-timeline-card.test.js \
            custom_components/securacv/www/securacv-strategy.test.js
```

Both files are also evaluated in a browser-like sandbox by their tests: every
SecuraCV module loads through one `add_extra_js_url` path, so a syntax error
in one would blank the others. This runs in CI in the **Evidence Viewer →
viewer** job.
