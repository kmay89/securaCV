# Research — Whisper on the hub: local voice, and the line it never crosses (2026-08)

Can SecuraCV use [OpenAI Whisper](https://github.com/openai/whisper) — the
MIT-licensed open-source speech-to-text model family — anywhere in the stack
without violating the invariants? **Yes, in exactly one direction: the owner
deliberately speaking *to* the system.** Whisper transcribing what the world
says *near* a witness device is forbidden forever, and this document is as
much about drawing that line precisely as about the feature on the allowed
side of it.

> **Scope note (this is research, not a built feature).** Nothing here ships
> anything. The recommendation is a small one — bless a hub-side voice-control
> recipe and expose fleet intents to it — and the "never" list is the larger
> deliverable.

---

## 1. The invariant question comes first

The forbidden-capability list ([`AGENTS.md`](../../AGENTS.md), Invariant II in
[`spec/invariants.md`](../../spec/invariants.md)) names `AudioTranscription`
alongside face recognition and plate OCR. What that ban protects against is
the **witness direction**: a microphone pointed at the world, turning
overheard speech into searchable text — an identity substrate and a
behavioral diary in one step. That stays absolute. No nuance below touches it.

The repo's current audio posture, for the record:

- The **kernel ingests no audio at all** (`src/` is video frames only), and
  its web surfaces actively send `Permissions-Policy: microphone=()`
  (`src/api/mod.rs`, `src/break_glass/server.rs`).
- The one mic-bearing display (the 4.3C dash) runs a **scalars-not-samples
  barrier**: each ~20 ms frame is reduced to one RMS number and the buffer is
  zeroed before the read returns. Its standing promise is *"alarm patterns
  only, never speech; audio never recorded, never leaves this board"*
  ([`display_mic_variant.md`](../hardware/display_mic_variant.md)).
- The WAP's `securacv_audio` library is the same barrier, and the guarantee
  everywhere is structural: **the code that could carry speech was never
  written.**

A voice **command** is a different act. The owner walks up to a device whose
whole purpose is listening for them, says a wake word, and issues an
instruction. The audio is *about the speaker, by the speaker, for the
system* — it is input, not witness. Every mainstream privacy framing (and our
own witnessing-vs-watching distinction) treats these differently, and so does
the repo already: [`away_access.md`](../away_access.md) notes that
speech-to-text needs no cloud subscription because "Piper and Whisper run
locally on the hub."

So the honest statement is:

| Direction | What it is | Verdict |
|---|---|---|
| World → system ("what did they say?") | Witnessing speech | **Forbidden. Invariant II, forever.** |
| Owner → system ("Canary, is the fleet OK?") | A command | Allowed, inside the contract in §3 |

## 2. What Whisper is, practically

- **License:** MIT — code *and* released weights. Compatible with our
  Apache-2.0 posture and with local-only deployment.
- **Shape:** encoder-decoder transformer, sizes from `tiny` (~39 M params) to
  `large-v3` (~1.5 B), plus the distilled `large-v3-turbo`. Multilingual.
- **Ports that matter here:**
  - [`faster-whisper`](https://github.com/SYSTRAN/faster-whisper) (MIT,
    CTranslate2) — what Home Assistant's Whisper add-on runs. The right hub
    engine.
  - [`whisper.cpp`](https://github.com/ggml-org/whisper.cpp) (MIT) — CPU/GGML
    port for desktop and mobile embedding, if we ever need in-app dictation.
  - WhisperKit (MIT) — CoreML port for Apple platforms. Noted for
    completeness; §5 explains why we likely never need it.
- **Hardware:** the HA Whisper add-on runs CPU-only on Pi-class hub hardware
  and lets the owner pick the model size. Which size is *usable* for command
  latency on a given hub is a bench question, not one this document answers:
  latency and accuracy vary by hardware, model, and quantization — benchmark
  on the actual hub before recommending a model or promising responsiveness
  (claims discipline applies to speed too).
- **What Whisper is not:** a speaker-identification model. But the pipelines
  people build *around* it (diarization, voice profiles) are exactly the
  identity substrate Invariant II bans — see §4.

## 3. The recommended use: hub voice control, fully local

The best-case use needs almost nothing built, because Home Assistant — which
the Hub already is — ships the whole pipeline as add-ons speaking the
[Wyoming protocol](https://www.home-assistant.io/voice_control/voice_remote_local_assistant/):

```
wake word (openWakeWord) ──► STT (faster-whisper) ──► HA intent ──► action
        on the hub                on the hub          on the hub      │
                                                                      ▼
                                              TTS answer (Piper, on the hub)
```

Every stage runs on the owner's hub. No cloud, no subscription, no audio or
transcript leaving the LAN — the same "free and local beats paid and remote"
verdict `away_access.md` already reached. openWakeWord gates the pipeline, so
nothing is transcribed until the wake word is heard.

**What SecuraCV would actually build** (in rough order of value):

1. **A recipe doc** — Whisper + Piper + openWakeWord add-on setup against our
   hub images, with the §3.1 contract stated up front. One page, the
   `away_access.md` shape: one blessed path, the wrong paths named.
2. **Fleet intents in the HA integration**
   (`custom_components/securacv/`), so "is the fleet OK?", "any events
   overnight?", "which Canary needs attention?" answer from entities the
   integration already exposes. This is the same increment the Apple doc
   plans for Siri ([`apple_home_integration.md`](../design/apple_home_integration.md)
   §4.4) — the fleet ladder, computed locally, read aloud.
3. **A transparency line on the Dash** mirroring the mic chip's honesty: if a
   household runs voice, the hub's voice pipeline is a *satellite* device,
   not a Canary — see the contract.

### 3.1 The voice contract (the load-bearing part)

1. **Voice input hardware is a dedicated voice satellite — never a Canary.**
   An HA Voice PE box, an old phone running the companion app, a push-to-talk
   button in the app. Canary microphones keep their scalars-not-samples
   barrier unchanged; no firmware path that ships audio samples off a Canary
   exists today and none gets written for this. A device whose promise is
   "it shows, it doesn't watch" does not grow ears as a side effect.
2. **Command audio and transcripts are transient.** The waveform and the text
   live exactly long enough to parse an intent, then are gone. Neither is
   ever sealed, journaled, exported, or used as evidence. The *action* the
   command caused is journaled like any other user action.
3. **Local only, forever.** The blessed pipeline is Wyoming services on the
   owner's own hub. Cloud STT is never the documented path — same posture as
   remote access.
4. **Voice may query and nudge; it may not change the security posture.**
   Anyone within earshot — including a television — can say words, and a
   voice command carries no signature. So voice never: originates a Beacon
   alert (the no-automatic-origination rule needs a human *action*, and the
   solo path needs a physical button), approves or requests break-glass
   (quorum approvals come from authenticated principals), disarms or arms
   anything, or mutes an Alert. Status questions, screen wakes, paging a
   dashboard: yes. The siren: never.
5. **No identity sidecars.** No diarization, no per-person voice profiles, no
   "only respond to my voice" enrollment. That is speaker recognition —
   Invariant II — regardless of how convenient it would be.

## 4. What this research does NOT green-light

Named explicitly, because each is a plausible-sounding feature request:

- **Transcribing witnessed audio into events** ("someone shouted near the
  gate: '…'"). Invariant II, full stop. The event vocabulary stays semantic
  and identity-free.
- **Whisper as a sound classifier** (glass break, dog bark). Wrong tool, and
  installing a speech model next to a witness microphone deletes the "can't,
  not won't" guarantee even if it's only used for barks. The T3/T4 cadence
  detector already covers the alarm sounds that matter, with scalars.
- **Transcription in vault or export tooling.** What a break-glass recipient
  does with lawfully released media is their act, on their machine, under
  their accountability — our tools do not offer a transcribe button.
- **Voice biometrics of any kind.** See §3.1 rule 5.
- **Whisper on the ESP32s.** Not a privacy call, a physics one: even `tiny`
  is ~39 M parameters and wants hundreds of MB of working memory; an
  ESP32-S3 has 8 MB of PSRAM. It cannot run there, and per §3.1 rule 1 it
  would not be allowed to anyway.

## 5. The apps

The iPhone/iPad/Watch surface already has its voice plan without Whisper:
**App Intents / Siri** answering "is the fleet OK?" from the on-phone
`FleetStore` ([`apple_home_integration.md`](../design/apple_home_integration.md)
§4.4) — Apple's stack, no new dependency, no new privacy surface. Embedding
Whisper in the apps (whisper.cpp / WhisperKit) earns a place only if a fully
local in-app dictation need appears that the OS keyboard's dictation cannot
meet; none is known today. Desktop Flasher and Lab have no voice need.

## 6. Next steps, smallest first

1. This document (the line, drawn once). ✅
2. The recipe page + FAQ entry ("can I talk to it?" — yes, locally, and
   here is what it will refuse to do by design).
3. Fleet intents in `custom_components/securacv/` behind the existing
   entities.
4. Nothing on-device. Nothing in the kernel. The kernel's answer to audio
   remains: it has none.

---

*Sources: [Whisper](https://github.com/openai/whisper) ·
[faster-whisper](https://github.com/SYSTRAN/faster-whisper) ·
[whisper.cpp](https://github.com/ggml-org/whisper.cpp) ·
[HA local voice pipeline](https://www.home-assistant.io/voice_control/voice_remote_local_assistant/) ·
[HA wake words](https://www.home-assistant.io/voice_control/create_wake_word/)*
