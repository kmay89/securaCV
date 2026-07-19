// canary-local/assets/guides.js — the curriculum.
//
// Every step can stage the emulator (stage(ctx)) so the glass SHOWS the
// state the words describe — the Apple-pairing-card trick applied to
// teaching: you don't read about the failure, you watch your device have
// it, then watch the fix land. ctx = { emu, fleet, setHour, note }.
//
// Copy is drawn from the design docs (display_ux_design.md,
// display_living_canary.md, canary_qr_onboarding.md,
// getting_started_canary.md) — same voice, same honesty rules.

async function allQuiet(ctx) {
  ctx.emu.setTimeScale(1);
  ctx.setHour(10);
  ctx.emu.setWifi(true);
  ctx.emu.setBroker(true);
  for (const w of ctx.fleet) if (!w.online) await ctx.emu.witnessRevive(w);
  const t = ctx.fleet.find((w) => w.tampered);
  if (t) ctx.emu.witnessTamper(t, false);
}

export const DISPLAY_TOUR = [
  {
    title: "This is the glass",
    body:
      "One arc per canary around the rim — green fresh, amber late, red lost. " +
      "The middle holds the one big fact: when all is quiet, that fact is the time. " +
      "A security display that mostly shows a clock is a system that mostly works.",
    stage: allQuiet,
  },
  {
    title: "The living canary",
    body:
      "The bird's mood is a gauge, not a mascot: every face maps 1:1 to state a " +
      "log line can name. Right now it's calm because every witness is fresh and " +
      "verified. No random sadness, no cheerful mask over a degraded system — " +
      "the Pwnagotchi property, kept honestly.",
    stage: allQuiet,
  },
  {
    title: "When a witness goes quiet…",
    body:
      "We just silenced the Garage canary and ran time at ×60. Watch its arc " +
      "age green → amber (3 min stale) → red (10 min lost), and the bird start " +
      "searching for it at the edge of the screen. Silence is never rendered as " +
      "safety — a missing witness is a first-class alarm.",
    stage: async (ctx) => {
      await allQuiet(ctx);
      const garage = ctx.fleet.find((w) => w.id.includes("garage")) || ctx.fleet[2];
      ctx.emu.witnessSilence(garage);
      ctx.emu.setTimeScale(60);
      ctx.note("time ×60 — three real minutes pass every three seconds");
    },
  },
  {
    title: "Tap pages, long-press acknowledges",
    body:
      "Tap the glass to walk pages: overview → each witness → recent events → " +
      "proof → about. Hold your finger down and an arc sweeps closed — when it " +
      "completes, the house is acknowledged. Quiet, deliberate, works half-asleep. " +
      "Acknowledge never deletes: a residual chip stays until the cause clears.",
    stage: allQuiet,
  },
  {
    title: "Proof on glass",
    body:
      "The proof page renders a QR of the newest signed chain head — verbatim, " +
      "exactly as the witness published it. Scan it with any phone and verify it " +
      "independently: the display's ✓ means an Ed25519 signature checked out " +
      "against a key pinned on THIS device. In this emulator that verification " +
      "is real — the badge you see passed the same code path the silicon runs.",
    stage: allQuiet,
  },
  {
    title: "Night is a promise",
    body:
      "At your quiet hours the glass drops to a calibrated near-dark floor and " +
      "the bird sleeps. A tap gives a dim peek — never a 3 a.m. flash-bang. One " +
      "thing overrides the floor: an unacknowledged alarm. The bedside glance " +
      "must never sleep through a tamper.",
    stage: async (ctx) => {
      await allQuiet(ctx);
      ctx.setHour(23);
      ctx.note("staged 23:00 — tap the glass for a night peek");
    },
  },
  {
    title: "Add a canary",
    body:
      "The display mints an SCV1 QR carrying your Wi-Fi, the hub address, and a " +
      "10-minute single-use token. A new canary looks at the glass and joins — " +
      "no typing, no app, no account. The code counts down and re-mints itself; " +
      "the moment the newcomer appears on the wire, the glass celebrates and " +
      "closes the surface. (Open it: long-press the empty hero, or the settings " +
      "page row.)",
    stage: allQuiet,
  },
  {
    title: "Honesty banners",
    body:
      "We just cut the broker link. The glass says so — a dead link is banner-" +
      "visible, the same baby-monitor rule as a silent witness. The display " +
      "keeps rendering last-known state, clearly marked, and retries on " +
      "backoff (2s → 4s → … → 30s cap) while asking the fleet over mDNS " +
      "whether the broker moved.",
    stage: async (ctx) => {
      await allQuiet(ctx);
      ctx.emu.setBroker(false);
    },
  },
];

export const DISPLAY_FIXES = [
  {
    symptom: "The screen looks dark / off",
    steps: [
      {
        title: "Is it night?",
        body:
          "During quiet hours the glass idles at a near-dark floor (watch) or " +
          "off (dash) on purpose. Tap it — a short dim peek should appear. " +
          "We've staged your emulator at 23:00 so you can see exactly what a " +
          "healthy 'dark' looks like.",
        stage: async (ctx) => { await allQuiet(ctx); ctx.setHour(23); },
      },
      {
        title: "Check the schedule, not the hardware",
        body:
          "Long-press the settings page to open on-glass settings: quiet hours, " +
          "night brightness, and the black-point wizard live there. A floor " +
          "calibrated too low on a dim panel can read as 'off' — re-run the " +
          "wizard and tap when the glow truly disappears.",
        stage: async (ctx) => { await allQuiet(ctx); ctx.setHour(23); },
      },
      {
        title: "The override that proves the panel works",
        body:
          "An unacknowledged alarm always overrides the night floor. We just " +
          "staged a tamper — if your real glass lights for this but stays dark " +
          "otherwise, the panel is healthy and you're simply inside quiet hours.",
        stage: async (ctx) => {
          await allQuiet(ctx);
          ctx.setHour(23);
          ctx.emu.witnessTamper(ctx.fleet[0], true, "enclosure_tamper");
        },
        onDevice:
          "Still black at 10:00 with an alarm staged? Now it's hardware: check " +
          "the USB-C supply (5V/1A+), then the display ribbon. The firmware " +
          "keeps running headless — if MQTT still shows its heartbeat, the " +
          "brain is fine and only the glass needs attention.",
      },
    ],
  },
  {
    symptom: "A witness shows stale / lost",
    steps: [
      {
        title: "Read the ladder first",
        body:
          "Stale (amber) = silent 3 minutes. Lost (red) = 10. Watch it happen " +
          "at ×60: this is a deadline, not a diagnosis — the display can only " +
          "know the witness stopped talking, not why.",
        stage: async (ctx) => {
          await allQuiet(ctx);
          ctx.emu.witnessSilence(ctx.fleet[2]);
          ctx.emu.setTimeScale(60);
        },
      },
      {
        title: "Walk to the witness and read ITS lights",
        body:
          "Every canary answers in a count-coded LED grammar you can say out " +
          "loud: groups of 2 = wrong Wi-Fi password · 3 = no IP from the " +
          "router · 4 = hub unreachable · 5 = provisioning code expired. " +
          "Steady double-blink means it has Wi-Fi and is looking for the hub.",
        onDevice:
          "Power-cycle the witness and watch 60 seconds of LED before touching " +
          "anything else — the pattern usually names the fix.",
      },
      {
        title: "When it comes back",
        body:
          "We revived the witness. Note what the glass does: the arc greens on " +
          "its next heartbeat, the bird stops searching, and the event log " +
          "keeps the gap honest — recovery never rewrites history.",
        stage: async (ctx) => {
          await ctx.emu.witnessRevive(ctx.fleet[2]);
          ctx.emu.setTimeScale(1);
          ctx.setHour(10);
        },
      },
    ],
  },
  {
    symptom: "Banner says the hub / broker is unreachable",
    steps: [
      {
        title: "Wi-Fi and broker are different failures",
        body:
          "The glass distinguishes them. This is broker-down (Wi-Fi fine): the " +
          "display keeps its link to your router but Mosquitto isn't answering. " +
          "Check the hub machine first — is Home Assistant / the broker " +
          "container running?",
        stage: async (ctx) => { await allQuiet(ctx); ctx.emu.setBroker(false); },
      },
      {
        title: "…and this is Wi-Fi-down",
        body:
          "Now the radio itself is out. The display retries on backoff and — " +
          "after five minutes of continuous outage — reboots as a last resort, " +
          "because a witness display that can't reach its network is better " +
          "off starting clean. If it reboot-loops, the network is the problem, " +
          "not the device.",
        stage: async (ctx) => { await allQuiet(ctx); ctx.emu.setWifi(false); },
      },
      {
        title: "The fleet heals itself",
        body:
          "If the broker moved (new DHCP lease, new hub box), any configured " +
          "canary gossips the fresh address over mDNS. We just published a " +
          "referral — watch the serial log adopt it and reconnect. One " +
          "hand-fixed device makes every other one plug-and-play again.",
        stage: async (ctx) => {
          ctx.emu.setWifi(true);
          ctx.emu.setBroker(true);
          ctx.emu.setReferral?.("hub-2.local", 1883);
        },
      },
    ],
  },
  {
    symptom: "No ✓ verified badge / verification FAILED",
    steps: [
      {
        title: "What ✓ actually means",
        body:
          "Verified is never decoration: it appears only after an Ed25519 " +
          "signature verifies against a key this display pinned on first " +
          "contact (TOFU). No pin yet → 'signed'. No signature → 'unsigned'. " +
          "The ladder never overclaims.",
        stage: allQuiet,
      },
      {
        title: "Failed is loud on purpose",
        body:
          "We just injected a chain head whose signature doesn't match the " +
          "pinned key — the firmware's own verifier caught it (in this page, " +
          "cryptographically for real). A red ✕ means the payload or the key " +
          "changed: reflashed witness, restored backup, or someone tampering. " +
          "It cannot be dismissed, only investigated.",
        stage: async (ctx) => {
          await allQuiet(ctx);
          const w = ctx.fleet[0];
          const bogus = {
            length: w.chainLen + 1,
            latest_hash: "deadbeef".repeat(8),
            sig: "A".repeat(86),
            fp: "0000000000000000",
          };
          ctx.emu.publish(w.topic("chain"), bogus);
        },
        onDevice:
          "Legit key change (you reflashed the witness)? Clear the pin from " +
          "the display's settings → trust, and let TOFU re-pin on the next " +
          "health payload.",
      },
    ],
  },
  {
    symptom: "Add-a-canary code won't scan / joins fail",
    steps: [
      {
        title: "The code is alive",
        body:
          "It expires every 10 minutes and silently re-mints with a fresh " +
          "single-use token, so a photographed code is worthless. If the " +
          "countdown hit zero while you fetched the new canary, just glance " +
          "again — it's already new.",
        stage: allQuiet,
      },
      {
        title: "Listen to the scanning canary",
        body:
          "A canary reading the code answers out loud: ascending chirp = " +
          "credentials accepted · error buzz = the code was stale · silence + " +
          "steady scan blink = it can't see a code yet (more light, hand-width " +
          "distance, hold still). Five-blink groups after a join attempt mean " +
          "the token was already used — mint a fresh code.",
      },
      {
        title: "Camera-less canaries",
        body:
          "A Canary Sense (no camera) joins over BLE Improv with the display " +
          "as commissioner, or via the phone's captive portal. Same grammar, " +
          "different eye.",
      },
    ],
  },
];

// Witnesses (no glass): the decoder cards the page shows instead of an
// emulator — LED grammar + chirp meanings, from canary_qr_onboarding.md
// and canary_peripheral_build_plan.md §7.
export const LED_GRAMMAR = [
  { pattern: "100 ms on / 900 ms off", meaning: "waiting for a provisioning code" },
  { pattern: "3 quick blinks, then solid 500 ms", meaning: "code read" },
  { pattern: "rapid even 100/100 ms", meaning: "joining Wi-Fi" },
  { pattern: "double-blink + 600 ms pause", meaning: "Wi-Fi up, finding hub" },
  { pattern: "solid 3 s, then dark", meaning: "enrolled — done" },
  { pattern: "groups of 2", meaning: "wrong Wi-Fi password" },
  { pattern: "groups of 3", meaning: "no IP — router unreachable" },
  { pattern: "groups of 4", meaning: "hub unreachable" },
  { pattern: "groups of 5", meaning: "code expired or already used" },
  { pattern: "2 long blinks", meaning: "unreadable code — keep it steady" },
];

// Translate the documented cadences into on/off timelines for the LED
// demo widget. DOM-free on purpose: tested under Node (repo convention —
// CI runs the exact shipped source).
export function ledSequence(pattern) {
  if (pattern.startsWith("100 ms on / 900")) return [[true, 100], [false, 900]];
  if (pattern.startsWith("rapid even")) return [[true, 100], [false, 100]];
  if (pattern.startsWith("double-blink")) return [[true, 90], [false, 90], [true, 90], [false, 600]];
  if (pattern.startsWith("3 quick")) return [[true, 80], [false, 80], [true, 80], [false, 80], [true, 80], [false, 80], [true, 500], [false, 900]];
  if (pattern.startsWith("solid 3")) return [[true, 3000], [false, 1500]];
  if (pattern.startsWith("2 long")) return [[true, 500], [false, 250], [true, 500], [false, 1200]];
  const m = pattern.match(/groups of (\d)/);
  if (m) {
    const n = Number(m[1]);
    const seq = [];
    for (let i = 0; i < n; i++) seq.push([true, 120], [false, 160]);
    seq.push([false, 900]);
    return seq;
  }
  return [[true, 200], [false, 200]];
}

export const CHIRP_GRAMMAR = [
  { name: "CONFIRM", sound: "one short mid chirp", meaning: "“I'm here”" },
  { name: "SUCCESS", sound: "C5–E5–G5 rising triad", meaning: "join / action succeeded" },
  { name: "ERROR", sound: "descending buzz", meaning: "that didn't work" },
  { name: "ALERT", sound: "rising pattern", meaning: "attention needed" },
  { name: "TAMPER", sound: "5 rapid high pulses", meaning: "enclosure opened / moved" },
  { name: "SELFTEST_OK", sound: "quiet monthly chirp (daytime only)", meaning: "smoke-alarm-style health proof" },
];
