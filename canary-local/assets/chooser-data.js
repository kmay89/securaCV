// canary-local/assets/chooser-data.js — how to pick a Canary, as data.
//
// The needs-matcher's questions, the candidate device+enclosure combos,
// and the scorer. DOM-free on purpose (Node-tested). Sources: the
// enclosure catalog (docs/hardware/enclosure/README.md — statuses,
// ratings honesty per field_ratings.md), the device registry, and the
// firmware feature docs. The scorer never oversells: hard privacy
// exclusions beat scores, outdoor requires a sealed set, and every
// recommendation carries its true release status.

export const QUESTIONS = [
  {
    id: "place",
    title: "Where will it live?",
    multi: false,
    options: [
      { id: "indoor", label: "Indoors — shelf, desk, hallway" },
      { id: "outdoor", label: "Outside — porch, yard, exposed to weather" },
      { id: "door", label: "At the door — doorbell position" },
      { id: "mobile", label: "On the move — vehicle, bag, body-worn" },
      { id: "inwall", label: "In-wall / panel — flush, wired" },
      { id: "bedside", label: "Bedside / nightstand" },
    ],
  },
  {
    id: "want",
    title: "What should it do?",
    multi: true,
    options: [
      { id: "see", label: "Verify people are there (camera, on-device only)" },
      { id: "feel", label: "Feel presence without any camera" },
      { id: "breathe", label: "Watch over sleep / breathing (wellbeing)" },
      { id: "show", label: "Show me the whole fleet at a glance" },
      { id: "prove", label: "Give me tamper-evident proof I can verify" },
    ],
  },
  {
    id: "privacy",
    title: "Hard privacy lines?",
    multi: true,
    options: [
      { id: "nocam", label: "No cameras. Ever." },
      { id: "nomic", label: "No microphones" },
      { id: "ok", label: "Cameras are fine if nothing leaves the device" },
    ],
  },
  {
    id: "power",
    title: "Power situation?",
    multi: false,
    options: [
      { id: "outlet", label: "Wall outlet / USB nearby" },
      { id: "battery", label: "Battery — no cord" },
      { id: "offgrid", label: "Off-grid / solar" },
    ],
  },
];

// Candidate combos. tags = what earns points; requires = every listed
// answer must be present; excludes = any listed answer kills it.
// status mirrors the catalog ("released" = print-validated enclosure +
// shipped firmware path; anything else says exactly what it is).
export const CANDIDATES = [
  {
    id: "wap-compact-indoor",
    device: "canary-wap",
    enclosure: "wap-compact",
    title: "Canary WAP · compact box",
    pitch: "Presence through the WiFi field itself — no camera, no mic needed for sensing. The canary.local dashboard lives on this one.",
    tags: { place: ["indoor"], want: ["feel", "prove"], power: ["outlet"] },
    excludes: {},
    status: "released",
    notes: ["Optional camera/mic exist on the Sense board — leave FEATURE flags off (or mute in NVS) for a radio-only witness."],
  },
  {
    id: "wap-weather-outdoor",
    device: "canary-wap",
    enclosure: "wap-weather",
    title: "Canary WAP · weather box",
    pitch: "The battery build sealed for outside: TPU gasket, drip-edge lid, keyhole mounts.",
    tags: { place: ["outdoor"], want: ["feel", "prove"], power: ["battery", "outlet"] },
    excludes: {},
    status: "released",
    notes: ["CER-2 (~IP54 target) — honest rating ladder in field_ratings.md; sheltered mounting recommended."],
  },
  {
    id: "vision-indoor",
    device: "canary-vision",
    enclosure: "vision-xiao-indoor",
    title: "Canary Vision · indoor",
    pitch: "Person detection on the camera module's own chip — only \"someone is here\" crosses the wire; pixels never leave.",
    tags: { place: ["indoor"], want: ["see", "prove"], power: ["outlet"], privacy: ["ok"] },
    excludes: { privacy: ["nocam"] },
    status: "released",
  },
  {
    id: "vision-weather",
    device: "canary-vision",
    enclosure: "vision-xiao-weather",
    title: "Canary Vision · weather",
    pitch: "Sealed camera witness with rain hood and vent, hinge + keyhole mounts.",
    tags: { place: ["outdoor"], want: ["see", "prove"], power: ["outlet"], privacy: ["ok"] },
    excludes: { privacy: ["nocam"] },
    status: "released",
  },
  {
    id: "vision-doorbell",
    device: "canary-vision",
    enclosure: "vision-doorbell",
    title: "Canary Vision · DOORBELL",
    pitch: "Wyze/Ring form factor with a witness's ethics: camera + button, plate-mounted, hidden security screw, sealed by default.",
    tags: { place: ["door"], want: ["see", "prove"], power: ["outlet"], privacy: ["ok"] },
    excludes: { privacy: ["nocam"] },
    status: "released",
  },
  {
    id: "sense-radome",
    device: "canary-sense",
    enclosure: "sense-radome",
    title: "Canary Sense · radome",
    pitch: "60 GHz radar presence — knows someone's there, never who. No camera, no mic, by construction.",
    tags: { place: ["indoor", "inwall"], want: ["feel", "prove"], power: ["outlet"], privacy: ["nocam", "nomic"] },
    excludes: {},
    status: "released",
    notes: ["Firmware phase 2 complete (signing witness); some bench items open."],
  },
  {
    id: "sense-bedside",
    device: "canary-sense",
    enclosure: "sense-bedside-stand",
    title: "Canary Sense · bedside stand (wellbeing)",
    pitch: "The wellbeing build watches breathing through radar — a baby-monitor's care without a camera in the bedroom.",
    tags: { place: ["bedside"], want: ["breathe", "feel"], power: ["outlet"], privacy: ["nocam", "nomic"] },
    excludes: {},
    status: "in-development",
    notes: ["Wellbeing firmware is a distinct signed OTA product; stand enclosure is render-verified, not print-validated."],
  },
  {
    id: "sense-inwall",
    device: "canary-sense",
    enclosure: "sense-in-wall-plate",
    title: "Canary Sense · in-wall plate",
    pitch: "Single-gang flush mount; the faceplate IS the radome.",
    tags: { place: ["inwall"], want: ["feel"], power: ["outlet"], privacy: ["nocam", "nomic"] },
    excludes: {},
    status: "in-development",
    notes: ["Low-voltage box only; check local code."],
  },
  {
    id: "watch-station",
    device: "canary-display-watch",
    enclosure: "watch-station",
    title: "Canary Watch Station",
    pitch: "The bedside glance: your whole fleet on one calm ring, a living canary whose mood is the system's honest health.",
    tags: { place: ["bedside", "indoor"], want: ["show"], power: ["outlet"] },
    excludes: {},
    status: "in-development",
    notes: ["Firmware compile/CI-verified (you can run it right now in this page's emulator); enclosure render-verified."],
  },
  {
    id: "dash",
    device: "canary-display-dash",
    enclosure: "dashboard-display-case",
    title: "Canary Dash",
    pitch: "The household poster — every witness, events, and proof sheets on one wall panel.",
    tags: { place: ["indoor"], want: ["show"], power: ["outlet"] },
    excludes: {},
    status: "in-development",
    notes: ["Runs live in this page's emulator today; case dims are nominal — measure your panel."],
  },
  {
    id: "field-case",
    device: "canary-wap",
    enclosure: "field-case",
    title: "Canary WAP · field case",
    pitch: "Bag-carry rugged witness: O-ring sealed, TPU boot, zero external ports.",
    tags: { place: ["mobile"], want: ["feel", "prove"], power: ["battery"] },
    excludes: {},
    status: "in-development",
    notes: ["CER-4 intent (IP67 + drop) — earn the rating, don't assume it (field_ratings.md)."],
  },
  {
    id: "solar-relay",
    device: "canary-wap",
    enclosure: "solar-lora-relay-pod",
    title: "Solar LoRa relay pod",
    pitch: "Off-grid mesh backhaul: LoRa + 18650 + solar roof, pole-strapped.",
    tags: { place: ["outdoor"], power: ["offgrid"] },
    excludes: {},
    status: "in-development",
  },
];

// ── scorer ──────────────────────────────────────────────────────────────
// answers: { place: "indoor", want: ["feel"], privacy: ["nocam"], power: "outlet" }
export function score(answers) {
  const as = normalize(answers);
  const out = [];
  for (const c of CANDIDATES) {
    // privacy hard lines
    let excluded = false;
    for (const [q, vals] of Object.entries(c.excludes || {})) {
      if (vals.some((v) => as[q]?.includes(v))) excluded = true;
    }
    // outdoor/mobile hard requirement: a sealed/rated set or bust
    if (as.place?.includes("outdoor") && !(c.tags.place || []).includes("outdoor")) excluded = true;
    if (as.place?.includes("mobile") && !(c.tags.place || []).includes("mobile")) excluded = true;
    if (excluded) continue;

    let s = 0;
    const reasons = [];
    for (const [q, vals] of Object.entries(c.tags)) {
      for (const v of vals) {
        if (as[q]?.includes(v)) {
          s += q === "place" ? 3 : q === "want" ? 2 : 1;
          reasons.push(`${q}:${v}`);
        }
      }
    }
    // released beats vaporware on ties; never silently hides in-dev
    if (c.status === "released") s += 0.5;
    if (s > 0) out.push({ ...c, score: s, reasons });
  }
  out.sort((a, b) => b.score - a.score);
  return out;
}

function normalize(answers) {
  const as = {};
  for (const [k, v] of Object.entries(answers || {})) {
    as[k] = Array.isArray(v) ? v : v != null ? [v] : [];
  }
  return as;
}

// A display belongs beside every recommendation with 2+ witnesses; the
// chooser surfaces it as the companion card, not a competitor.
export function companion(answers, ranked) {
  const wantsShow = (answers.want || []).includes?.("show") ||
                    answers.want === "show";
  if (wantsShow) return null; // already in the main ranking
  if (!ranked.length) return null;
  const bedside = answers.place === "bedside";
  return {
    device: bedside ? "canary-display-watch" : "canary-display-dash",
    why: bedside
      ? "a Watch Station beside the bed turns the fleet into one calm ring"
      : "a Dash on the wall shows every witness — and mints the QR that enrolls the next canary in seconds",
  };
}
