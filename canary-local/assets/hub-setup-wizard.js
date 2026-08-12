// canary-local/assets/hub-setup-wizard.js — the "don't feel lost" Hub wizard.
//
// Home Assistant + MQTT is where non-technical people give up: too many
// screens, unlabeled fields, no idea what to type. This wizard walks them
// through it one small step at a time, in plain words — what each thing IS and
// WHY, the exact value to type (with a copy button), and a "Stuck?" panel on
// every step that always has a next move, so nobody hits a dead end. Progress
// is saved locally, so closing the tab never loses their place.
//
// Facts (integration version, minimum HA, the broker port, the topics, the
// canonical doc) come from devices/homeassistant.json so the copy can't drift
// from what the integration actually is. Pure DOM, zero-dep, no network.

const KEY = "hub.wizard.step.v1";
const BROKER_PORT = 1883; // Mosquitto's standard MQTT port
// Repo docs are linked at their source, the same way hub.js/sense.js/vision.js/
// start.js do it. A relative "../docs/…" would walk out of the Lab's own
// directory — which works from a repo checkout and nowhere else: the deployed
// site and the native app both serve canary-local as a root with no parent.
const GH = "https://github.com/kmay89/securaCV/blob/main/";

function el(tag, cls, text) {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
}

// The plain-language procedure. `facts` fills in the honest numbers/links.
export function wizardSteps(facts) {
  const f = facts || {};
  const ha = f.min_ha ? `Home Assistant ${f.min_ha} or newer` : "a recent Home Assistant";
  const ver = f.integration_version ? `v${f.integration_version}` : "the latest";
  return [
    {
      id: "why",
      kicker: "Before anything — why bother?",
      title: "What you’re building, and why it’s worth it",
      what:
        "Right now each Canary watches its own corner alone. This one-time setup " +
        "connects them into a single private guardian for your home: one place " +
        "that sees every room at once, tells you the moment something matters, and " +
        "keeps an honest record of what happened — all running on a box in your " +
        "house, with no cloud, no account, and no monthly fee.",
      do: [
        "You gain: instant alerts (a phone buzz, a light, a siren — your choice) the second a Canary witnesses something.",
        "You gain: one timeline of your whole home — what happened and when, never who. It records events, not faces.",
        "You gain: it keeps working in a blackout or with the internet down, because it all lives at home. Nobody can switch it off from outside, and nothing is ever sold or leaked.",
        "You gain: it’s yours — no subscription, no lock-in. Add or move Canaries anytime and they just join in.",
        "The catch: the pieces need to meet each other once. That’s the next 15 minutes — we’ll do it together, and you can’t break anything.",
      ],
      stuck: [
        "Not sure it’s worth it yet? You can flash and use a single Canary without any of this — the hub is what turns several into a household. Come back when you’re ready.",
        "Worried it’s too technical? That’s exactly what this wizard is for. Every step tells you the precise thing to click, and nothing here can harm your devices.",
      ],
    },
    {
      id: "what",
      kicker: "First, the two words",
      title: "What are Home Assistant and MQTT?",
      what:
        "Home Assistant (HA) is a free app you run at home — think of it as the " +
        "front desk where all your Canaries report in. MQTT is just the language " +
        "they speak to it: a tiny local “post office” that carries short messages " +
        "from each Canary to HA. Nothing leaves your home; there’s no cloud and no " +
        "account to make.",
      do: [
        "You’ll do this once, in about 15 minutes.",
        "You need: a small always-on computer for HA (a Raspberry Pi or any mini-PC), and a Canary you’ve flashed.",
        "If any step goes sideways, open the “Stuck?” panel — it always has a way forward.",
      ],
      stuck: [
        "No spare computer? A $60 Raspberry Pi 5 is plenty — the Board Room lists a ready kit.",
        "Prefer to read it all first? The full written guide is linked at the bottom of every step.",
      ],
    },
    {
      id: "install-ha",
      kicker: "Step 1 of 6",
      title: "Get Home Assistant running",
      what:
        "Install Home Assistant Operating System (HAOS) on your Pi/mini-PC. It’s the " +
        "easiest, most reliable way — it updates and heals itself. You want " + ha + ". " +
        "The Pi never needs a monitor or keyboard: flash it, power it, and it announces " +
        "itself on your network by itself.",
      do: [
        "Follow HA’s official installer for your device, or flash our ready Hub image. Heads up on Wi-Fi: seeding it onto the card at flash time is a desktop Flasher app feature — the browser guide writes stock HAOS, so on that route plug in an ethernet cable for the first boot (you can switch to Wi-Fi inside HA afterwards).",
        "Power it on and give the first boot under 10 minutes as a rule, up to 20 on a slow card — it’s setting itself up; the blinking light is normal.",
        "Then open Home Assistant in your browser (usually http://homeassistant.local:8123) and create your account — that account is local, stored on your box.",
      ],
      link: { label: "Use our ready Hub image →", href: "start.html" },
      stuck: [
        "“homeassistant.local” won’t load? Try it from a phone first (some computers can’t resolve .local names), or find the box’s IP in your router’s device list — look for “homeassistant” — and open that IP, still on :8123.",
        "Flashed with Wi-Fi and it never appears? A mistyped network name or password is invisible from outside — plug in an ethernet cable (it needs no setup at all), or re-flash with the Wi-Fi typed fresh.",
        "Installer stuck? Re-flash the SD card / drive and retry — HAOS images are forgiving, you can’t hurt anything.",
      ],
      check: "I can open Home Assistant and I’m logged in.",
    },
    {
      id: "broker",
      kicker: "Step 2 of 6",
      title: "Add the MQTT “post office” (Mosquitto)",
      what:
        "This is the mailbox your Canaries drop messages into. Home Assistant offers " +
        "it as a one-click app called Mosquitto broker — you don’t configure " +
        "anything, just install and start it.",
      do: [
        "In HA: Settings → Apps → Install app (older Home Assistant calls these “Add-ons”).",
        "Find “Mosquitto broker”, open it, press Install, then Start.",
        "Turn on “Start on boot” and “Watchdog” so it always comes back.",
        "Leave its Configuration tab alone — the defaults are right, and “Logins” there is the wrong place for your Canary account.",
      ],
      stuck: [
        "No Apps section (or Add-on Store)? You’re on HA Container/Core instead of HAOS — install a standalone Mosquitto, or switch to HAOS (recommended).",
        "Install spins forever? Give it a minute on slow SD cards, then reload the page.",
      ],
      check: "Mosquitto broker shows “Started”.",
    },
    {
      id: "mqtt-user",
      kicker: "Step 3 of 6",
      title: "Make a login your Canaries will use",
      what:
        "The post office needs a name badge. Create one simple Home Assistant user " +
        "just for the devices — the Canary signs in to the broker with it. Pick " +
        "anything you’ll remember; you’ll type it once more in Step 5.",
      do: [
        "First turn on Advanced Mode: click your name at the bottom-left, then the Advanced Mode switch — without it the Users list is hidden and this step looks impossible.",
        "In HA: Settings → People → Users → Add User.",
        "Name it something like “canary”, give it a password, and leave Administrator off — the broker login needs no admin rights.",
        "Write the username and password down — Step 5 asks for them.",
      ],
      values: [
        { label: "Suggested username", value: "canary" },
      ],
      stuck: [
        "You can reuse your own HA login instead — it works, it’s just tidier to have a separate one for devices.",
        "Forgot the password later? Just edit the user in HA and set a new one, then update the Canary in Step 5.",
      ],
      check: "I created a user and know its password.",
    },
    {
      id: "integration",
      kicker: "Step 4 of 6",
      title: "Teach Home Assistant about Canaries",
      what:
        "Two quick adds. First MQTT itself, so HA listens to the post office. Then the " +
        "SecuraCV integration (" + ver + "), which turns the Canaries’ messages into " +
        "proper devices, timelines and alerts — installed through HACS.",
      do: [
        "MQTT: Settings → Devices & Services → Add Integration → “MQTT”. It auto-detects Mosquitto — just accept.",
        "SecuraCV: install HACS if you haven’t, then in HACS search “SecuraCV”, install it, and restart HA.",
        "After restart: Settings → Devices & Services → Add → “SecuraCV”.",
      ],
      link: { label: "The full written walkthrough →", href: "homeassistant.html" },
      stuck: [
        "MQTT step asks for a broker? Enter host “core-mosquitto”, port " + BROKER_PORT + ", and the user/password from Step 3.",
        "Can’t find SecuraCV in HACS? Add github.com/kmay89/securacv-homeassistant as a custom repository in HACS → three-dots → Custom repositories.",
      ],
      check: "MQTT and SecuraCV both show under Devices & Services.",
    },
    {
      id: "point-canary",
      kicker: "Step 5 of 6",
      title: "Point your Canary at the post office",
      what:
        "Now tell the Canary where to send its messages. You do this in the flasher " +
        "when you set up Wi-Fi — the broker is just your Home Assistant box. These are " +
        "the four things to enter:",
      do: [
        "Broker host: your Home Assistant’s IP address (find it in HA → Settings → System → Network).",
        "Port: " + BROKER_PORT + " (the standard — leave it).",
        "Username / Password: the login you made in Step 3.",
      ],
      values: [
        { label: "Broker host", value: "your HA’s IP (e.g. 192.168.1.20)" },
        { label: "Port", value: String(BROKER_PORT) },
        { label: "Username", value: "canary (from Step 3)" },
      ],
      link: { label: "Open the flasher to enter these →", href: "flash.html" },
      stuck: [
        "Don’t know your HA’s IP? HA → Settings → System → Network shows it, or check your router’s device list.",
        "Typed it and nothing? Make sure the Canary and HA are on the same Wi-Fi, and the port is " + BROKER_PORT + ".",
      ],
      check: "The Canary has the broker host, port and login.",
    },
    {
      id: "verify",
      kicker: "Step 6 of 6",
      title: "Watch it appear",
      what:
        "When the Canary powers up on your Wi-Fi, it announces itself to MQTT and Home " +
        "Assistant builds its device card automatically — usually within 30 seconds. No " +
        "more setup: from here it just works.",
      do: [
        "In HA: Settings → Devices & Services → SecuraCV — your Canary shows up with its entities.",
        "Scroll down on this page to watch a live demo of exactly what that looks like.",
      ],
      stuck: [
        "Nothing after a minute? Power-cycle the Canary; on first boot it can take a moment to join Wi-Fi.",
        "Still nothing? In HA → Settings → Apps → Mosquitto broker → Log, you should see the Canary connect — if not, re-check the login (Step 3/5).",
        "Truly stuck? The full guide and a checklist are one click below — you’re close, this is the last mile.",
      ],
      check: "My Canary shows up in Home Assistant. 🎉",
      done: true,
    },
  ];
}

function loadStep() {
  try { const n = parseInt(localStorage.getItem(KEY), 10); return Number.isFinite(n) ? n : 0; }
  catch { return 0; }
}
function saveStep(n) { try { localStorage.setItem(KEY, String(n)); } catch { /* private mode */ } }

// Build the wizard into `container`, reading honest facts from the HA catalog.
export function buildHubWizard(container, ha) {
  const facts = {
    min_ha: ha && ha.integration && ha.integration.min_ha,
    integration_version: ha && ha.integration && ha.integration.version,
    doc: (ha && ha.docs && ha.docs.setup) || "docs/homeassistant_setup.md",
  };
  const steps = wizardSteps(facts);
  let idx = Math.min(loadStep(), steps.length - 1);

  const root = el("div", "hub-wiz");
  container.append(root);

  const render = () => {
    root.innerHTML = "";
    const step = steps[idx];

    // progress rail — always shows the whole path and where you are
    const rail = el("ol", "hub-wiz-rail");
    steps.forEach((s, i) => {
      const li = el("li", "hub-wiz-dot" +
        (i === idx ? " on" : "") + (i < idx ? " done" : ""));
      li.title = s.title;
      li.textContent = i < idx ? "✓" : String(i + 1);
      li.addEventListener("click", () => { idx = i; saveStep(idx); render(); });
      rail.append(li);
    });
    root.append(rail);

    const card = el("div", "hub-wiz-card");
    if (step.kicker) card.append(el("p", "hub-wiz-kicker", step.kicker));
    card.append(el("h3", "hub-wiz-title", step.title));
    card.append(el("p", "hub-wiz-what", step.what));

    if (step.do && step.do.length) {
      const ol = el("ol", "hub-wiz-do");
      step.do.forEach((d) => ol.append(el("li", null, d)));
      card.append(ol);
    }

    if (step.values && step.values.length) {
      const vals = el("div", "hub-wiz-values");
      step.values.forEach((v) => {
        const row = el("div", "hub-wiz-value");
        row.append(el("span", "hub-wiz-value-label", v.label));
        const code = el("code", "hub-wiz-value-code", v.value);
        row.append(code);
        const copy = el("button", "hub-wiz-copy", "copy");
        copy.type = "button";
        copy.addEventListener("click", () => {
          try { navigator.clipboard.writeText(v.value); copy.textContent = "copied ✓"; setTimeout(() => (copy.textContent = "copy"), 1200); }
          catch { /* clipboard blocked — the value is right there to read */ }
        });
        row.append(copy);
        vals.append(row);
      });
      card.append(vals);
    }

    if (step.link) {
      const a = el("a", "hub-wiz-link", step.link.label);
      a.href = step.link.href;
      card.append(a);
    }

    // the always-there escape hatch — no dead ends
    if (step.stuck && step.stuck.length) {
      const det = el("details", "hub-wiz-stuck");
      det.append(el("summary", null, "Stuck? Not sure? Open this →"));
      const ul = el("ul", null);
      step.stuck.forEach((s) => ul.append(el("li", null, s)));
      det.append(ul);
      card.append(det);
    }

    // nav
    const nav = el("div", "hub-wiz-nav");
    const back = el("button", "ghost small", "← Back");
    back.type = "button";
    back.disabled = idx === 0;
    back.addEventListener("click", () => { idx = Math.max(0, idx - 1); saveStep(idx); render(); });
    nav.append(back);

    if (step.done) {
      const restart = el("button", "hub-wiz-next", "Set up another — start over");
      restart.type = "button";
      restart.addEventListener("click", () => { idx = 0; saveStep(idx); render(); });
      nav.append(restart);
    } else {
      const next = el("button", "hub-wiz-next", step.check ? "Done ✓ — next step" : "Next →");
      next.type = "button";
      next.addEventListener("click", () => { idx = Math.min(steps.length - 1, idx + 1); saveStep(idx); render(); });
      nav.append(next);
    }
    card.append(nav);

    // the whole-guide safety net, on every step
    const net = el("p", "hub-wiz-net");
    net.append(document.createTextNode("Rather read it all, or truly stuck? "));
    const dl = el("a", null, "the complete written guide");
    dl.href = GH + facts.doc;
    // New tab, always: the Lab app is one webview with no chrome and no Back,
    // so an in-place navigation to the guide strands the user in a page they
    // can only leave by restarting. Every other external link here does this.
    dl.target = "_blank";
    dl.rel = "noopener noreferrer";
    net.append(dl);
    net.append(document.createTextNode(" walks through every screen."));
    card.append(net);

    root.append(card);
  };

  render();
  return root;
}
