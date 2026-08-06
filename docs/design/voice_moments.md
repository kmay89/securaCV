# The five moments — what this voice is actually for

> **This is a scope document. Its most important content is the list of
> things we will not build.** Read it before adding an intent.

## We are not a general assistant, and trying would lose

There is already a device in that room that does timers, music, and trivia,
and it is better at them than we will ever be. Chasing that is how a
purpose-built product turns into a worse copy of Alexa.

What the usage research says, and it is consistent across every source:

- **Real usage collapses to a handful of commands.** Music, weather,
  quick facts, timers, lists. The long tail of skills is effectively
  unused.
- **Discoverability is the leading cause of abandonment** — Nielsen Norman
  Group finds users give up within the *first few interactions* because
  they don't know what to say. A GUI lets you browse; a voice interface
  gives you nothing to look at.
- **Speech is serial and non-persistent.** You cannot re-read a sentence
  you half-heard. Long answers exceed working memory and get abandoned
  mid-sentence.

So the strategy is not "cover more." It is: **be indispensable in the few
moments where a screen is the wrong answer, and be silent everywhere
else.**

## The five moments

Every intent must earn its place in one of these. If a proposed feature
doesn't fit a moment, that is the answer.

### 1. The 2 a.m. moment — the one that matters most

You are in bed. It's dark. You heard something, or think you did. The
alternative to voice is: sit up, find the phone, unlock it, open an app,
wait for it to load, interpret a dashboard — forty seconds of adrenaline
before you learn anything. **"Hey Canary, is everything OK?" is three
seconds, and you never open your eyes.**

No general assistant can serve this moment, because none of them know your
house's witness log. This is the moment the whole product exists for, and
every design law below is downstream of it.

### 2. The doorway moment

Arms full of groceries, walking in, keys in your teeth. Hands busy and
eyes busy is the classic case where voice genuinely beats a screen.
*"What did I miss?"*

### 3. The bedtime ritual

*"Goodnight."* Closing the loop before sleep — who is on watch, anything
that would stop them. Rituals are the one place a slightly longer answer
is welcome, because the person is not in a hurry and is not alarmed.

### 4. The guest question

Someone in your living room asks, *"wait — is that thing listening to
me?"* There is no screen answer to this. It is a trust moment, and being
able to answer it out loud, honestly, is a feature no competitor can copy
without changing what their product does.

### 5. The passing worry

*"Keep an eye on the litter box for two weeks."* The concern arrives while
you're doing something else. A form loses it; a sentence captures it.
(→ [watches](watches.md))

## What we will not build

Named explicitly, so that "wouldn't it be cool if…" has somewhere to land:

| Not this | Why |
|---|---|
| Timers, alarms, reminders | The room already has these. We would be a worse copy. |
| Music, shopping lists, trivia | Same, and none of it touches what we know. |
| Weather as a destination | It's a garnish on the catch-up, never something to ask us for. Ask the thing that's good at it. |
| Reading long lists aloud | Speech is serial; a twelve-item list is unusable out loud. Summarize and point at the screen. |
| Anything needing comparison or precision | "Which zone had more events on Tuesday" is a dashboard question. Voice should decline gracefully, not answer badly. |
| Multi-turn wizards | Every extra turn is a chance to be misheard and abandoned. One sentence in, one answer out. |
| Actions that reduce what you're told | Contract, not taste (→ [watches](watches.md), the asymmetry). |

## The design laws that follow

These are enforced in code and pinned by tests, not left to taste.

**1. Brevity is a constraint, not a preference.** Voice output vanishes.
The 2 a.m. answer must fit in one breath. Longer answers are permitted
only in the ritual moment, where the person is calm and not in a hurry.

**2. There is a night register.** Between 22:00 and 06:00 the same
question gets a different answer: shorter, calmer, and stripped of small
talk. Nobody wants the weather or a pending-update nag at 2 a.m. — they
want to know whether they can go back to sleep. (The same waking-hours
window the display's audible self-test respects.)

**3. Never enumerate a list past a handful.** Two or three names is
speech; twelve is noise. Past the threshold, summarize and name the
surface that does lists well.

**4. Teach on every failure.** Since not knowing what to say is *the*
abandonment cause, a sentence we can't match must answer with an example,
never a bare "sorry." The help answer is a fallback, not the plan.

**5. One question in, one answer out.** No clarifying dialogs. If the
subject is ambiguous we say what we do know and let the person redirect —
a second turn is a second chance to lose them.

## What that means for what already exists

Applying the laws above to the intents already shipped:

| Intent | Verdict |
|---|---|
| `WhatsUp` | **Core.** Serves moments 1 and 2. Gains the night register. |
| `Privacy` | **Core.** Moment 4, and uncopyable. |
| `Goodnight` | **Core.** Moment 3 — the one place a longer answer is right. |
| `StartWatch` / `ListWatches` | **Core.** Moment 5. |
| `FleetStatus`, `LastEvent`, `OfflineCheck` | **Keep** — they are the precise forms of moment 1, for people who ask precisely. |
| `Roster` | **Keep, capped.** Stops enumerating past a handful. |
| `DeviceCheck` | **Keep.** Real when a specific device is on your mind. |
| `Help` | **Keep, but demoted.** Discoverability is served by teaching in context; a help command is the fallback for people who think to ask. |

Nothing is deleted, because each one is a real phrasing of a real moment —
but the ordering above is the priority when they conflict, and the laws
bind all of them.

## Related

- [Talking to your fleet](../voice_control.md) — the surface
- [Watches](watches.md) — moment 5
- [Whisper local voice](../research/whisper_local_voice.md) — the contract
