# Canary Poolwatch — pool‑area awareness aid (research & design dossier)

> **⚠️ Read this first. This is an *awareness aid*, not a drowning‑detection or life‑safety device.**
> **"SecuraCV is a supplementary awareness aid that can alert you when someone or something enters
> your pool area — it is not a drowning‑detection or life‑safety device, and it does not replace a
> four‑sided isolation fence with a self‑latching gate, a pool/gate alarm, or constant supervision by
> a responsible adult."** That sentence is load‑bearing and must appear verbatim on the product,
> in‑app onboarding, and any marketing. On this device, overselling is how a child dies instead of
> being saved.

**Status:** concept — sourced research and a design, **no firmware, no bench unit**.

**The one‑sentence version:** a camera that watches a *defined pool zone* and pokes your phone when a
small person enters the pool area or is at the water unattended (and, optionally, when your child's
worn immersion tag goes under) — one extra layer, behind the fence and the grown‑up.

---

## 1 · The doctrine this device must obey (layers of protection)

Every drowning‑prevention authority (CPSC *Pool Safely*, AAP, CDC, NDPA) is unified: **no single
measure prevents drowning; you stack independent layers, and the physical barrier plus human
supervision are primary — everything electronic is supplementary.** The stakes set the honesty bar:

- **Drowning is the #1 cause of death for children ages 1–4**, and it's **silent and 20–60 seconds
  fast** — no splashing, no scream. That's *why* no alert can promise rescue‑in‑time, and why the
  useful moment is the **approach/entry**, before immersion.
- **The proven primary layer is a 4‑sided isolation fence (≥4 ft, gaps <4 in, self‑closing/
  self‑latching gate) — ~83% risk reduction.** Then a designated adult "Water Watcher." Then gate/
  door alarms. Poolwatch is a *fourth‑layer* awareness aid that sits **behind** all of those and
  **prevents nothing physically**.

If our copy ever implies a parent can trust Poolwatch *instead of* a fence, we've made a child measurably
less safe. Non‑negotiable.

---

## 2 · What's honestly achievable (and what isn't)

**Not achievable on a consumer above‑water camera: "drowning detection."** Drowning is subtle, often
near‑motionless and underwater; even commercial camera+sonar systems (Lynxight, AngelEye, SwimEye)
only approximate it under calibration and position themselves as **lifeguard‑assist**. The closest
consumer product (Coral Manta, ~$1,999, underwater) only claims "unmoving body underwater for ~15 s"
— and still won't call itself a substitute for supervision. That's the category ceiling, far above a
XIAO node. **We do not compete on drowning detection.**

**Achievable and honest: a pool‑zone *entry / edge‑presence* alert** — *"a person (ideally flagged
child‑sized) has entered the pool zone or is at the water's edge unattended."* Same class of problem
as consumer person‑detection, and it maps to the real prevention goal: catch the unsupervised approach
early. That's the layer that can actually help.

---

## 3 · The sensor design — Pro tier + an owned immersion tag

**Yes, this is a "Pro tier" job — because of the night.** The defining pediatric scenario is a
**child wandering out at night/dawn** to an unsupervised pool while adults sleep — exactly when
supervision is weakest. A daytime‑only camera is blind when an alert is worth the most, so
[Canary Vision Pro](./canary_vision_pro_recamera.md)'s **starlight low‑light** sensing is the single
most important capability here. (Grove Vision AI V2 is a fine low‑power daytime/wildlife detector at a
narrow gate, but its small sensor + short range + weak low light make it a *supporting* sensor, not
the primary pool‑safety camera.)

**Split the problem by what each sensor can reliably do — the strongest honest design:**

- **Camera = the "approach/entry" layer.** Person detection in a defined pool zone, child‑size bias,
  works for anyone in frame (a visiting child too), and doubles for wildlife (§5).
- **Owned BLE immersion tag = the "in‑water" layer.** A Safety‑Turtle‑style submersion wristband/
  collar the *specific* child or pet wears gives a **high‑confidence in‑water event** the camera
  physically can't match — an owned tag, the same posture as Guardian/Paw. **Its honest limit:** it
  protects *only while worn* (a 2‑year‑old can remove it; a visiting child isn't wearing one) — which
  is exactly why the camera zone‑alert (wearable‑independent) and, above all, the fence still matter.
- **Radar = a presence‑fusion corroborator** — confirms a *real body* in the zone vs. a
  reflection/float, works in the dark; but it can't tell child from pet from goose, so it's never the
  primary.

---

## 4 · False‑alarm mitigation (build it in from day one — assume the raw node is noisy)

Legacy motion runs 80%+ false positives; even tuned analytics only reach <5% *after* these, and a
life‑safety alert must bias toward sensitivity (a missed child is the catastrophic error):

1. **Defined pool zone / polygon mask** — reason only about the water + immediate apron.
2. **Person‑vs‑animal‑vs‑object class + child‑vs‑adult size bias** — so pets, geese, floats, and
   robot cleaners don't fire the *child* alert (a *bias*, never a hard gate that could suppress a real
   child).
3. **Dwell / persistence gating** — a tracked object across frames, killing glare/ripple/shadow blips.
4. **Multi‑sensor fusion** — corroborate camera with radar and/or the BLE tag before escalating an
   in‑water alert; single‑sensor = a lower‑severity "heads up."
5. **Starlight low‑light path** so night degrades gracefully instead of going blind.
6. **Anti‑glare placement** — aim away from direct sun reflection off the water (a top false source).
7. **Tiered alerts** — "someone entered the pool zone" (act now) vs. "wildlife in pool"
   (informational), so a goose never desensitizes you to a child.

Known false sources to expect: sun glare, ripples/reflections, floats/toys/cleaners, pets/wildlife,
moving shadows, and night on a non‑starlight camera.

---

## 5 · The geese/wildlife angle — the honest, low‑stakes bonus

"An animal is in/at the pool" needs only a **coarse animal class** — well within a Grove AI V2 / edge
model's reach, and it carries **no life‑safety burden** (a missed goose harms no one). Two roles: a
plain, safe‑to‑market **"geese in the pool" convenience** feature, and a **fusion corroborator** (the
animal‑vs‑child class keeps a pet/goose from firing the child alert). Keep the copy honest about the
asymmetry — the wildlife feature is a convenience; it must not imply the child‑safety path is equally
"solved."

---

## 6 · Claim mapping, privacy & alerts

- **Pool‑zone entry / edge presence → `PresenceInRestrictedZone`** (or `SmallObjectBoundaryCrossing`),
  child‑size as a confidence‑weighted attribute; **geese → `SmallObjectBoundaryCrossing`**;
  **immersion‑tag event** rides the owned‑tag presence path. **No new claim vocabulary.**
- **On‑device inference, coarse claim out — never raw video** (the Vision rule); the time‑critical
  warning rides the [alert relay](../design/alert_relay.md) with **tiered severity**.
- Privacy: a camera at a pool is pointed at your own family; same discipline as every Canary camera —
  inference in, coarse claim out, no frames in the log; the immersion tag is one you own.

---

## 7 · Never let it rot & open items

- **Reuses Vision Pro (starlight) + Ranger‑class radar (fusion) + an owned BLE tag + the alert relay
  + the cold‑battery/solar rule** — nothing bespoke in the kernel; no new vocabulary.
- **No bench unit.** Child‑size classification (children are the *hardest* class — see
  [Canary Curbwatch](./canary_curbwatch_research.md)), the zone/dwell tuning, and the day/night
  false‑alarm floor all need real‑pool tuning + golden vectors.
- **The positioning line is part of the product**, enforced in copy review — never "drowning
  detection / prevention," "protects your child," "peace of mind," or a hero image of the camera
  watching *instead of* a parent. Mirror the ASTM/PoolGuard disclaimer language; **do not cite ASTM
  F2208 / UL 2017 unless we actually certify** (implying an unheld standard is false and dangerous).
- **The wearable's worn‑only limit** and the **latency chain** (sense → classify → alert → an adult
  who must notice and act in seconds) are stated plainly, not hidden.

---

*Sources: CPSC *Pool Safely* / In‑Home Drowning Safety; AAP *Prevention of Drowning* policy + technical
report; CDC drowning facts + the 4‑sided‑fence ~83% figure; PoolGuard PGRM‑2 (ASTM F2208) and its
"not a life‑saving system… supplement active supervision" disclaimer; Safety Turtle 2.0 wearable and
its worn‑only caveat; Lynxight / AngelEye / SwimEye / Coral Manta (commercial lifeguard‑assist ceiling);
Grove Vision AI V2 specs; AI‑camera false‑alarm literature. Full URLs in the concept card's `sources`
block; shared parts/rules from [`canary_vision_pro_recamera.md`](./canary_vision_pro_recamera.md),
[`canary_ranger_research.md`](./canary_ranger_research.md), and [`../design/alert_relay.md`](../design/alert_relay.md).*
