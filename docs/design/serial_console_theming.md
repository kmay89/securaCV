# Themed serial console — a title block that proves identity, not just looks good

Design document. Status: **Phase 1 implemented** — the host-tested scene engine
(`firmware/common/ui/`), the drunken-bishop randomart, the `l` identity banner,
and the terminal capability probe ship now. The animated "wake" (below) is
designed and gated for a follow-up.

## Purpose first (this is not for fun)

A serial console is the first thing a builder sees when they plug a Canary into
a laptop. For a **witness device**, that first impression should do a job:
convince a human that *this is the device they think it is, running the firmware
they think it is* — and look like the work of people who sweat the details.

So the centerpiece isn't a mascot. It's a **trust card**: a framed identity the
operator can verify by eye.

```
+- SecuraCV  -  Canary witness ----------------------+
|                         ,_,                        |
|                       (o.o)                        |
|                       /) )\                        |
|                                                    |
| Device    canary-7fA3                              |
| Firmware  2.2.0 (a1b2c3d)   built 2026-07-21       |
| Chain     3f9ac10b  seq 41  boots 12               |
| Health    100%  nominal                            |
| Key FP    9b2cff0144a07e33                         |
+- public-key randomart  (eyeball this) -------------+
|                +-----------------+                 |
|                |      ...        |                 |
|                |     . .         |                 |
|                |    .   .        |                 |
|                |   .   .         |                 |
|                |    . E S        |                 |
|                |     . + +       |                 |
|                |      . + .      |                 |
|                |       o   .     |                 |
|                |        ...      |                 |
|                +-----------------+                 |
|                                                    |
| Memorize this shape - it changes if the key does.  |
+----------------------------------------------------+
```

The **randomart** is the load-bearing part. It's the OpenSSH "drunken bishop"
walk (`ssh-keygen -lv`, `sshkey.c fingerprint_randomart`) run over the device's
public key: 32 unmemorable bytes become one small, stable picture. A human
memorises the shape once; a swapped board or a tampered key draws a visibly
different picture. Verifiable identity you can check with your eyes, needing no
tool — exactly the property a witness device should make effortless. On a color
terminal the same card lights up (brand-blue frame, moss/amber health), but the
color is a bonus: **every state that gets a color also gets a word** (WCAG
1.4.1 / the ambient-display house rule), and the whole card is fully legible in
plain text.

## The hard part: robust, never fragile

Naïvely printing ANSI to a serial console is how you get `^[[38;2;..m` garbage on
the wrong terminal. Over a bare serial link you can't read `terminfo`, so the
engine follows a strict **capability ladder** and a probe:

- **Tier 0 — the ASCII floor (default).** 7-bit ASCII only, borders from `+ - |`,
  no color, no cursor control, `\r\n` line ends. This is what we emit when we
  know nothing. It is *authored to be genuinely nice on its own* — animation and
  color are never required to convey the trust info.
- **Tier 1 — confirmed ANSI + Unicode.** Box-drawing `─│┌┐└┘`, 256-color, only
  after the terminal proves itself.

**The probe** (`console_probe`, firmware): emit a cursor-position report request
`ESC[6n`; if the terminal answers `ESC[row;colR` within ~200 ms it speaks ANSI →
Tier 1. Silence → stay Tier 0. We never emit an escape we didn't confirm.

Why this is not optional: **our own browser flasher would distrust us otherwise.**
The flasher's serial monitor (`canary-local/assets/flash-core.js`,
`looksLikeGarbage`) counts control bytes (ESC = `0x1B` included); >20 % and it
declares the stream "wrong baud". An unconfirmed ANSI banner would make our own
tool think the Canary is misconfigured. Default-ASCII is a correctness
requirement, not a preference — and it's **host-tested**: the ASCII tier is
proven to contain zero escape bytes and zero non-7-bit bytes.

### The robustness checklist (enforced or followed)

- [x] Default to Tier 0; escalate only on a positive `ESC[6n` reply. *(host-tested: no ESC / no byte ≥ 0x80 in ASCII tier)*
- [x] Capability probe is bounded (~200 ms) and non-blocking; "no reply" = "no ANSI".
- [x] Assume 80 columns (the card is 54 wide, fits comfortably); never draw wider than known width.
- [x] Every framed line aligns to the same width — content is ASCII so byte length == column width; only borders differ per tier. *(host-tested)*
- [x] CRLF line endings (raw PuTTY / screen / minicom don't translate LF).
- [x] Always `ESC[0m` after a colored run; never leave the cursor hidden (the static card never emits `ESC[?25l`). *(host-tested)*
- [x] Color never carries meaning alone — health/tamper always carry a word. *(host-tested)*
- [x] The emitter only ever produces a fixed whitelist of sequences; it never echoes untrusted bytes inside an escape (ANSI-injection hygiene).
- [x] Feature-gated (`FEATURE_CONSOLE_THEME`) for flash budget; pure header composition, no heap.

## Architecture

A pure, host-testable engine with a thin serial adapter — the same shape as
`common/health/test_console.h`.

| File | Role |
|------|------|
| `firmware/common/ui/randomart.h` | The drunken-bishop walk (pure, deterministic, bounded). Hash-agnostic; we feed it the public key. |
| `firmware/common/ui/console_theme.h` | `Caps` (the capability tier) + `Renderer` (color/border primitives, gated) + panel primitives (`hrule`, `row`, `center_into`). |
| `firmware/common/ui/console_scenes.h` | The `trust_card` scene, composed from the above. |
| `firmware/common/ui/console_wake.h` | The animated-wake frames + gated cursor-control helpers (the `a` command). |
| `firmware/tests_host/test_console_scene.cpp` | Proves the randomart walk and the ASCII-tier safety + alignment invariants. |
| `firmware/canary/src/main.cpp` | The `l` command: `console_probe()` → build `TrustInfo` from `witness_get_device()` → `trust_card`. |

The engine writes through an `emit_fn` sink — `Serial.print` in firmware, a
string collector in tests — so all composition is pure and unit-tested.

### The randomart walk (our convention)

Faithful to OpenSSH: a 17×9 field of visit counters; start at center; for each
byte, 4 steps of 2 bits (bit 0 = horizontal, bit 1 = vertical), clamping at the
walls (which is what makes coins pile along edges); glyph ramp
`" .o+=*BOX@%&#/^SE"` with `S`/`E` the start/end markers. OpenSSH computes its art
over the **MD5** raw digest; we run the identical walk over the **device public
key** bytes and document that as our fixed convention (the algorithm is
hash-agnostic given a fixed input string).

## Phase 2 (implemented): the animated wake — `a`

The animation with a job. The `a` command runs the real self-test and reveals
its ten probes one at a time — `[..] -> [OK]` / `[!!]` with each probe's real
name + duration — then settles into the identity card. The animation **is** the
health check, not filler.

```
+- self-test  (running) -----------------------------+       +- self-test  (complete) ----------------+
| [OK] NVS read/write                           12ms |       | [OK] NVS read/write               12ms |
| [OK] Device keys                              40ms |  ...  | [!!] SD card                       5ms |
| [..] SD card                                       |  -->  | [OK] Wi-Fi radio                   8ms |
| [~~] Wi-Fi radio                                   |       |  ...                                   |
| Health    checking...   4/10 reported              |       | Health    90%   9/10 probes passed     |
+----------------------------------------------------+       +----------------------------------------+
```

Per the Flipper Zero lesson — its serial banner is *static and robust*; the
animation lives on the LCD — the serial default stays static, and the reveal is
a **Tier-1, skippable bonus** that degrades cleanly:

- **Confirmed ANSI:** the fixed-height block repaints in place after each probe
  (`ESC[nA` move-up + overwrite; every row is a fixed width so nothing needs
  clearing and nothing tears — research strategy #1, well inside the ~11.5 KB/s
  budget at 115200). Cursor hidden during the reveal, always restored.
- **ASCII floor:** no cursor control, so it reveals all results and prints the
  finished checklist **once** — same content, zero escapes.
- **Press any key to skip:** any RX byte reveals the rest and jumps to the final
  frame; the sequence always converges to the same static end-state, so a
  scrollback capture is clean.
- The running frame **never shows the final score** (only "N/10 reported"), so
  the reveal isn't spoiled — the score appears only on the completed frame.

Pure composition in `firmware/common/ui/console_wake.h`; timing / skip /
`diag_run_selftest()` live in `main.cpp:run_wake()`. Host-tested: ASCII tier
escape-free + width-aligned, cursor control emitted only at the ANSI tier,
markers carry words (`OK`/`!!`, never color alone), and the running frame hides
the score.

## Open-source lineage / licenses

Ideas borrowed, nothing copied: drunken-bishop from **OpenSSH** (BSD); the
diff-redraw model from **notcurses** / **tcell** (Apache-2.0); big-text banners,
if added, precomputed offline with **FIGlet** (BSD) and embedded as `const char[]`
rather than shipping the renderer. The half-block image trick (`▀` with fg=top,
bg=bottom) from **chafa** (LGPLv3) is reimplemented, not linked.
