# Character — choosing how the glass feels, without choosing wrong

> Status: **spec + wave 1** — companion to
> [`display_ux_design.md`](./display_ux_design.md) ("Quiet Glass" is now the
> default *Character*, not the only one), [`display_settings.md`](./display_settings.md)
> (the picker is one more One-Screen-One-Decision editor), and
> [`display_living_canary.md`](./display_living_canary.md) (a Character also
> sets the bird's *temperament*). Tokens live in
> `include/canary/ui/character.h`; the honesty rules below are enforced
> there, not by convention.

## 1. The job

A wall object lives in someone's home for years. The look should be *theirs* —
warm and roomy for a grandparent's hallway, minimal for a studio apartment,
bright and quick for a teenager's desk. But "here's a color picker" is the
wrong tool: it hands the user a blank canvas and a decision they are not
equipped to make, and most will make it badly (a 3 a.m. cyan-on-magenta
bedroom glow), regret it, and blame the product. The paradox of choice is
real and measured — more options, *lower* satisfaction and more decision
paralysis (Iyengar & Lepper's jam study is the canonical cite; every
watch-face and car-HMI team has re-learned it since).

So the job is not "let them choose colors." It is:

**Offer a small ring of tasteful, pre-validated looks — each a coherent
"age of technology" a person recognizes on sight — and let them flip through
and land on one. Every option is a decision the designer already made
correctly, so there is no wrong answer to reach.**

This is exactly the Apple Watch face model: you do not mix your own face from
raw parts; you swipe a curated gallery of named, art-directed faces, each with
sensible complication defaults, and pick the one that feels like you. The
constraint *is* the feature.

## 2. What a Character is (and is not)

A **Character** is a bundle of four coordinated dimensions:

| Dimension | What it sets | Example (Heirloom vs Neon) |
|---|---|---|
| **Ground & chrome** | background / surface / edge / text tiers, one accent hue | warm charcoal + brass ink vs true-black + electric cyan |
| **Type feel** | which enabled Montserrat size fills each role | one step *up* everywhere vs the compact default |
| **Temperament** | the bird's baseline breath rate, flourish cadence, hop energy | slow & sparing vs quick & springy |
| **Voice** | register of the non-semantic microcopy (§9 wave 2) | "All is well" / "welcome home" vs "All good" / "hey again" |

A Character is **not** a license to change what anything *means*. Two lines
are load-bearing and no Character may cross them:

- **Semantics stay at timeline-card parity.** Green `#43A047` ok/verified,
  amber `#FB8C00` attention, red `#E53935` alert/tamper, blue `#03A9F4`
  signed — these are *signal* colors, identical in every Character, because
  "a state means the same color on the wall as in the app" (G7) and because a
  fire is red for everyone. A Character restyles the *room*; it never repaints
  the *alarms*.
- **Night is sacred, and it belongs to the night engine, not the Character.**
  The red-shifted, melatonin-band-free (446–477 nm) night palette and the
  "soft red / plain" choice live in Settings and outrank every Character.
  A "Neon" glass does not blast blue light at 3 a.m.; at night it is the same
  calm red-shifted floor as every other. Your style is *how it looks by day,
  at rest.*

Holding those two lines is what lets the looks be genuinely different without
ever becoming dishonest or unsafe. It is also, not coincidentally, how Apple
ships loud watch faces alongside Night Shift and a red battery ring that
mean the same thing on all of them.

## 3. Color theory, applied (not decorated)

Each Character is built from the same four rules, so none of them can clash:

1. **60 / 30 / 10.** Ground is ~60% of the glass, surface/edge chrome ~30%,
   the accent ~10%. Semantics sit *outside* this budget — they are signal, not
   decoration, and are allowed to interrupt. Keeping decorative accent to a
   tenth of the field is what makes even the vivid Characters read as calm.
2. **One accent hue, harmonized.** Each Character picks a *single* chrome
   accent and stays monochromatic/analogous around it. The only deliberately
   complementary tension on the glass is the semantic set (amber against a
   cool ground *should* pop — that is the point). No Character introduces a
   second decorative hue to fight the first.
3. **Contrast is measured, not eyeballed.** Body text ≥ 4.5:1 and large text
   ≥ 3:1 against its own ground (WCAG 1.4.3); non-text UI (arcs, spines, the
   accent) ≥ 3:1 (WCAG 1.4.11). Heirloom targets ≥ 7:1 (AAA) because its
   audience skews older (see §4). The palette table in `character.h` carries
   the computed ratios in comments so a future edit can't quietly regress one.
4. **Hue never carries meaning alone.** Unchanged from G10 — severity is also
   position, glyph, and label, so ~8% of men (red-green colorblind) lose
   nothing. Character accents are *decorative only* and never the sole carrier
   of any state, so a livelier palette costs no accessibility.

Two hard constraints from the platform fold in here: every ground stays dark
enough that the bezel disappears and the night floor can go low (the
"Quiet Glass" ground rule), and blue-forward accents (Aqua, Neon) are
**day-only** by construction because the night path swaps them out. A true
light/paper Character — cream ground, ink text — is genuinely different (it
needs the semantic set *darkened within the same hue family* to hold 3:1 on a
light field, plus its own light-safe night story) and is deferred to wave 3
rather than faked now.

## 4. The lessons we took from Apple's watch-face research

Apple has published more on glanceable, personal, at-a-distance faces than
anyone, and the findings map cleanly onto a wall glass:

- **Curated, named, art-directed — not a parts bin.** Faces ship as complete
  looks with opinionated defaults; customization is *within* a face, bounded.
  → Our ring of named Characters; the only free choice inside one is Settings
  (brightness, night), never raw colors.
- **Complication density is a design decision, not the user's burden.**
  Apple's guidance and field research both point the same way: more
  complications *lower* glance speed, and older users do markedly better with
  *fewer, larger* ones. → **Heirloom** deliberately reduces density: the type
  ladder steps up, hit targets grow, and the bird's fidgets thin out. A calmer
  glass is not a lesser glass; it is a *tuned* one.
- **Legibility scales with the person.** Dynamic Type / larger accessibility
  sizes exist because one size does not fit every eye. → The Type-feel
  dimension: Heirloom is the "large print" edition without a separate SKU.
- **Warm at night is a system promise, not a theme.** Night Shift warms every
  face on schedule. → Our red-shift is owned by the night engine and outranks
  Character, exactly so no chosen look can defeat it.
- **Motion is purposeful and interruptible.** Reduce-Motion is a first-class
  setting; animation earns its place or is cut. → Temperament tunes *timing
  and spacing* within the existing motion budget; it never adds a new motion,
  and "night = breath only" / "alarm = bird-free" hold in every Character.

## 5. The bird has a temperament now (the MIT-roboticist layer)

The living canary is already a **truthful gauge**: valence in the eye, arousal
in the body, every pose mapping 1:1 to a nameable system state
([`display_living_canary.md`](./display_living_canary.md)). That honesty
property (from Pwnagotchi) is non-negotiable. What a Character adds is
*temperament* — the same idea social-robotics has used since Breazeal's Kismet
at the MIT Media Lab: a small set of continuous expressive dimensions produces
a readable, consistent "personality" without a combinatorial explosion of
hand-authored states.

Temperament is **three bounded scalars**, applied *under* the mood engine:

| Scalar | Range | What it changes | Disney principle it tunes |
|---|---|---|---|
| `breath_scale` | 0.85–1.25 × | breath half-period (arousal baseline) | slow-in / slow-out |
| `flourish_scale` | 0.7–1.4 × | idle-flourish cadence window | timing |
| `hop_energy` | 0.85–1.25 × | hop amplitude (clamped) | exaggeration, squash/stretch |

Rules that keep it honest and safe:

1. **Temperament changes cadence, never vocabulary.** A worried bird is
   worried in every Character — narrowed eye, half-raised wing. Neon just gets
   there a beat quicker. The *silhouette* (the diagnostic) is invariant.
2. **The mood engine still owns truth.** Scalars multiply timings the mood
   already chose; they cannot promote Idle over Worried, wake a sleeping bird,
   or bring the bird back during a live alarm.
3. **Rarity stays earned.** The trust-gated "song" is still gated by clean
   days, not by Character. A lively temperament flourishes *more often*, not
   *more specially*.
4. **Bounds are clamped in code.** Even the liveliest Character stays inside
   the calm-tech ration — "lively" reads as *charming*, not *frantic*, and the
   abstract geometry keeps us clear of the uncanny valley.

This is the whole personality trick: one honest state machine, four
temperaments layered on top. The bird stays the same *character actor*; the
Character is its *mood on the day you met it.*

## 6. The wave-1 ring (four ages, one honest system)

Curated small on purpose — four is enough to feel personal, few enough to flip
through in five seconds. All are dark-ground (§3), semantics anchored, night
sacred.

| Character | The age | Ground / accent | Type | Temperament | Who it's for |
|---|---|---|---|---|---|
| **Heirloom** | warm machines (mid-century) | warm charcoal `#0B0A08` / brass `#C9A24B`, cream ink `#F3E9D2` | one step **up** | slow & sparing (breath ×1.15, flourish ×1.3) | a grandparent's hallway — big, warm, unhurried, ≥7:1 |
| **Quiet Glass** *(default)* | glass (2010s minimal) | true black `#000000` / signal blue `#03A9F4`, ink `#EDEDED` | today's ladder | measured (×1.0) | the studio, the default — calm, neutral, invisible |
| **Aqua** | the millennium (early-2000s gloss) | blue-black `#050912` / glossy cyan `#38C6FF`, ink `#E8F1F7` | today's ladder | friendly (breath ×0.95) | the optimist — turn-of-the-century shine, still calm |
| **Neon** | now (Gen-Alpha energy) | near-black `#08060C` / electric `#22E0C8`+`#FF4FD8` chrome | compact | quick & springy (breath ×0.9, flourish ×0.75, hop ×1.2) | the desk, the teenager — vivid and alive, still honest |

Default is **Quiet Glass**, and it is byte-for-byte today's look — a user who
never opens the picker sees zero change, and an old settings blob that predates
the field migrates in place: every saved preference kept, only the new field
defaulted (§8).

## 7. The picker (choosing without anxiety)

One more editor on the Settings tree (`display_settings.md` §the tree),
obeying the same five principles. It is deliberately **not** a swatch grid —
a grid invites comparison paralysis. It is a **flip-through**:

```
settings › style
   ┌─────────────────────────┐
   │        Heirloom         │   ‹ the name, big
   │  warm & roomy — easy    │   ‹ one-line era caption
   │        reading          │
   │      ●  ○  ○  ○         │   ‹ where you are in the ring
   │      ‹           ›       │   ‹ flip prev / next
   └─────────────────────────┘
```

- **The screen is the preview** (principle 2), taken literally: flipping to a
  Character *restyles the picker itself, live* — the ground, the text tiers,
  the type scale, and the accent all change under your thumb. You are not
  reading a description of Heirloom; you are *in* Heirloom, deciding whether to
  stay. (The bird's temperament applies at the same instant and is waiting for
  you the moment you step back to the face — it isn't perched on the picker,
  because the glass keeps exactly one live bird and we won't evict the face's.)
- **One decision** (principle 3): which look. Nothing else on the screen.
- **Come home** (principle 4): the ring includes the default, and Settings ›
  reset returns to it; the black-point calibration is never touched.
- **No wrong turn:** every stop on the ring is a validated look, so "just keep
  flipping" always lands somewhere good. Landing *is* choosing — there is no
  Apply button and no confirmation, because there is nothing to get wrong.

The picked Character persists in the same debounced, versioned settings blob
as every other preference; it applies at boot before the first face draws, so
the glass wakes up already wearing it.

## 8. Where it lives (and what stays out of its reach)

- `include/canary/ui/character.h` / `src/ui/character.cpp` — the `Character`
  enum, the palette/type/temperament table (raw `0xRRGGBB` + font-role ladders
  + the three scalars, all pure data), and `character_apply()` /
  `active_character()` / `character_name()` / `character_caption()`.
- `theme.h` / `theme.cpp` — the **single choke point**. The ground and text
  accessors (`col_bg/surface/edge/text/muted/faint`) and the font-role
  accessors read the active Character; a new `col_accent()` exposes its chrome
  hue. `col_ok/warn/alert/signed` and every `ncol_*` stay **constant** — the
  two load-bearing lines from §2, enforced by the type system (they are not
  Character-driven functions).
- `glass_settings.h` / `.cpp` — one `uint8_t character` field; `BLOB_VERSION`
  bumps `1 → 2` **with an explicit v1 → v2 migration**: a v1 blob is
  recognized by its own frozen layout and copied field-for-field, so the
  upgrade keeps every preference a person already tuned (night hours, glow,
  peek) and defaults *only* the new field to Quiet Glass, then rewrites the
  blob as v2 through the normal debounced commit. Reject-to-defaults remains
  the fallback for genuinely unrecognized/corrupt blobs only (review catch:
  a version bump alone would have silently reset every saved preference).
  `sanitize()` clamps out-of-range to the default.
- `canary_mark.*` — `canary_mark_temperament()` stashes the three clamped
  scalars; breath/flourish/hop read them. The mood engine (`bird_mood.h`)
  stays **pure and untouched** — temperament is a rendering concern, not a
  system-state one, so the host tests keep passing.
- `main.cpp` — one `character_apply(settings().character)` after
  `settings_init()`, so boot wears the saved look; the picker re-applies on
  change.

## 9. Roadmap

- **Wave 2 — Voice** *(shipped)*. A per-Character register for the
  *non-semantic* microcopy only — three slots in the Character table
  (`Voice` in `character.h`): the calm-fleet hero word ("All quiet" /
  "All is well" / "All clear" / "All good"), its inline register under a
  time hero, and the returning-boot splash greeting ("hello again" /
  "welcome home" / "welcome back" / "hey again"). The rules that made it
  safe to ship:
  - **Contentment may be rephrased; trouble may not.** Severity words,
    badge text, link labels, event copy, and every degraded-state
    instruction ("waiting for wifi", "no canaries yet • hold to add",
    "Plug in a canary…") are diagnosis and guidance — they never come from
    the Voice table, structurally: the trouble branches in `glance_ui` /
    `dash_ui` don't reference it.
  - **The first meeting stays canonical.** The typed speech-bubble script
    is the same bird for everyone; only the *returning* greeting takes the
    register (and `character_apply` runs before the splash, so the glass
    greets you in its saved voice from the first frame).
  - **The mirror speaks in the wall's voice.** The active calm words ride
    the `/api/glass` snapshot (`aq`/`aql`, static table strings), so the
    phone mirror and the wall can never disagree on register; trouble
    words the mirror still derives from `worst`, from the same invariant
    vocabulary. Older mirror HTML falls back to "All quiet" harmlessly.
  - **Plain ASCII by rule.** The built-in Montserrat tables carry no
    emoji, and the glance contract wants words that read at 3 m.
- **Wave 3 — Light / paper Characters.** A true cream-ground "Almanac" look,
  which requires the semantic set darkened within-family for 3:1 on light and
  a light-safe night story. Deferred, not faked.
- **Wave 4 — More ages, still curated.** Candidates: **Terminal** (phosphor
  green, mono-feel caps — the age of the CRT) and **Blueprint** (drafting
  cyan on ink). The ring grows by *invitation*, never by opening a parts bin.
- **Not planned:** a free color picker. The curated ring is the product
  decision, and re-litigating it would re-import the anxiety we designed out.
