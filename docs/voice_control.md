# Talking to your fleet — local voice on the hub

How to ask "is the fleet OK?" out loud and get an honest answer, with every
stage — wake word, speech-to-text, the answer itself, text-to-speech —
running on your own Raspberry Pi hub. No cloud, no account, no subscription,
and no audio leaving the house.

This page is the worked recipe for the design in
[Whisper local voice](research/whisper_local_voice.md). Read that page's §3.1
if you want the full contract; the short version binds everything below:

1. **Voice input comes from a dedicated voice satellite — never a Canary.**
   Canary microphones (where they exist at all) reduce sound to single
   numbers and structurally cannot ship speech anywhere. That stays true.
2. **Command audio and transcripts are transient** — parsed for an intent,
   then gone. Never journaled, sealed, or exported.
3. **Everything is local.** The pipeline below is Home Assistant's own
   Wyoming stack running as add-ons on the hub.
4. **Voice may ask; it may not act.** The SecuraCV intents are read-only by
   construction — there is no sentence that arms, disarms, mutes, or unseals
   anything, because a spoken word carries no signature.
5. **No voice profiles, no "respond only to me."** That would be speaker
   recognition, which this project never implements.

## What you need

- The hub: Home Assistant OS on a Raspberry Pi (the
  [full-stack path](full_stack_setup.md)), with the SecuraCV integration
  installed.
- A voice satellite — pick one:
  - **Your phone** (the Home Assistant companion app's Assist button) —
    push-to-talk, nothing to buy, and the default this page assumes.
  - **A dedicated satellite** (Home Assistant Voice Preview Edition, or an
    ESPHome voice satellite you build) — needed only if you want a
    hands-free wake word in a room. Buying hardware? See
    [the microphone short list](#which-microphone--the-short-list) below.
  - **Never a Canary.** No firmware path ships audio samples off a Canary,
    and none will be written for this.

## The pipeline

```
wake word (openWakeWord)  ─►  STT (Whisper)  ─►  SecuraCV intent  ─►  answer (Piper)
   on the satellite/hub        on the hub          on the hub          on the hub
```

## Set it up — one command, then two clicks

The wizard does the repetitive part for you. Open the hub's terminal
(**Settings → Add-ons → Terminal & SSH**) and run:

```sh
wget -q https://raw.githubusercontent.com/kmay89/securaCV/main/tools/hub_voice_setup.sh
bash hub_voice_setup.sh
```

It installs and starts the three local voice engines (Whisper, Piper,
openWakeWord), adds the Assist Satellite runtime, installs the fleet
sentences, and places the "Hey Canary" wake-word model if you have one —
telling you, line by line, exactly what it did and what it skipped. Every
step checks before it acts, so **re-running is always safe**, and
`bash hub_voice_setup.sh verify` reports state without changing anything —
drop it in a cron if you want the setup to check itself.

Honesty about what a script can't do: the last two steps are choices made
with your eyes, so the wizard ends by pointing at them —

1. **Settings → Voice assistants → Add assistant** — pick Whisper for
   speech-to-text, Piper for text-to-speech, and your wake word.
2. Plug the microphone in and select this assistant for it.

Then say, out loud: *"Is the fleet OK?"*

The wizard is generated from the same sentences file this page links
([`voice_sentences_en.yaml`](voice_sentences_en.yaml)) and CI byte-checks
it, so the script and this page cannot tell you two different stories.

<details>
<summary><strong>By hand — every move the wizard makes, spelled out</strong>
(the trust here is transparency, so nothing above is magic)</summary>

### 1. Install the three add-ons

**Settings → Add-ons → Add-on Store**, install and start:

- **Whisper** — speech-to-text (the `faster-whisper` engine).
- **Piper** — text-to-speech.
- **openWakeWord** — wake-word detection (only needed for hands-free use;
  skip it for push-to-talk).

Home Assistant discovers each one as a **Wyoming** integration —
**Settings → Devices & Services** will prompt; accept all.

On model size: the Whisper add-on's model is a dropdown. Start with the
smallest (`tiny`/`tiny-int8`) and move up only if your hub keeps pace —
which size is *usable* on your Pi is something you find out on your Pi,
not something this page can promise.

### 2. Create the local assistant

**Settings → Voice assistants → Add assistant**:

- **Conversation agent:** Home Assistant (the built-in, local agent — this
  is what routes sentences to the SecuraCV intents).
- **Speech-to-text:** the Whisper add-on.
- **Text-to-speech:** Piper.
- **Wake word:** leave off for push-to-talk; see §4 to opt in.

### 3. Teach it the fleet sentences

Copy [`voice_sentences_en.yaml`](voice_sentences_en.yaml) from this repo to
the hub as `/config/custom_sentences/en/securacv.yaml` (create the folders
if they don't exist — the **File editor** or **Samba** add-on both work),
then restart Home Assistant.

</details>

## How it feels to use

Press the Assist button — or say the wake word — and ask:

| You say | It answers with |
|---|---|
| "What's up?" | The casual catch-up, one honest breath: an alarm or tamper leads if there is one, then anything needing attention, the latest activity (or "all quiet"), fleet health, the weather outside, and anything waiting on you — like pending hub updates. |
| "Is the fleet OK?" | Device count, signature-trust summary, kernel reachability — worst news first. |
| "What was the last witness event?" | The newest event's coarse label, a ten-minute-floor relative time, and its trust status — an unsigned or key-mismatched publish is named out loud, never spoken as the plain truth. |

The weather line at the end of "what's up" covers Home Assistant's whole
condition vocabulary with a warm phrase for every season — optimistic on
purpose, with one honesty override (severe weather is flagged, never
charmed):

> *"Outside it's 28 degrees and snowing — it'll be pretty out there."*
> *"Outside it's 55 degrees and rainy — the garden will be glad."*
> *"Outside it's 84 degrees and sunny — a good one to step out in."*
> *"Outside it's 48 degrees and clear — good stars if you look up."*

The answers keep the project's vocabulary discipline out loud:
**"verified" is spoken only for a device whose Ed25519 signature checked
against its pinned key.** A device the hub has merely received MQTT from is
"heard" — the same honest ladder the dashboards use. A key mismatch leads
the answer, because that is the one thing you'd want interrupted first.

## Your wake word: "Hey Canary"

The fleet's device is the Canary, and you talk to a thing by saying its
name — so the brand wake word is **"Hey Canary."** It's a good wake word on
the merits, not just the branding: four syllables, distinctive phonemes,
and a word that almost never occurs in ordinary conversation, which is what
keeps false wakes rare. Saying the name is also the consent gesture made
audible — nothing is transcribed until you address the device, the same way
nothing is watched by design everywhere else.

**Honest status: you mint this one yourself, today.** No pre-trained
"Hey Canary" model exists yet in the community wake-word collection, and
the openWakeWord add-on ships with well-tested built-ins (e.g. "Okay
Nabu") that work now. To mint the brand wake word:

1. Train a model with the [openWakeWord training notebook](https://www.home-assistant.io/voice_control/create_wake_word/)
   (Home Assistant's guide walks the whole Colab) using the phrase
   "hey canary" — it needs no ML background and produces one file:
   `hey_canary.tflite`.
2. Drop that file in `/share/openwakeword/` on the hub — or next to
   `hub_voice_setup.sh` and re-run it, and the wizard places it for you.
3. Pick `hey_canary` in your assistant's wake word menu.

Until then, use a built-in and rename nothing — a wake word that works
beats a brand that doesn't.

### What turning a wake word on means

Push-to-talk needs no fine print: audio is captured only while you hold the
button. A wake word is different, and this project describes it honestly
rather than not at all:

- An always-on wake-word satellite runs a tiny local model listening for
  one phrase; nothing is transcribed until it fires.
- **False wakes happen.** A television or a guest can trip it, and when it
  fires, the next few seconds of room audio are transiently transcribed on
  your hub before the intent parser shrugs and discards them. That residue
  is why the voice contract's "transcripts are never retained" rule is
  absolute — but it makes wake-word listening a **`won't`, not a `can't`**.
- So: wake words are an explicit opt-in, on hardware whose whole stated job
  is listening (the satellite), in rooms where you accept that trade. The
  Canaries and the mic-free promise of every other surface are untouched
  either way.

Enable it by assigning the openWakeWord add-on's wake word (e.g. "okay
nabu") to your satellite in the assistant's settings.

## Which microphone — the short list

Scoped against three tests: does it work on Home Assistant OS **without
kernel-driver surgery**, does it fit the satellite contract, and is the
acoustic hardware honest about far-field pickup. Prices are street prices at
the time of writing — verify before buying, they drift.

| Pick | What it is | Verdict |
|---|---|---|
| **Seeed ReSpeaker Lite Voice Assistant Kit** (~$25–40) | 2-mic array + XMOS XU-316 audio DSP with a pre-soldered **XIAO ESP32-S3**, optional speaker + enclosure | **The recommended satellite.** Runs ESPHome as an Assist satellite with the wake word on-device, so audio streams to the hub only after the wake fires — the cleanest possible fit for the contract's rule 1. The DSP does echo cancellation and noise suppression in hardware. And it's the same XIAO ESP32-S3 family half this project's boards already use — a board we know. |
| **Home Assistant Voice Preview Edition** (~$59) | The first-party box: XMOS DSP, dual mics, mute switch | The zero-build alternative. Buy it if you'd rather not flash anything; it exists to be exactly this satellite. |
| **Seeed ReSpeaker XVF3800 USB 4-Mic Array** (~$35–60) | Circular 4-mic array, XMOS XVF3800: AEC, beamforming, 360° far-field to ~5 m, **plain USB audio device** | The hub-attached option. Because it enumerates as standard USB audio, it works on Home Assistant OS with no drivers — pair it with the **Assist Satellite** add-on (the successor to the deprecated Assist Microphone / Wyoming Satellite stack) and the hub itself becomes the room's listening device. Only right if the Pi actually sits in the living space (a hub in a closet can't hear you), and note the transparency shift: the wake-word honesty section above then applies to the hub. |
| **ReSpeaker 2-Mics Pi HAT (v1/v2)** (~$13) | GPIO HAT for the Pi | **Avoid on Home Assistant OS.** The `seeed-voicecard` kernel driver is not in the HAOS build and the request to add it has been open for years — the cheap HAT is the classic trap. It's fine on a *separate* Pi Zero satellite running Raspberry Pi OS + Wyoming, but that's a harder build than the ReSpeaker Lite for similar money. |

Two rules of thumb fall out: **USB beats HAT on Home Assistant OS** (no
kernel modules, no forks to babysit), and **a satellite in the room beats a
better microphone on a hub in a cupboard** — placement is worth more than
mic count. Whichever you pick, the contract is unchanged: wake word opt-in,
transcripts transient, queries only, and never a Canary.

## What voice will refuse — by construction

- **No actions.** The integration registers query intents only
  (`custom_components/securacv/intent.py`). "Disarm the siren" isn't a
  sentence it knows, and there is no handler it could reach if it were.
  Anything that changes the security posture stays on authenticated
  surfaces.
- **No identity questions.** "Who was at the gate?" has no answer anywhere
  in the system — that's Invariant II, not a missing feature.
- **No cloud assistants on this page.** Alexa/Google integration requires a
  publicly reachable endpoint — the exact thing
  [the remote-access page](away_access.md) exists to talk you out of. If
  you want them anyway, that page explains the honest trade.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| "Sorry, I couldn't understand that" | Sentences file not at `/config/custom_sentences/en/securacv.yaml`, or HA not restarted since copying it. |
| Sentence matches but errors | SecuraCV integration not loaded, or an older version without `intent.py` — update the integration. |
| Answers are slow | Whisper model too big for the hub — drop a size in the add-on config. |
| Wake word never fires | openWakeWord not assigned to the satellite, or the satellite has no mic path configured. |

## How this page stays true

A setup guide rots the day it merges unless something holds it to the
code, so this one is pinned three ways:

- The wizard is **generated** (`scripts/gen_hub_voice_setup.py`) with the
  sentences file embedded verbatim, and CI byte-checks the committed
  script — the wizard cannot install a grammar this page doesn't link.
- A test pins the sentences file to the intents the integration actually
  registers (`custom_components/securacv/tests/test_voice.py`) — a
  sentence without a handler, or a handler without a sentence, fails CI.
- `bash hub_voice_setup.sh verify` re-checks a real hub any time —
  the printed state, not this prose, is the authority on your setup.

## Related

- [Whisper local voice — the research and the line it never crosses](research/whisper_local_voice.md)
- [Home Assistant setup](homeassistant_setup.md) · [the full stack](full_stack_setup.md)
- [Reaching your fleet from away](away_access.md) — the same local-first
  posture, applied to remote access
