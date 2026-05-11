# Beacon ↔ CAP Gateway Interop v0 (Spec only — no v0 implementation)

Status: Draft v0.1, **specification only**
Intended Status: Informational (until a future release implements it)
Last Updated: 2026-05-11
Companion specs: `spec/beacon_channel_v0.md`, `spec/chirp_channel_v0.md` (v0.2)
Research basis: `docs/research/harm_reduction_prior_art.md`

## 1. Purpose

Define **how** a Beacon network could interoperate with the OASIS Common Alerting Protocol (CAP) ecosystem — in both directions — without (a) impersonating an official emergency alert, (b) requiring civilians to obtain IPAWS Alerting Authority status, or (c) violating any of the non-impersonation rules in `docs/research/harm_reduction_prior_art.md` §2.

**This specification is intentionally not implemented in v0.** It exists so that:

1. The Beacon wire format and template set are committed to a shape that *would* work for CAP interop, preventing future schema churn.
2. A future authorized gateway operator (NWS office, building management with FEMA designation, university campus alert system) has a clear specification to implement against.
3. Local Beacon audit logs are exportable to CAP-compatible JSON for after-action review, even without a live gateway.

If and when a real gateway is implemented, it MUST be in a separate, explicitly-named firmware build that ships with a per-deployment legal review and a designated operator identity.

## 2. Direction A — Inbound (CAP → Beacon)

### 2.1 Use cases

- NWS issues a Tornado Warning for a polygon that covers a building. A trusted gateway device in the building translates the CAP `info` block into `BCN_WX_TORNADO` and originates a Beacon under its `gateway` trust-level pubkey.
- A campus emergency-management system pushes "shelter in place" CAP alerts to a gateway running on campus; receivers throughout campus see `BCN_EMERG_SHELTER_IN_PLACE` with a "from campus operations" badge.
- A building management system detects a fire alarm panel activation and bridges it to Beacon as `BCN_EMERG_FIRE_VISIBLE`.

### 2.2 Gateway trust model

A gateway is a Beacon-set member with `trust_level = 1`. Adding a gateway requires the same visual pairing flow as a regular cosigner, but the operator selects "Pair as gateway" in the UI; this requires a separate hold-confirm and surfaces a warning ("Gateway devices may originate alerts solo. Only pair gateways you trust to be operated correctly.").

Per beacon set, a maximum of **3 gateway entries** is permitted. This is a hard cap in the firmware to prevent a deployment from being silently overrun by gateway-originated traffic.

### 2.3 Gateway origination rules

A gateway pubkey may originate Beacon frames **solo** (no cosigner required) **if and only if** the frame carries a verifiable upstream signature in a new field:

```c
struct BeaconGatewayAttestation {
  uint8_t  upstream_kind;       // 0=CAP_XML_DSIG, 1=NWS_NOAA_HMAC, 2=IPAWS_OPEN_PLATFORM, 255=other
  uint16_t upstream_sig_len;    // network byte order
  uint8_t  upstream_sig[upstream_sig_len];
  uint8_t  upstream_source_id[32];  // SHA-256 of the upstream feed identifier
};
```

This appended block sits after the regular dual-signature frame; the `cosigner_fp` is the gateway's own derived "cosigner-of-record" key (an internal artifact, distinct from the gateway's primary pubkey, so that the regular two-pubkey verification path still applies cryptographically).

Receivers validate the upstream signature against a small built-in trust root (NWS public key, FEMA IPAWS public key, etc.) before accepting. The trust root is part of the firmware build, updated via OTA.

If the upstream signature cannot be validated, the frame is rejected. The gateway gets no special privilege without producing the upstream attestation.

### 2.4 Display rules for gateway-originated frames

Receivers MUST:

- Display the gateway's user-friendly name from the local beacon set entry, prefixed "via":
  `🛰 via NWS Atlanta — tornado warning`
- Show the upstream-source identifier in a "details" view.
- Treat the urgency/severity as advisory — receivers MAY display gateway-originated frames at one notch lower visual prominence than community-originated frames, on the theory that the local community is the authoritative source for "is something really happening here right now."
- NEVER display a gateway frame with red color, WEA tone, or protected phrasing.
- NEVER claim the frame is an "official emergency alert" — display it as "advisory from gateway X" only.

### 2.5 CAP → Beacon template mapping

This table is normative. Adding a new CAP category requires a Beacon template change (schema bump).

| CAP `category` | CAP `responseType` | Maps to Beacon template | Notes |
|---|---|---|---|
| `Fire` | `Evacuate` | `BCN_EMERG_EVACUATION` or `BCN_EMERG_FIRE_VISIBLE` | Choose by severity |
| `Health` | `Monitor` | `BCN_EMERG_MEDICAL_SCENE` | Single-incident only |
| `Health` (event = "Multiple Casualty Incident") | `Monitor` | `BCN_EMERG_MULTIPLE_AMBULANCE` | Coordinates community awareness without identifying patients |
| `Safety` | `Evacuate` | `BCN_EMERG_EVACUATION` | |
| `Safety` | `Shelter` | `BCN_EMERG_SHELTER_IN_PLACE` | |
| `Safety` | `AllClear` | `BCN_CLR_RESOLVED` | Default all-clear after an active alarm |
| `Safety` | `AllClear` (post-investigation) | `BCN_CLR_SAFE` | Area confirmed safe (e.g. building cleared) |
| `Safety` | `AllClear` (origination mistake) | `BCN_CLR_FALSE_ALARM` | Explicit false-alarm cancel — separate audit bucket |
| `Infra` | `Avoid` | `BCN_INFRA_GAS_SMELL` | If `event` mentions gas |
| `Infra` | `Monitor` | `BCN_INFRA_POWER_OUT` | If `event` mentions power |
| `Met` | `Monitor` | `BCN_WX_SEVERE_WARNING` | Default |
| `Met` (event = "Tornado Warning") | `Shelter` | `BCN_WX_TORNADO` | |
| `Met` (event = "Flood Warning") | `Avoid` | `BCN_WX_FLOOD` | |
| Anything else | n/a | **Reject** | Gateway logs and drops |

The gateway is required to drop any CAP message that doesn't map to one of the 13 Beacon templates. This is the discipline that keeps Beacon's scope narrow. **No free text, no fall-through, no "miscellaneous" template.**

### 2.6 CAP fields preserved across translation

For audit and after-action review:

| CAP field | Beacon canonical equivalent | Stored in audit log? |
|---|---|---|
| `identifier` | mapped to `nonce` | Yes (full CAP identifier in audit log) |
| `sender` | gateway's user-friendly name | Yes |
| `sent` | `effective` | Yes |
| `status` | (via `flags`) | Yes |
| `msgType` | `msg_type` | Yes |
| `scope` | always `Private` on wire; original CAP scope in audit log | Yes |
| `category` | (mapped via table above) | Yes |
| `responseType` | (mapped via table above) | Yes |
| `urgency`/`severity`/`certainty` | direct | Yes |
| `effective`/`onset`/`expires` | `effective`/`expires` | Yes (onset in audit) |
| `headline`/`description`/`instruction` | not on wire (template + detail_slot only) | Yes (audit log retains for export) |
| `area`/`geocode` | not on wire (Beacon is hop-scoped, not geo-scoped) | Yes (audit log retains) |

## 3. Direction B — Outbound (Beacon audit log → CAP-compatible export)

### 3.1 Use cases

- An authorized operator (building manager, university campus safety office, neighborhood-scale public-health researcher with IRB approval) wants to review what Beacons fired in their building over the past month, in a format their existing tools (EMnet, IPAWS-OPEN, Everbridge, Rave) can parse.
- A post-incident review by emergency services wants to understand what the community-level signaling looked like in the minutes before official response arrived.

### 3.2 Export format

JSON Lines (.jsonl), one record per Beacon. **Not** CAP XML — using CAP XML would invite confusion with actual official CAP messages. CAP-compatible JSON deliberately distinct.

```json
{
  "schema": "securacv.beacon.audit/v0",
  "record_type": "alert" | "cancel" | "exercise" | "selftest_summary",
  "nonce_hex": "...",
  "received_at": "2026-05-11T14:31:02Z",
  "originated_at": "2026-05-11T14:30:58Z",
  "msg_type": "Alert" | "Update" | "Cancel" | "Exercise",
  "status": "Actual" | "Test" | "Exercise",
  "scope": "Private",
  "cap_mapping": {
    "category": "Fire",
    "responseType": "Evacuate",
    "urgency": "Immediate",
    "severity": "Severe",
    "certainty": "Likely"
  },
  "beacon": {
    "template_id": "BCN_EMERG_FIRE_VISIBLE",
    "template_text": "fire or smoke visible",
    "detail_slot": "DETAIL_STATUS_ONGOING",
    "hop_count": 1
  },
  "signers": {
    "originator_fp_hex": "...",
    "originator_name": "Kitchen Canary",
    "cosigner_fp_hex": "...",
    "cosigner_name": "Hallway Canary",
    "signatures_verified": true
  },
  "gateway_attestation": null,
  "active_until": "2026-05-11T14:45:58Z"
}
```

### 3.3 Export rules

- Export is **user-initiated only**. There is no auto-publish to anywhere.
- Export contains only what the local audit log already retains; no live polling.
- Pubkeys are exported as **fingerprints** only, never full pubkeys, to limit reconstruction value of the export.
- Names are exported from the local beacon set; names are human-assigned and may be redacted at export time via a `--anonymize-names` flag.
- The export carries the original Ed25519 signatures so a consumer can re-verify authenticity against pubkeys they trust.
- Exports are **never sent to IPAWS, EAS, WEA, or any official channel**. The export schema explicitly does not match CAP XML to make this hard to do by accident.
- Exports MUST carry a warning header:

  ```
  # securacv.beacon.audit/v0 — community advisory log, not an official alert feed
  # Generated by SecuraCV Canary firmware v<x.y.z>
  # This is NOT a CAP feed. It is a community signaling audit log.
  # Distribution to IPAWS / EAS / WEA / 911 / official channels is prohibited.
  ```

### 3.4 Privacy at export

Even with fingerprints and optional name redaction, an export reveals which beacon set fired which alarms when. This is itself sensitive. The export endpoint requires:

- Bearer-token auth (same as other admin endpoints).
- Hold-to-confirm UI (2 s hold, like origination).
- A warning dialog: "This export reveals your building's alarm activity over the selected period. Share only with operators you trust."
- An export receipt entered in the audit log, signed by the device generating the export.

## 4. Non-impersonation contract (CI-enforced)

The lint script `scripts/lint_no_impersonation.sh` MUST fail the build if any of the following appear in firmware sources, HTML/JS UI sources, or audio frequency tables:

| Forbidden item | Why |
|---|---|
| `"Wireless Emergency Alert"` or `"WEA"` (as a label) | Reserved phrase |
| `"AMBER Alert"` or `"AMBER ALERT"` | NCMEC trademark |
| `"Silver Alert"` | State-program reserved |
| `"Presidential Alert"` | 47 CFR §10 reserved |
| `"Civil Emergency Message"` or `"CEM"` (as code) | EAS event code |
| `"This is a test of the Emergency Alert System"` | EAS test phrasing |
| `"Emergency Alert System"` or `"EAS"` (as label, not as reference text) | Reserved |
| `"IPAWS"` (in user-visible strings) | Reserved program name |
| Audio tone `853 Hz` combined with `960 Hz` in any single pattern | WEA two-tone |
| `8000` ms `853` and `960` adjacent | EAS attention signal |
| Color `#FF0000` or any pure red as primary alert color | Visual confusion with EAS/IPAWS |

The lint runs in CI on every PR that touches `firmware/`, `homeassistant/`, or `canary-vision/`. It uses simple `grep -rE` patterns; false positives are resolved by code review.

## 5. Open questions for future implementation

1. **Which upstream signatures to trust?** NWS publishes their public key; FEMA IPAWS-OPEN uses a different model. The trust root needs operational care — same problem as a CA bundle.
2. **Gateway provisioning UX.** Pairing a gateway is different from pairing a neighbor; the UI must make this clear.
3. **Per-jurisdiction template extensions.** Different countries have different relevant templates (typhoon vs hurricane vs cyclone — same hazard, different word). Internationalization of template text is a separate concern; mapping table stays language-agnostic via template IDs.
4. **Rate-limit interactions.** Gateway-originated frames count against the gateway's per-pubkey limit, but if a real emergency justifies many alerts in rapid succession (e.g., tornado warning followed by update followed by extension), the limit must allow it. Proposal: gateway pubkeys get `MAX_ORIGINATIONS_PER_PUBKEY_24H = 50` instead of 5, justified by the upstream attestation requirement.
5. **Acknowledgment back upstream.** CAP supports `Ack` msgType. Whether and how to feed acknowledgments back to the upstream system is an operator-specific concern, not a protocol concern.

## 6. Changelog

- v0.1 (2026-05-11): Initial draft. Specification only; no implementation in this firmware release.
