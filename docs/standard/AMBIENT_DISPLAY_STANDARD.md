# The Open Ambient Security Display Standard — draft v0.1

> Status: **DRAFT** (trailblazer spec §10). Reference implementation:
> [`firmware/projects/canary-display`](../../firmware/projects/canary-display/)
> (Apache-2.0, same as the repository). Feedback via issues/PRs.

## 0. Why a standard

Every consumer security product renders status somewhere — and nearly all
of them get the same five things wrong: silence reads as safety, color is
the only carrier, trust is asserted rather than proven, night means either
blinding or blind, and the alert channel dies with the cloud. This
document distills what a *trustworthy ambient security display* must do,
independent of vendor, panel, or toolkit — so anyone can build one, on any
hardware, that a household can actually rely on.

A conforming display can be a 240×240 puck, an e-ink tile, a wall panel,
or a repurposed retail gadget. The standard constrains **behavior**, not
pixels.

## 1. Conformance levels

- **AD-Core** — the trust and honesty invariants (§2). Mandatory.
- **AD-Calm** — the attention/light/sound discipline (§3).
- **AD-Resilient** — the failure ladder (§4).
- **AD-Verified** — cryptographic display-side verification (§5).

A product may claim each level independently ("AD-Core + AD-Calm").

## 2. AD-Core — honesty invariants

1. **Silence ≠ safety.** A witness that stops reporting MUST visibly
   degrade on deadlines (reference: amber ≤ 3 min, red ≤ 10 min), and a
   dead transport MUST be bannered on the glass within one render cycle.
   Last-known state MUST be labeled as such.
2. **Color never carries meaning alone** (WCAG 2.1 SC 1.4.1): every state
   also has a label, glyph, or position.
3. **Acknowledge ≠ amnesia.** An ack quiets emphasis; a residual indicator
   persists until the condition clears. Tamper-grade conditions can be
   quieted, never dismissed. A NEW alert-grade condition MUST cancel a
   standing ack.
4. **Attention decays; alarms don't.** Informational events age out on
   their own (reference: 10/30 min); alert/tamper hold until acknowledged.
5. **No overclaiming.** Words like "verified" MUST be backed by an actual
   check the display performed (see §5); otherwise use weaker vocabulary
   ("signed", "reported").

## 3. AD-Calm — attention, light, sound

1. **Glance budget:** worst-state readable in ≤ 1 s at room distance
   without reading text.
2. **Motion budget:** every animation is enumerated with a job; the
   surface at rest is still. (Reference budget: page transition, alert
   breathing while unacked, ack-progress affordance, and at most one
   earned "all-well" pulse per minute.)
3. **Night floor:** a bedroom-safe mode ≤ 10 lux at the pillow,
   red-shifted (no 460–500 nm), with ONLY unacknowledged alert-grade
   conditions allowed to override it.
4. **Sound grammar** (if any sound): distinct patterns per severity tier
   (a fault must never sound like an intruder), a falling all-clear so
   silence keeps meaning "nothing new", and only the highest tier may
   break the night floor.
5. **No ads. No engagement mechanics. Ever.** An ambient security surface
   that competes for attention has failed.

## 4. AD-Resilient — the failure ladder

For each of: WiFi loss, broker/hub loss, hub *moved* (address change),
power cycle, and witness death — the display MUST have a defined, honest
behavior and automatic recovery. Reference behaviors: exponential-backoff
reconnect; retained-state repopulation on reconnect; mDNS re-discovery of
a moved hub; and (optional but strongly recommended) a transport-free
local fallback for alert-grade events (reference: passive BLE chirp scan).

## 5. AD-Verified — proof on glass

1. The display maintains its own trust store (trust-on-first-use pinning
   of witness public keys, persisted locally) and verifies signatures
   **on its own silicon** — it never outsources the word "verified".
2. Key mismatch for a pinned identity is surfaced loudly and never
   silently re-pinned.
3. **Proof is exportable**: the display can present the verbatim signed
   material plus the pinned public key in a machine-readable form (QR)
   such that a third party can verify independently with commodity tools —
   no vendor account, app, or cloud.
4. Proof never goes stale silently: if current material can't be proven,
   the display says so rather than showing older proof as current.

## 6. Reference wire vocabulary (informative)

The reference implementation consumes MQTT topics
`<root>/<device>/{status,availability,health,events,tamper,chain,state,meta}`
with Ed25519 signatures over locked canonical strings
(`securacv-canary-sig|v1|...` — see `docs/ble_protocol.md` and
`custom_components/securacv/signature.py`), retained LWTs for liveness,
retained `meta` `{"name","room"}` for human naming, a shared retained
`<root>/fleet/ack` for household acknowledgement, and 17-byte BLE
manufacturer-data chirps as the off-grid channel. Other ecosystems MAY map
their own vocabularies onto the behaviors above; the invariants, not the
topics, are the standard.

## 7. Conformance checklist (self-assessment)

| # | Level | Check |
|---|---|---|
| 1 | Core | Silent witness degrades on deadlines; transport loss bannered |
| 2 | Core | Last-known state labeled |
| 3 | Core | No color-only state |
| 4 | Core | Ack leaves residual; tamper undismissable; new alert cancels ack |
| 5 | Core | "Verified" only when locally checked |
| 6 | Calm | ≤ 1 s glance; enumerated motion budget |
| 7 | Calm | ≤ 10 lux red-shifted night floor; alert-only override |
| 8 | Calm | Severity-distinct sounds + falling all-clear (if sound) |
| 9 | Calm | Zero ads/engagement mechanics |
| 10 | Resilient | Defined behavior + auto-recovery for all five failures |
| 11 | Resilient | Off-transport alert fallback (recommended) |
| 12 | Verified | Local TOFU store + on-device signature verification |
| 13 | Verified | Exportable, independently-checkable proof |
| 14 | Verified | Stale proof never presented as current |

## 8. Porting guide (informative)

The reference fleet model
(`firmware/projects/canary-display/include/canary/fleet/fleet_model.h`) is
dependency-free C++ (time injected, host-testable) and encodes invariants
2.1–2.4 directly — ports to other toolkits (ESPHome/LVGL boards, e-ink,
LED matrices) can reuse it verbatim and re-skin only the render layer.
The "Quiet Glass" design tokens and motion budget are documented in
[`display_ux_design.md`](../hardware/display_ux_design.md) §Design language.
