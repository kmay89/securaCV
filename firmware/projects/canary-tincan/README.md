# canary-tincan — the Tin Can

A kids' wrist Canary. Two watches **tie a string** — a parent-witnessed,
LAN-only pairing — and after that they can *knock*, *tug*, *stamp* and *doodle*
at each other, with no voice, no text, no location and no cloud. A parent gets
exactly one privileged message, **the Ring** ("come inside"), which is honest
about whether it arrived.

- **Design:** [`docs/design/canary_tincan_kids_watch.md`](../../../docs/design/canary_tincan_kids_watch.md)
- **Board:** [`waveshare-esp32s3-amoled206`](../../boards/waveshare-esp32s3-amoled206/README.md)
- **Status:** **Phase 0.** The pure cores are written and **host-tested in CI**.
  There is no PlatformIO environment and no runtime yet — see "What exists"
  below, and the honesty rule that governs it.

## The one idea

**The Tin Can carries no speech.** That single refusal is what makes it fun
(kids invent their own knock codes), what makes it cheap to defend (there is no
content to moderate, record, or leak), and what keeps it inside
[Invariant I](../../../spec/invariants.md) instead of asking for the carve-out
the [LAN baby monitor](../../../docs/design/lan_baby_monitor.md) would have
needed. What the string carries instead is *rhythm, pressure and pictograms* —
and a rhythm is not language.

## What exists

Everything below builds and runs with plain `g++`, with no board attached:

```
firmware/common/link/                 the generic transport — NOT kid-specific
  link_frame.h                        frame layout, discrimination, AEAD nonce
  link_replay.h                       anti-replay window + non-rewinding counter
  link_session.h                      roles, directional key labels, revocation
  link_node_table.h                   many-node presence, liveness, routes
  link_relay.h                        blind forwarding: dedupe, ttl, rewrite

firmware/projects/canary-tincan/include/canary/tincan/
  knock_codec.h                       tap a rhythm, feel it on the far wrist
  string_model.h                      taut / slack / cut, and the tug
  ring_policy.h                       the parent's one message + delivery truth
  tie_ceremony.h                      the parent window, the knot, the confirm
  stamp_set.h                         the sixteen stamps
  warmer_colder.h                     hide and seek off the household Canaries
  duel_model.h                        the step duel

firmware/projects/canary-tincan/tests_host/
  test_link_core.cpp                  18 groups
  test_tincan_cores.cpp               29 groups
```

```sh
cd firmware/projects/canary-tincan/tests_host && make
```

## Why the cores come first

The board is not on anyone's desk yet — but the parts of this design that are
easy to get *catastrophically* wrong do not need one. Nonce uniqueness across
both directions of a string, a send counter that must never rewind after an
unclean reboot, a revocation that must never quietly un-revoke, a Ring that
must never claim it was delivered when it wasn't: every one of those is
decidable in a host test, and every one of them is decided before a wrist wears
one.

The tests are written to fail loudly on the *specific* mistakes, not to
exercise the happy path. `test_nonce_directions_never_collide` is the clearest
example: two peers sharing a session and each counting from 1 must never
produce the same nonce, because that one mistake destroys both confidentiality
and authenticity under ChaCha20-Poly1305 and AES-GCM alike.

## Why the transport is a separate, generic layer

`firmware/common/link/` knows nothing about children. Its payload `kind` is an
opaque byte. That is on purpose: **many nodes, one radio, no broker, no cloud,
and messages that survive the router going down** is a general problem, and the
Tin Can is the smallest honest place to solve it. An industrial deployment
wanting authenticated peer-to-peer datagrams across a site gets the same
frame, the same replay window, the same session model and the same blind relay
— and a different payload layer on top.

Two properties that make the relay safe to run anywhere:

- **Relays are blind.** A forwarded frame is never decrypted, inspected or
  logged. A household Canary — a device with a camera in it — can carry a
  child's traffic without being able to see any of it.
- **`hops`/`ttl` are untrusted.** They sit outside the AEAD so a relay can
  rewrite them, which means anyone can. They are used for loop prevention and
  nothing else — never to decide trust, never to infer distance.

## What does not exist yet

No PlatformIO environment, no `src/`, no display driver, no radio glue. The
board is registered at **compile-tested** tier with `used_by: []` — the same
Phase-0 posture as `xiao-esp32c6-sentinel` — because this tree honours the
repo's rule that nothing is claimed as working until it has been built and run.
Adding a build env before there is a runtime to build would put a green tick
next to something that does not exist.

Next, in order:

1. **Bring-up.** Confirm the touch controller is really an FT3168 (the store
   copy says CST9220 and the vendor code disagrees), get LVGL onto the CO5300
   at 410 × 502, and put a **DRV2605L + LRA** on the I²C port. The knock is the
   product; a mushy knock is a dead product, so bench LRA against ERM before
   anything else is built on top.
2. **The radio.** A second ESP-NOW frame type alongside the fleet presence
   beacon, plus the transmit path — the existing peer path is receive-only by
   stated design and accepts only an exact 11-byte beacon, so this is new work,
   not reuse.
3. **One string.** Tie ceremony, knock and tug between two watches, taut/slack
   drawn honestly.
4. **The Ring**, with the three delivery states visible end to end.

## Before anyone calls this a kids' product

This ships as a **maker kit for the builder's own household**. Before the word
"kids" appears on a store listing it needs CPSIA third-party testing and ASTM
F963, a child-resistant battery enclosure in the spirit of Reese's Law, a
breakaway strap, and FCC/CE. A bare Li-po strapped to a child's wrist is the
largest physical risk in the whole design and no firmware decision touches it.
Full argument in
[the design doc §3.3](../../../docs/design/canary_tincan_kids_watch.md).
