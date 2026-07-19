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
    title: "Make it yours",
    body:
      "Seven Characters — ages of technology, from a grandparent's warm " +
      "Heirloom to phosphor Terminal — each a pre-validated look: type, " +
      "palette, the bird's temperament, even how it phrases \"all quiet\". " +
      "We just dressed this one in Heirloom, live. The alarms never restyle " +
      "(a fire is red for everyone) and night outranks every look. Flip " +
      "through the rest under Try it.",
    stage: async (ctx) => {
      await allQuiet(ctx);
      ctx.emu.applyCharacter?.(1); // Heirloom — the wave the tour wears
    },
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
  {
    title: "Now make it yours",
    body:
      "That's the whole promise: glance, know, go back to your life. " +
      "Head to Try it — flip all seven Characters (the glass remembers " +
      "through power cycles), meet the bird for the very first time, and " +
      "break the household on purpose to watch it refuse to lie. " +
      "Everything you just used is the real firmware; the one on your " +
      "desk will feel exactly like this.",
    stage: allQuiet,
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

// ── The bench's debug curriculum ────────────────────────────────────────
// Symptom-first flows for the PHYSICAL layer — cable, battery, switch,
// buttons, the hardwired lights. ctx additionally carries `bench` (the
// BenchPower plane from emulator/web/bench.js). Same honesty rules: every
// staged state is exactly what the power plane would do to real silicon,
// and where hardware genuinely can't do a thing (turn off a rail LED from
// software), the flow says so instead of pretending.
export const BENCH_FIXES = [
  {
    symptom: "A red light is always on — can I turn it off?",
    steps: [
      {
        title: "Those lights don't answer to software",
        body:
          "The power/charge LEDs (PWR, CHG, DONE on the dash; CHG on the " +
          "watch's XIAO) are wired to the power rail and the charge chip — " +
          "not to any pin the ESP32 controls. No setting, no firmware " +
          "update, no amount of code can switch them off while the board " +
          "is powered. The bench proves it: the firmware is running right " +
          "now, and it has no knob for this.",
        stage: async (ctx) => {
          ctx.bench?.setUsb(true);
          ctx.note?.("watch the LED rail — the firmware has no say in it");
        },
      },
      {
        title: "What actually turns them off",
        body:
          "Physics only: PWR follows the rail (cut all power and it dies), " +
          "CHG goes out when charging finishes or USB leaves, DONE only " +
          "glows while a full battery sits on USB. We just unplugged USB — " +
          "watch the charge lights drop while the board rides the battery.",
        stage: async (ctx) => {
          ctx.bench?.setBattery(true);
          ctx.bench?.setSwitch(true);
          ctx.bench?.setUsb(false);
        },
        onDevice:
          "If the glow bothers you at night: the enclosures are designed to " +
          "shade the board LEDs, and a small square of matte tape over the " +
          "light is bench-legal. Don't desolder — the CHG/DONE pair is your " +
          "only honest window into the charger.",
      },
      {
        title: "The one exception",
        body:
          "The watch's XIAO carries a USER LED on GPIO21 — that one IS " +
          "firmware territory. This firmware leaves it dark on purpose " +
          "(a bedside device must not grow unexplained lights), which is " +
          "why you've never seen it.",
      },
    ],
  },
  {
    symptom: "The charge light is flickering",
    steps: [
      {
        title: "It's hunting for a battery",
        body:
          "On the watch's XIAO, a flickering CHG light with USB in means no " +
          "battery is fitted — the charge chip keeps probing for a cell and " +
          "finding nothing. Completely healthy on a USB-only bench. We just " +
          "staged it: USB in, battery out.",
        stage: async (ctx) => {
          ctx.bench?.setUsb(true);
          ctx.bench?.setBattery(false);
        },
      },
      {
        title: "Fit the battery and it settles",
        body:
          "Battery back in: CHG goes steady while filling, then hands over " +
          "(off on the watch, DONE on the dash) when the cell is full. Use " +
          "the bench's fast-forward to watch the handover without waiting.",
        stage: async (ctx) => {
          ctx.bench?.setUsb(true);
          ctx.bench?.setBattery(true);
        },
      },
    ],
  },
  {
    symptom: "Screen dark, but the power light is on",
    steps: [
      {
        title: "First guess: it's asleep, not broken",
        body:
          "During quiet hours the glass idles near-dark (watch) or off " +
          "(dash) on purpose, while the board — and its PWR light — stay " +
          "up. Tap the glass for a dim peek. Staged: 23:00.",
        stage: async (ctx) => {
          ctx.bench?.setUsb(true);
          ctx.emu.setTimeScale(1);
          ctx.setHour(23);
        },
      },
      {
        title: "Second guess: it's in download mode",
        body:
          "If BOOT was held (or wedged by an enclosure misfit) when the " +
          "board last reset, the ROM sits in download mode: rail up, PWR " +
          "lit, screen dead, serial saying 'waiting for download'. We just " +
          "did exactly that. Press RESET — alone — and it boots normally.",
        stage: async (ctx) => {
          ctx.bench?.setBootHeld(true);
          ctx.bench?.pressReset();
          ctx.bench?.setBootHeld(false);
          ctx.note?.("check the serial panel — then press RESET on the bench to recover");
        },
        onDevice:
          "Serial silent AND the screen dark at 10:00? Now suspect the " +
          "display ribbon or the panel supply — the brain and the glass " +
          "have separate failure modes.",
      },
    ],
  },
  {
    symptom: "Serial says 'waiting for download'",
    steps: [
      {
        title: "That's the ROM, not a fault",
        body:
          "BOOT (GPIO0) was low when reset released, so the mask ROM " +
          "parked the chip for flashing instead of running the app. It " +
          "will wait forever — that's its job. It happens on a bench when " +
          "you hold BOOT out of habit, or a case presses the button.",
        stage: async (ctx) => {
          ctx.bench?.setBootHeld(true);
          ctx.bench?.pressReset();
          ctx.bench?.setBootHeld(false);
        },
      },
      {
        title: "Recovery is one button",
        body:
          "Press RESET with BOOT released and the app boots — nothing was " +
          "lost; NVS never noticed. Do it on the bench now and watch the " +
          "real boot banner replace the ROM's.",
        stage: async (ctx) => {
          ctx.bench?.pressReset();
        },
      },
    ],
  },
  {
    symptom: "Nothing at all — no lights, no glass",
    steps: [
      {
        title: "No light means no power — full stop",
        body:
          "The PWR/CHG LEDs sit ahead of everything the firmware does. If " +
          "every light is dark, the board has no rail: cable, switch, or " +
          "battery. We just staged the worst case — everything removed.",
        stage: async (ctx) => {
          ctx.bench?.setUsb(false);
          ctx.bench?.setBattery(false);
        },
      },
      {
        title: "Work the power path in order",
        body:
          "USB first (a DATA cable — charge-only leads are the classic " +
          "bench trap), then the switch (it only matters on battery), then " +
          "the battery itself. Restore USB on the bench and watch the ROM " +
          "banner and the splash come back on their own — NVS kept " +
          "everything through the outage.",
        stage: async (ctx) => {
          ctx.bench?.setUsb(true);
        },
        onDevice:
          "Real bench, still dark on a known-good data cable? Try a 5 V/1 A+ " +
          "supply — the dash's RGB panel browns out weak laptop ports.",
      },
    ],
  },
  {
    symptom: "It died the moment I unplugged the cable",
    steps: [
      {
        title: "The switch only gates the battery",
        body:
          "On both boards the ON/OFF switch is in the battery path, not " +
          "the USB path. USB in → the board runs regardless of the switch. " +
          "So if pulling USB kills it, either the switch is OFF, no " +
          "battery is fitted, or the battery is flat. Staged: switch OFF, " +
          "USB out — instant dark.",
        stage: async (ctx) => {
          ctx.bench?.setUsb(true);
          ctx.bench?.setBattery(true);
          ctx.bench?.setSwitch(false);
          ctx.bench?.setUsb(false);
        },
      },
      {
        title: "…and the ride-through, done right",
        body:
          "Switch ON, healthy battery, pull USB: the firmware never even " +
          "notices — same uptime, same fleet, same everything. That's the " +
          "whole point of the battery. Watch the serial log: no reboot.",
        stage: async (ctx) => {
          ctx.bench?.setUsb(true);
          ctx.bench?.setBattery(true);
          ctx.bench?.setSwitch(true);
          ctx.bench?.setUsb(false);
        },
      },
    ],
  },
  {
    symptom: "It reboots by itself",
    steps: [
      {
        title: "Recovery of last resort, on schedule",
        body:
          "A display that loses Wi-Fi retries on backoff and — after five " +
          "continuous minutes — reboots deliberately, because starting " +
          "clean beats wedging forever. We cut Wi-Fi and ran time at ×60: " +
          "watch the serial log count down to its own reset, then come " +
          "back with memory intact.",
        stage: async (ctx) => {
          ctx.emu.setWifi(false);
          ctx.emu.setTimeScale(60);
          ctx.note?.("×60 — the 5-minute outage deadline lands in seconds");
        },
      },
      {
        title: "Tell deliberate reboots from brownouts",
        body:
          "The reset reason in the ROM banner never lies: RTC_SW_SYS_RST " +
          "is the firmware rebooting itself; a brownout means power, not " +
          "software — undersized supply or a dying battery. Restore Wi-Fi " +
          "and the reboots stop.",
        stage: async (ctx) => {
          ctx.emu.setWifi(true);
          ctx.emu.setTimeScale(1);
        },
        onDevice:
          "Reboot-looping on real glass? Read the rst: line at the top of " +
          "each boot. POWERON in a loop = power problem. RTC_SW_SYS_RST in " +
          "a loop = it can't reach your network — fix the network, not the " +
          "board.",
      },
    ],
  },
  {
    symptom: "Pressing BOOT does nothing",
    steps: [
      {
        title: "Correct — and that's a design fact",
        body:
          "BOOT is GPIO0, a strapping pin the ROM samples at reset. While " +
          "the firmware runs, this display never reads it (witness canaries " +
          "use theirs as a presence gate; the displays deliberately don't). " +
          "Hold it now — nothing. Hold it AND press RESET — download mode. " +
          "That's its entire vocabulary.",
        stage: async (ctx) => {
          ctx.bench?.setBootHeld(true);
          ctx.note?.("BOOT held — the running firmware doesn't even see it");
        },
      },
      {
        title: "Release it before the next reset",
        body:
          "The only way BOOT 'does something' is by being low at the wrong " +
          "moment. Released, every reset boots the app. (We released it " +
          "for you.)",
        stage: async (ctx) => {
          ctx.bench?.setBootHeld(false);
        },
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
