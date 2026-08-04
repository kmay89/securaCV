// canary-local/assets/house-data.js — the Canary House, as data.
//
// An isometric cutaway home and every place a Canary earns its perch:
// rooms on a floor grid, one PLACEMENT per perch, each tied to a real
// chooser candidate (so titles/status/pitch can never drift from the
// catalog), plus the visitor's walk and per-modality honesty copy.
// DOM-free on purpose (Node-tested, like chooser-data.js).
//
// Coordinates: floor-plan units (~1 m). x grows toward screen
// lower-right, y toward screen lower-left; the two back walls sit on
// x=0 and y=0. z is height above the room's floor slab.
import { CANDIDATES } from "./chooser-data.js";

export const WALL_H = 3.0; // story wall height, units
export const SLAB_T = 0.3; // floor slab thickness, units

export const FLOORS = [
  { id: "ground", z: 0 },
  { id: "upper", z: 5.6 }, // exploded dollhouse gap between stories
];

// Interior rooms are rectangles on a floor; `outside: true` rooms are
// open slabs (porch, yard) with no walls.
export const ROOMS = [
  { id: "living",  floor: "ground", label: "Living room",   x0: 0,   y0: 0,   x1: 4.6, y1: 6.5 },
  { id: "kitchen", floor: "ground", label: "Kitchen",       x0: 4.6, y0: 0,   x1: 10,  y1: 3.4 },
  { id: "entry",   floor: "ground", label: "Entry",         x0: 4.6, y0: 3.4, x1: 7.4, y1: 6.5 },
  { id: "garage",  floor: "ground", label: "Garage",        x0: 7.4, y0: 3.4, x1: 10,  y1: 6.5 },
  { id: "bedroom", floor: "upper",  label: "Bedroom",       x0: 0,   y0: 0,   x1: 5,   y1: 4 },
  { id: "nursery", floor: "upper",  label: "Nursery",       x0: 5,   y0: 0,   x1: 10,  y1: 4 },
  { id: "landing", floor: "upper",  label: "Office · landing", x0: 0, y0: 4,  x1: 10,  y1: 6.5 },
  { id: "porch",   floor: "ground", label: "Porch",         x0: 4.6, y0: 6.5, x1: 7.4, y1: 8.5, outside: true },
  { id: "yard",    floor: "ground", label: "Yard",          x0: -2,  y0: -1.6, x1: 12.6, y1: 9.8, outside: true },
];

// Low "ghost" divider walls between interior rooms, per floor. Each is a
// segment in plan; the renderer stands it up to DIVIDER_H.
export const DIVIDER_H = 1.0;
export const DIVIDERS = [
  { floor: "ground", x0: 4.6, y0: 0,   x1: 4.6, y1: 6.5 },
  { floor: "ground", x0: 4.6, y0: 3.4, x1: 10,  y1: 3.4 },
  { floor: "upper",  x0: 5,   y0: 0,   x1: 5,   y1: 4 },
  { floor: "upper",  x0: 0,   y0: 4,   x1: 10,  y1: 4 },
];

// How each sensing modality is drawn and — more importantly — what it
// honestly is. `emits` is the whole point of SecuraCV: the only thing
// that ever leaves a witness is a small signed claim.
export const SENSE_COPY = {
  camera: {
    label: "on-device camera",
    how: "Person detection runs on the camera module's own chip. The picture is born, judged, and destroyed in the same square centimeter.",
    emits: "“person: yes” + confidence, Ed25519-signed — pixels never leave the device.",
  },
  wifi: {
    label: "WiFi field (CSI)",
    how: "Reads the disturbance a body makes in the WiFi field already filling the room — no camera, no microphone needed to feel presence.",
    emits: "presence / dwelling / confidence, Ed25519-signed — the radio field stays a feeling, never a picture.",
  },
  radar: {
    label: "60 GHz mmWave radar",
    how: "Millimeter-wave radar resolves that someone is there — range and motion — while physically unable to resolve who.",
    emits: "presence / occupants / range, Ed25519-signed — identity is impossible by construction.",
  },
  breath: {
    label: "radar wellbeing (breathing)",
    how: "The same radar, tuned soft: it watches the rise and fall of breathing through a duvet — a baby monitor's care with no camera in the room.",
    emits: "breathing rate + presence, Ed25519-signed — nothing to leak but a heartbeat's rhythm.",
  },
  display: {
    label: "display (shows, never senses)",
    how: "No sensors at all — this is where the fleet becomes visible: witness halos, event timelines, proof QRs you can verify by phone.",
    emits: "nothing — it subscribes and shows; its only output is its own signed heartbeat.",
  },
  lora: {
    label: "LoRa mesh backhaul",
    how: "Solar-fed relay that carries witness claims over LoRa where WiFi ends — the fleet's voice, off-grid.",
    emits: "relayed signed claims only — it moves proofs, it doesn't make them.",
  },
  mesh: {
    label: "Meshtastic perimeter node (concept)",
    how: "A fence-guard concept: clamps to the chain-link, feels the fence's own vibration signature — climb, cut, rattle — and speaks Meshtastic, so signed claims hop the LoRa mesh home with no WiFi at all. Solar over battery, sealed against weather; it prefers a shaded run of fence to keep the cell happy.",
    emits: "concept — it would emit fence-event claims, Ed25519-signed, over the mesh. Nothing else, same as every witness.",
  },
};

// One entry per perch. `candidate` MUST be a chooser CANDIDATES id —
// title, status, and device come from there via placementInfo().
// `answers` is a valid pre-fill for the chooser (choose.html#…).
// `aim` is the facing direction in plan degrees (0° = +x, CCW),
// `range` the visual sensing reach in units.
//
// A perch may instead carry `teaser: { title, pitch }` — a concept that
// exists in no catalog yet. Teasers are honesty-fenced by tests: no
// candidate, no chooser answers, excluded from the fleet tally's real
// counts, and (in house.js) they never file witness-feed events.
export const PLACEMENTS = [
  {
    id: "doorbell",
    candidate: "vision-doorbell",
    room: "porch",
    at: { x: 6.95, y: 6.62, z: 1.35 },
    sense: "camera",
    aim: 100,
    range: 3.1,
    spot: "on the door frame",
    headline: "Knows someone is at the door — tells no one what they look like.",
    answers: { place: "door", want: ["see", "prove"], privacy: ["ok"], power: "outlet" },
  },
  {
    id: "driveway",
    candidate: "vision-weather",
    room: "yard",
    at: { x: 10.15, y: 4.6, z: 2.5 },
    sense: "camera",
    aim: 70,
    range: 3.6,
    spot: "under the garage eave",
    headline: "Weather-sealed eyes on the driveway — rain hood, vent, and a vow of silence.",
    answers: { place: "outdoor", want: ["see", "prove"], privacy: ["ok"], power: "outlet" },
  },
  {
    id: "living-wap",
    candidate: "wap-compact-indoor",
    room: "living",
    at: { x: 0.7, y: 1.1, z: 1.0 },
    sense: "wifi",
    aim: 315,
    range: 3.4,
    spot: "on the bookshelf",
    headline: "Feels the whole living room through the WiFi already in it.",
    answers: { place: "indoor", want: ["feel", "prove"], privacy: ["nocam"], power: "outlet" },
  },
  {
    id: "dash",
    candidate: "dash",
    room: "living",
    at: { x: 0.12, y: 4.0, z: 1.7 },
    sense: "display",
    aim: 0,
    range: 1.1,
    spot: "on the wall",
    headline: "The household poster: every witness, one wall, proof QRs included.",
    answers: { place: "indoor", want: ["show"], power: "outlet" },
  },
  {
    id: "kitchen-sense",
    candidate: "sense-radome",
    room: "kitchen",
    at: { x: 7.3, y: 0.15, z: 2.2 },
    sense: "radar",
    aim: 90,
    range: 2.7,
    spot: "high on the back wall",
    headline: "Radar presence over the kitchen — knows someone's cooking, never who.",
    answers: { place: "indoor", want: ["feel", "prove"], privacy: ["nocam", "nomic"], power: "outlet" },
  },
  {
    id: "entry-plate",
    candidate: "sense-inwall",
    room: "entry",
    at: { x: 4.72, y: 4.9, z: 1.4 },
    sense: "radar",
    aim: 0,
    range: 2.1,
    spot: "flush in the wall",
    headline: "A light-switch that happens to be a witness — the faceplate is the radome.",
    answers: { place: "inwall", want: ["feel"], privacy: ["nocam", "nomic"], power: "outlet" },
  },
  {
    id: "go-bag",
    candidate: "field-case",
    room: "entry",
    at: { x: 6.6, y: 3.75, z: 0.55 },
    sense: "wifi",
    aim: 240,
    range: 1.4,
    spot: "on the go-bag shelf",
    headline: "The witness that leaves with you — sealed, battery-fed, zero ports.",
    answers: { place: "mobile", want: ["feel", "prove"], power: "battery" },
  },
  {
    id: "bedroom-radome",
    candidate: "sense-radome",
    room: "bedroom",
    at: { x: 4.85, y: 0.6, z: 2.3 },
    sense: "radar",
    aim: 150,
    range: 3.0,
    spot: "on the wardrobe wall",
    headline: "Presence in the most private room — by the only sensor with nothing to confess.",
    answers: { place: "indoor", want: ["feel"], privacy: ["nocam", "nomic"], power: "outlet" },
  },
  {
    id: "watch",
    candidate: "watch-station",
    room: "bedroom",
    at: { x: 0.7, y: 0.75, z: 0.62 },
    sense: "display",
    aim: 0,
    range: 0.9,
    spot: "on the nightstand",
    headline: "The bedside glance — your whole fleet as one calm ring.",
    answers: { place: "bedside", want: ["show"], power: "outlet" },
  },
  {
    id: "nursery-bed",
    candidate: "sense-bedside",
    room: "nursery",
    at: { x: 8.9, y: 0.8, z: 0.7 },
    sense: "breath",
    aim: 150,
    range: 2.2,
    spot: "on the dresser, aimed at the crib",
    headline: "Watches breathing rise and fall — a baby monitor with no camera to point.",
    answers: { place: "bedside", want: ["breathe", "feel"], privacy: ["nocam", "nomic"], power: "outlet" },
  },
  {
    id: "relay",
    candidate: "solar-relay",
    room: "yard",
    at: { x: 11.7, y: 0.4, z: 0 },
    sense: "lora",
    aim: 0,
    range: 3.3,
    spot: "on a pole past the WiFi's edge",
    headline: "Solar LoRa relay — the fleet keeps its voice where the WiFi ends.",
    answers: { place: "outdoor", power: "offgrid" },
  },
  {
    id: "fence-guard",
    teaser: {
      title: "Canary Fence Guard",
      pitch: "Meshtastic on the property line — the fence becomes a witness.",
    },
    room: "yard",
    at: { x: 12.15, y: 5.4, z: 1.05 },
    sense: "mesh",
    aim: 180,
    range: 2.4,
    spot: "clamped to the chain-link, in the fence line's shade",
    headline: "Coming soon: a solar-fed, weather-sealed Meshtastic fence guard — shade preferred, chain-link mounted, meshed to the relay's field.",
  },
];

// The visitor's walk: plan waypoints (ground floor). Each placement
// with the visitor inside `range` of `at` (in plan) pulses and files a
// witness-feed event.
export const WALK = [
  { x: 6.3, y: 9.6 },
  { x: 6.2, y: 7.4, pause: 1.2, note: "at the door" },
  { x: 6.0, y: 5.0 },
  { x: 3.0, y: 4.6 },
  { x: 2.2, y: 2.4, pause: 0.9, note: "in the living room" },
  { x: 5.6, y: 1.7 },
  { x: 8.2, y: 1.7, pause: 0.9, note: "in the kitchen" },
];

// Look up the honest catalog facts behind a placement. Teaser perches
// answer from their own concept card — status is always "coming-soon".
export function placementInfo(p) {
  if (p.teaser) {
    return {
      title: p.teaser.title,
      device: null,
      status: "coming-soon",
      pitch: p.teaser.pitch,
      enclosure: null,
      notes: ["A concept, not a catalog entry — no firmware, no enclosure, no BOM yet. The request door below is how concepts become candidates."],
      sense: SENSE_COPY[p.sense],
      teaser: true,
    };
  }
  const c = CANDIDATES.find((x) => x.id === p.candidate);
  if (!c) return null;
  return {
    title: c.title,
    device: c.device,
    status: c.status,
    pitch: c.pitch,
    enclosure: c.enclosure,
    notes: c.notes || [],
    sense: SENSE_COPY[p.sense],
  };
}

// choose.html pre-fill fragment for a placement's answers.
export function chooserHash(answers) {
  const parts = [];
  for (const [q, v] of Object.entries(answers || {})) {
    parts.push(`${q}=${Array.isArray(v) ? v.join(",") : v}`);
  }
  return parts.length ? "#" + parts.join("&") : "";
}

// The fleet, tallied honestly: witnesses vs displays vs infrastructure,
// and how many of the chosen perches are released vs in development.
// Teaser concepts never inflate the real counts — they get their own
// `soon` number and stay out of `total`.
export function fleetSummary(activeIds) {
  const on = new Set(activeIds);
  let witnesses = 0, displays = 0, infra = 0, released = 0, indev = 0, soon = 0;
  for (const p of PLACEMENTS) {
    if (!on.has(p.id)) continue;
    if (p.teaser) { soon += 1; continue; }
    const info = placementInfo(p);
    if (!info) continue;
    if (p.sense === "display") displays += 1;
    else if (p.sense === "lora") infra += 1;
    else witnesses += 1;
    if (info.status === "released") released += 1;
    else indev += 1;
  }
  return { witnesses, displays, infra, released, indev, soon, total: witnesses + displays + infra };
}
