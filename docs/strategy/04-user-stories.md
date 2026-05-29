# 04 — User Stories: Buy → Install → Use

Five personas drawn from the audiences the code actually serves. Each story walks the full
journey and flags **current friction** vs **what "good" looks like**. Acceptance bullets are
written so they can become product requirements.

---

## Persona A — Priya, the privacy-conscious prosumer (LEAD SEGMENT)

*Runs Home Assistant on a Pi 5, has 3 Reolink RTSP cameras, hates subscriptions, comfortable
with YAML but not with Rust.* This is the **largest reachable market today** and SecuraCV
already fits her stack.

- **Buy**: nothing to buy — she reuses her cameras + Pi. (Optional: a $25–60 Coral TPU.)
- **Install**: finds SecuraCV on HACS / an HA add-on, clicks install, runs the wizard.
- **Use**: cameras show a **verified ✓** per event; a morning digest summarizes counts per
  zone; pattern alerts ping her phone for unusual-hour activity. She never pays a monthly fee.

| | Current state | "Good" |
|---|---|---|
| Buy | ✅ uses existing gear | ✅ |
| Install | ⚠️ `curl \| bash` + manual add-on; ONNX model hand-download for AI | One-click add-on; detection model bundled |
| Use | ⚠️ events visible as HA sensors; no rich timeline; digest works | Polished timeline card + digest + alerts in HA |

**Acceptance:** install completes from the HA add-on store without a terminal; first verified
event visible within 15 minutes; no recurring cost.

---

## Persona B — Marcus, the at-risk / evidence user (THE "WHY WE EXIST" STORY)

*A tenant documenting unauthorized landlord entry; could be a journalist, activist, or abuse
survivor.* Needs records that **hold up and can't be quietly altered** — by anyone, including
a coercive party with physical access. This is a smaller, harder-to-reach market, but it is the
**moral and marketing core** of the product and the source of its differentiation.

- **Buy**: wants something cheap and unobtrusive — ideally a pre-flashed Canary device.
- **Install**: needs it to "just work" with minimal config; sets trustees for break-glass.
- **Use**: events are logged tamper-evidently; if an incident occurs he can produce a
  **court-credible, signed export** that a third party can verify independently.

| | Current state | "Good" |
|---|---|---|
| Buy | ❌ must self-source + flash ESP32 | Pre-flashed Canary kit |
| Install | ⚠️ break-glass + trustees are CLI-only | Guided trustee setup in UI |
| Use | ✅ signed hash chain + `log_verify`; ⚠️ export is CLI; no legal-grade bundle | One-tap "export for evidence" → signed bundle + verifier link |

**Acceptance:** a non-developer can set 2-of-3 trustees in a UI; produce a signed export bundle
a court/lawyer can verify with a standalone tool; the export proves no tampering and reveals no
unrelated data.

---

## Persona C — The Chen family, mainstream homeowners (BIGGEST TAM, BIGGEST GAP)

*Want a doorbell/camera that doesn't spy on them and has no monthly fee. Will not touch a
terminal, YAML, or "trustees."* Largest market by far; today SecuraCV cannot serve them.

- **Buy**: expect to buy a finished device in a box from a normal store.
- **Install**: scan a QR code, connect to Wi-Fi in an app, done in 5 minutes.
- **Use**: phone notifications, a simple event timeline, the ability to see a clip when needed.

| | Current state | "Good" |
|---|---|---|
| Buy | ❌ no retail product | Boxed Canary device |
| Install | ❌ requires HA + terminal | App + QR onboarding |
| Use | ❌ no consumer app; "coarse time / no faces" conflicts with expectations | Friendly app; the privacy model explained as a *feature*, not a limitation |

**Tension to resolve (see [07](07-timeline-events-privacy-design.md))**: mainstream buyers
expect facial recognition and exact-second clips. SecuraCV deliberately refuses both. Winning
here means *reframing* "no faces, coarse time, 24-hour memory" as the selling point ("it can't
be used to spy on you, even by us"), not apologizing for it. This persona is a **3–5 year
target**, not a launch target.

---

## Persona D — Dana, the Canary hardware buyer / tinkerer

*Bought (or built) Canary ESP32 devices; wants a small mesh of tamper-aware sensors.*

- **Buy**: pre-flashed Canary Vision / WAP, or DIY from `firmware/`.
- **Install**: power on → provision over BLE/Wi-Fi AP → joins mesh; appears in HA via MQTT.
- **Use**: multi-transport resilience (Wi-Fi/BLE/mesh/Chirp) so a device gets its witness out
  "by any means necessary" even if tampered with; per-device tamper sensors in HA.

| | Current state | "Good" |
|---|---|---|
| Buy | ⚠️ DIY only | Pre-flashed kit option |
| Install | ⚠️ flashing/provisioning is technical | Guided provisioning app |
| Use | ✅ rich tamper + transport sensors already in the HA integration | + co-signed cross-device witnessing |

**Acceptance:** a device joins the mesh and appears in HA within minutes of power-on; tamper
events (enclosure, power loss, SD removal, GPS jamming) raise distinct HA binary sensors.

---

## Persona E — Sam, the small-business / civic operator (UPSIDE, NOT LAUNCH)

*A small shop, co-op, or city pilot that needs accountable cameras without running a surveillance
apparatus.* Cares about being able to *prove* what happened without retaining a searchable
archive of everyone who walked by.

- **Buy**: a handful of devices + a Pi/NUC.
- **Install**: standalone multi-camera mode (currently best via Frigate mode).
- **Use**: tamper-evident logs for insurance/incident response; **non-queryable by design**
  is a compliance and liability *advantage*.

**Acceptance:** multi-camera standalone mode; an export suitable for an insurer/police report;
documentation framing "we cannot mass-search this footage" as a feature.

---

## Cross-cutting friction (applies to most personas today)

1. **Install is developer-grade** (`curl | bash`, manual add-on, ONNX hand-download).
2. **No polished timeline/app UI** — events live as HA sensors.
3. **Break-glass and trustee setup are CLI-only.**
4. **No productized hardware** — Canary is DIY.
5. **v1 not shipped** — undermines "ready to rely on this."

These map directly to the priorities in [06](06-feature-prioritization.md).
