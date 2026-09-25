// massing.mjs — what each thing in the fleet actually looks like, in mm.
//
// A figure is a MASSING, not a render: proportion, stack order and the one
// or two features that make a part recognizable (the radome window, the
// lens, the button, the keyhole). It deliberately carries no fillet detail,
// no tilt and no texture. That is what lets the same description drive a
// 900 px hero and a 20 px watch row and still read as the same object.
//
// ── Where the numbers come from ──────────────────────────────────────────
// Almost nothing here is typed by hand. A part declares the committed STL it
// IS, and the generator reads that STL's bounding box at build time and
// hands it to `build(E)` as the envelope. So the figure is a function of the
// CAD, not a copy of it: re-export the STL and the figure follows. Parts
// with no committed STL (the in-development enclosures, and the concept
// devices that are still only a research note) declare a `sketch` envelope
// instead and are marked as such in the ledger — a sketched thing renders as
// a ghost and can never pass for a product.
//
// ── Frames ───────────────────────────────────────────────────────────────
// The SCADs are authored face-up for printing: +Z is out through the face,
// +Y is up the wall. The figure frame is +X right, +Y front, +Z up. So a
// wall-mounted part's STL box maps (x, y, z)_scad -> (x, z, y)_fig. That
// single swap is `frame: 'scad-wall'` and it is applied by the generator, so
// `build(E)` always receives {w, d, h} already in the figure frame.

/* ------------------------------------------------------------------ helpers
 * Small vocabulary so the specs below read as description, not arithmetic.
 * EPS holds a stacked element a hair proud of the surface under it — the
 * same rule the .glb generators follow, for the same reason: two coplanar
 * faces of different materials are a defect (z-fighting there, an ambiguous
 * paint order here). The coplanar guard in tests/figures.test.js enforces it.
 */
const EPS = 0.05;

// A printed plate facing the viewer: fills the envelope's width and height,
// `t` thick, its front face at y0 + t. Inset by `i` all round. This is what
// almost every part in the fleet is — the SCADs are authored face-up.
const slab = (E, m, y0, t, i = 0, r = 0, extra = {}) => ({
  kind: 'box', m, face: 'y', at: [i, y0, i], size: [E.w - 2 * i, t, E.h - 2 * i], r, ...extra,
});

// A feature disc looking at the viewer out of the front face (+Y), standing
// proud of it: a lens, a button, a status LED.
const onFace = (E, m, x, z, r, h = 0.8, extra = {}) => ({
  kind: 'disc', m, axis: 'y', at: [x, E.d + EPS, z], r, h, ...extra,
});

// A rectangular feature on the front face (a screen, a radome window, a vent
// block). `face:'y'` rounds the outline in the plane you can see, not in plan.
const panel = (E, m, x, z, w, h, t = 0.8, r = 0, extra = {}) => ({
  kind: 'box', m, face: 'y', at: [x, E.d + EPS, z], size: [w, t, h], r, ...extra,
});

/* ------------------------------------------------------------------ figures
 * `role` drives how the thing is talked about, not how it is drawn:
 *   device     a whole product a user owns
 *   part       something that comes off a print bed
 *   board      a bought module
 *   tool       a thing you print to check something, not to keep
 */

export const FIGURES = [
  /* ═══════════════════════════════════════════════ Canary Sense ══════ */
  {
    id: 'part.sense.back',
    title: 'Canary Sense — back shell',
    role: 'part', of: 'canary-sense',
    stl: 'canary_sense_back.stl', frame: 'scad-wall',
    build: (E) => [
      slab(E, 'shell', 0, E.d),
      // the mount tail runs past the body; a keyhole marks which end is up
      onFace(E, 'dark', E.w / 2, E.h - 7, 3.2, 0.6, { detail: 'full' }),
    ],
  },
  {
    id: 'part.sense.front',
    title: 'Canary Sense — radome front',
    role: 'part', of: 'canary-sense',
    stl: 'canary_sense_front.stl', frame: 'scad-wall',
    build: (E) => [
      slab(E, 'shell', 0, E.d, 0, 3),
      // the 24 x 24 mm window the radar looks through — the one feature that
      // tells this part apart from every other flat printed face in the fleet
      panel(E, 'radome', E.w / 2 - 12, E.h / 2 - 12, 24, 24, 0.9, 1.5),
    ],
  },
  {
    id: 'device.canary-sense',
    title: 'Canary Sense',
    role: 'device', of: 'canary-sense',
    parts: ['canary_sense_back.stl', 'canary_sense_front.stl'],
    frame: 'scad-wall',
    // Drawn AS ASSEMBLED: each part fills its visible band between the
    // measured seams (assembled_dims.json), so the drawn depth is the
    // assembled depth — the front's nesting lip is inside the back, not
    // stacked in front of it. Same rule on every multi-part device below.
    build: (E, P, A) => {
      // the piston plate: the shell (front) is the whole side profile — the
      // plate (back) nests inside its walls, so no seam crosses the side
      return [
        { kind: 'box', m: 'shell', face: 'y', at: [0, 0, 0], size: [E.w, E.d, E.h], r: 3 },
        {
          kind: 'box', m: 'radome', face: 'y',
          at: [E.w / 2 - 12, E.d - EPS, E.h / 2 - 12],
          size: [24, 0.9, 24], r: 1.5,
        },
        { kind: 'disc', m: 'accent', axis: 'y', at: [E.w / 2, E.d + 0.9 - EPS, 6], r: 2, h: 0.6, detail: 'full' },
      ];
    },
  },

  /* ══════════════════════════════════════════════ Canary Vision ══════ */
  {
    id: 'part.vision.back',
    title: 'Canary Vision — back shell (XIAO, indoor)',
    role: 'part', of: 'canary-vision',
    stl: 'canary_vision_enclosure_xiao_indoor_back.stl', frame: 'scad-wall',
    build: (E) => [slab(E, 'shell2', 0, E.d, 0, 2)],
  },
  {
    id: 'part.vision.front',
    title: 'Canary Vision — front face (XIAO, indoor)',
    role: 'part', of: 'canary-vision',
    stl: 'canary_vision_enclosure_xiao_indoor_front.stl', frame: 'scad-wall',
    build: (E) => [
      slab(E, 'shell', 0, E.d, 0, 3),
      onFace(E, 'lens', E.w / 2, E.h - 14, 5.5, 1.2),
      onFace(E, 'accent', E.w / 2, 8, 1.8, 0.5, { detail: 'full' }),
    ],
  },
  {
    id: 'part.vision.gasket',
    title: 'Canary Vision — TPU weather gasket',
    role: 'part', of: 'canary-vision',
    stl: 'canary_vision_enclosure_xiao_weather_gasket.stl', frame: 'scad-wall',
    build: (E) => [slab(E, 'gasket', 0, E.d, 0, 3)],
  },
  {
    id: 'part.vision.bracket',
    title: 'Canary Vision — GoPro-compatible bracket',
    role: 'part', of: 'canary-vision',
    stl: 'canary_vision_enclosure_bracket.stl', frame: 'scad-wall',
    build: (E) => [
      // the flat foot, and the two-eared clevis that stands off it
      { kind: 'box', m: 'shell2', face: 'y', at: [0, 0, 0], size: [E.w, E.d, E.h * 0.42], r: 3 },
      { kind: 'box', m: 'shell2', face: 'y', at: [E.w * 0.32, 0, E.h * 0.42 - EPS], size: [E.w * 0.36, E.d, E.h * 0.58], r: 3 },
    ],
  },
  {
    id: 'part.vision.knob',
    title: 'Canary Vision — bracket knob',
    role: 'part', of: 'canary-vision',
    stl: 'canary_vision_enclosure_knob.stl', frame: 'scad-wall',
    build: (E) => [{ kind: 'cyl', m: 'dark', axis: 'y', at: [E.w / 2, 0, E.h / 2], r: E.w / 2, h: E.d }],
  },
  {
    id: 'device.canary-vision',
    title: 'Canary Vision',
    role: 'device', of: 'canary-vision',
    parts: [
      'canary_vision_enclosure_xiao_indoor_back.stl',
      'canary_vision_enclosure_xiao_indoor_front.stl',
    ],
    frame: 'scad-wall',
    build: (E, P, A) => {
      const top = E.h;
      // the piston plate: the shell (front) is the whole side profile
      return [
        { kind: 'box', m: 'shell', face: 'y', at: [0, 0, 0], size: [E.w, E.d, E.h], r: 3 },
        { kind: 'disc', m: 'lens', axis: 'y', at: [E.w / 2, E.d - EPS, top - 14], r: 5.5, h: 1.4 },
        { kind: 'disc', m: 'accent', axis: 'y', at: [E.w / 2, E.d - EPS, 8], r: 1.8, h: 0.5, detail: 'full' },
      ];
    },
  },

  {
    id: 'device.canary-vision-devkit',
    title: 'Canary Vision (DevKitM host)',
    role: 'device', of: 'canary-vision',
    // The Grove-cabled DevKitM layout is a genuinely different housing from
    // the stacked-XIAO build — wider, and its own committed STLs. Drawing one
    // for both would show half the owners the wrong box.
    parts: [
      'canary_vision_enclosure_devkit_indoor_back.stl',
      'canary_vision_enclosure_devkit_indoor_front.stl',
    ],
    frame: 'scad-wall',
    build: (E, P, A) => {
      // the piston plate: the shell (front) is the whole side profile
      return [
        { kind: 'box', m: 'shell', face: 'y', at: [0, 0, 0], size: [E.w, E.d, E.h], r: 3 },
        { kind: 'disc', m: 'lens', axis: 'y', at: [E.w / 2, E.d - EPS, E.h - 14], r: 5.5, h: 1.4 },
        { kind: 'disc', m: 'accent', axis: 'y', at: [E.w / 2, E.d - EPS, 8], r: 1.8, h: 0.5, detail: 'full' },
      ];
    },
  },

  /* ═════════════════════════════════════ Canary Vision Doorbell ══════ */
  {
    id: 'part.doorbell.plate',
    title: 'Canary Vision Doorbell — wall plate',
    role: 'part', of: 'canary-vision',
    stl: 'canary_vision_doorbell_plate.stl', frame: 'scad-wall',
    build: (E) => [slab(E, 'shell2', 0, E.d, 0, 8)],
  },
  {
    id: 'part.doorbell.body',
    title: 'Canary Vision Doorbell — body',
    role: 'part', of: 'canary-vision',
    stl: 'canary_vision_doorbell_body.stl', frame: 'scad-wall',
    build: (E) => [slab(E, 'shell', 0, E.d, 0, 12)],
  },
  {
    id: 'part.doorbell.face',
    title: 'Canary Vision Doorbell — face plate',
    role: 'part', of: 'canary-vision',
    stl: 'canary_vision_doorbell_face.stl', frame: 'scad-wall',
    build: (E) => [
      slab(E, 'dark', 0, E.d, 0, 12),
      onFace(E, 'lens', E.w / 2, E.h - 22, 6, 1.2),
      onFace(E, 'accent', E.w / 2, 22, 6, 1.4),
    ],
  },
  {
    id: 'part.doorbell.gasket',
    title: 'Canary Vision Doorbell — TPU gasket',
    role: 'part', of: 'canary-vision',
    stl: 'canary_vision_doorbell_gasket.stl', frame: 'scad-wall',
    build: (E) => [slab(E, 'gasket', 0, E.d, 0, 11)],
  },
  {
    id: 'device.canary-vision-doorbell',
    title: 'Canary Vision Doorbell',
    role: 'device', of: 'canary-vision',
    parts: [
      'canary_vision_doorbell_plate.stl',
      'canary_vision_doorbell_body.stl',
      'canary_vision_doorbell_face.stl',
    ],
    frame: 'scad-wall',
    build: (E, P, A) => {
      const plate = P['canary_vision_doorbell_plate.stl'];
      const face = P['canary_vision_doorbell_face.stl'];
      const [s0] = A.seams;   // the wall plate's reveal; the face (the shell) rides from here out
      const fx = (E.w - face.w) / 2;
      // the piston plate: the body nests inside the face's walls, so the
      // side profile is the wall plate and then the face alone
      return [
        { kind: 'box', m: 'shell2', face: 'y', at: [(E.w - plate.w) / 2, 0, 0], size: [plate.w, s0, plate.h], r: 8 },
        { kind: 'box', m: 'dark', face: 'y', at: [fx, s0 - EPS, (E.h - face.h) / 2], size: [face.w, E.d - s0 + EPS, face.h], r: 12 },
        { kind: 'disc', m: 'lens', axis: 'y', at: [E.w / 2, E.d - EPS, E.h - 24], r: 6, h: 1.4 },
        { kind: 'disc', m: 'accent', axis: 'y', at: [E.w / 2, E.d - EPS, 24], r: 6, h: 1.6 },
      ];
    },
  },

  /* ══════════════════════════════════════════════ Canary Combo ══════ */
  {
    id: 'device.canary-combo',
    title: 'Canary Combo',
    // The radar + camera witness (canary_combo.scad, v0.1-dev): the Vision
    // stack beside the Sense stack in one housing, lens on the left column,
    // radome window on the right. In development, with no committed STLs, so
    // it is MEASURED off its CAD as seated (assembled_dims.json — the front
    // at z = base_d, where the file's own echo, head pads and lid key put
    // it: 86.4 x 73.6 x 26.38 today), not sketched from that echo. The lens
    // and the radome window are drawn at the variables front() cuts them at
    // (A.features: lens_x/lens_y, rad_cx/rad_cy, re-evaluated with the
    // envelope, not read off the cut), so a moved stack moves the drawing.
    // `of` is the catalog's own answer (the combo-witness variant is a
    // canary-vision build); the ladder reads THIS case's catalog entry, not
    // the Vision's released ones (gen_figures.mjs, catalogEvidence).
    role: 'device', of: 'canary-vision',
    assembled: true,
    frame: 'scad-wall',
    build: (E, P, A) => {
      const { lens, radome } = A.features;
      return [
        // the shell is the whole side profile: the plate nests inside its walls
        { kind: 'box', m: 'shell', face: 'y', at: [0, 0, 0], size: [E.w, E.d, E.h], r: 3 },
        // the lens in its aperture, and the radome window — the radar looks
        // through a blind thinning, invisible from outside, drawn proud here
        // exactly as the Sense figure draws its own
        { kind: 'disc', m: 'lens', axis: 'y', at: [lens.x, E.d - EPS, lens.z], r: lens.w / 2, h: 1.4 },
        {
          kind: 'box', m: 'radome', face: 'y',
          at: [radome.x - radome.w / 2, E.d - EPS, radome.z - radome.h / 2],
          size: [radome.w, 0.9, radome.h], r: 1.5,
        },
      ];
    },
  },

  /* ═════════════════════════════════════════════════ Canary WAP ══════ */
  {
    id: 'part.wap.base',
    title: 'Canary WAP — compact base',
    role: 'part', of: 'canary-wap',
    stl: 'canary_wap_enclosure_compact_base.stl', frame: 'scad-wall',
    build: (E) => [slab(E, 'shell2', 0, E.d, 0, 3)],
  },
  {
    id: 'part.wap.lid',
    title: 'Canary WAP — compact lid',
    role: 'part', of: 'canary-wap',
    stl: 'canary_wap_enclosure_compact_lid.stl', frame: 'scad-wall',
    build: (E) => [
      slab(E, 'shell', 0, E.d, 0, 3),
      onFace(E, 'accent', E.w / 2, E.h / 2, 2.2, 0.5, { detail: 'full' }),
    ],
  },
  {
    id: 'part.wap.battery-base',
    title: 'Canary WAP — battery base',
    role: 'part', of: 'canary-wap',
    stl: 'canary_wap_enclosure_battery_base.stl', frame: 'scad-wall',
    build: (E) => [slab(E, 'shell2', 0, E.d, 0, 3)],
  },
  {
    id: 'part.wap.weather-shield',
    title: 'Canary WAP — solar shield',
    role: 'part', of: 'canary-wap',
    stl: 'canary_wap_enclosure_weather_shield.stl', frame: 'scad-wall',
    build: (E) => [slab(E, 'shell2', 0, E.d, 0, 5)],
  },
  {
    id: 'part.wap.gasket',
    title: 'Canary WAP — TPU weather gasket',
    role: 'part', of: 'canary-wap',
    stl: 'canary_wap_enclosure_weather_gasket.stl', frame: 'scad-wall',
    build: (E) => [slab(E, 'gasket', 0, E.d, 0, 3)],
  },
  {
    id: 'part.wap.tray',
    title: 'Canary WAP — desiccant tray',
    role: 'part', of: 'canary-wap',
    stl: 'canary_wap_enclosure_tray.stl', frame: 'scad-wall',
    build: (E) => [slab(E, 'shell2', 0, E.d, 0, 1)],
  },
  {
    id: 'device.canary-wap',
    title: 'Canary WAP',
    role: 'device', of: 'canary-wap',
    parts: ['canary_wap_enclosure_compact_base.stl', 'canary_wap_enclosure_compact_lid.stl'],
    frame: 'scad-wall',
    build: (E, P, A) => {
      // the piston plate: the lid (the shell) is the whole side profile —
      // the base (the plate) nests inside its walls
      return [
        { kind: 'box', m: 'shell', face: 'y', at: [0, 0, 0], size: [E.w, E.d, E.h], r: 3 },
        { kind: 'disc', m: 'accent', axis: 'y', at: [E.w / 2, E.d - EPS, E.h / 2], r: 2.2, h: 0.6, detail: 'full' },
      ];
    },
  },

  /* ═══════════════════════════════════════════ tools you print ═══════ */
  {
    id: 'tool.fit-coupon',
    title: 'Fit coupon',
    role: 'tool', of: '_universal',
    stl: 'canary_wap_enclosure_clip_coupon.stl', frame: 'scad-wall',
    build: (E) => [
      // base plate + the mating boss you push into it: the whole coupon is
      // "does this snap fit on YOUR printer", so the joint is the figure
      slab(E, 'shell2', 0, E.d * 0.55, 0, 2),
      { kind: 'box', m: 'accent', face: 'y', at: [E.w * 0.2, E.d * 0.55 - EPS, E.h * 0.25], size: [E.w * 0.6, E.d * 0.45 + EPS, E.h * 0.5], r: 1.5 },
    ],
  },

  /* ═════════════════════════════════════════ bought-in modules ═══════
   * Boards are figured because half of what a user is told to do ("plug the
   * XIAO into the back socket") is about a board, and a name alone is exactly
   * the ambiguity these figures exist to remove.
   *
   * We do not own their CAD — but for most of them we have committed the
   * vendor's STEP and tessellated it (canary-local/devices/boards.json), so
   * `board:` names that entry and the generator takes the envelope from the
   * vendor geometry, exactly as a printed part takes it from its STL. A board
   * with no committed vendor CAD falls back to a `sketch` and the ledger says
   * so, the same as anywhere else. */
  {
    id: 'board.xiao',
    title: 'Seeed XIAO (ESP32 family)',
    role: 'board', of: '_universal', supplier: 'seeed',
    board: 'seeed_xiao_esp32s3',
    build: (E) => [
      // the PCB, the shielded module can, and the USB-C tongue at one end.
      // The can is the deepest thing on the board, so it is what reaches the
      // CAD's full depth — the drift guard checks exactly that.
      slab(E, 'board', 0, E.d * 0.34, 0, 1),
      { kind: 'box', m: 'metal', face: 'y', at: [E.w * 0.28, E.d * 0.34, E.h * 0.2], size: [E.w * 0.44, E.d * 0.66, E.h * 0.6], r: 0.4 },
      { kind: 'box', m: 'metal', face: 'y', at: [E.w / 2 - 4.4, E.d * 0.34, 0], size: [8.8, E.d * 0.45, 3.2], r: 0.6, detail: 'full' },
    ],
  },
  {
    id: 'board.grove-vision-ai-v2',
    title: 'Grove Vision AI V2',
    role: 'board', of: 'canary-vision', supplier: 'seeed',
    board: 'seeed_grove_vision_ai_v2',
    build: (E) => [
      slab(E, 'board', 0, E.d * 0.34, 0, 1),
      { kind: 'box', m: 'dark', face: 'y', at: [E.w * 0.3, E.d * 0.34, E.h * 0.25], size: [E.w * 0.4, E.d * 0.66, E.h * 0.5], r: 0.4 },
      // the CSI flex connector along the top edge — how the camera attaches
      { kind: 'box', m: 'metal', face: 'y', at: [2, E.d * 0.34, E.h - 3.2], size: [E.w - 4, E.d * 0.45, 3.2], r: 0.4, detail: 'full' },
    ],
  },
  {
    id: 'board.mr60bha2',
    title: 'Seeed MR60BHA2 60 GHz radar',
    role: 'board', of: 'canary-sense', supplier: 'seeed',
    // No committed vendor STEP for this one yet — sketched from the module's
    // published 24 x 24 mm outline, and the ledger records it as a sketch.
    sketch: { w: 24.0, d: 3.2, h: 24.0 },
    sketchNote: 'the published 24 x 24 mm module outline; no vendor CAD committed',
    build: (E) => [
      slab(E, 'board', 0, E.d * 0.4, 0, 1),
      // the antenna array is the whole point of this board — figure it
      { kind: 'box', m: 'metal', face: 'y', at: [E.w * 0.18, E.d * 0.4, E.h * 0.18], size: [E.w * 0.64, E.d * 0.6, E.h * 0.64], r: 0.3 },
    ],
  },
  {
    id: 'board.round-display',
    title: 'Seeed Round Display for XIAO',
    role: 'board', of: 'canary-display', supplier: 'seeed',
    board: 'seeed_round_display_xiao',
    build: (E) => [
      { kind: 'cyl', m: 'board', axis: 'y', at: [E.w / 2, 0, E.h / 2], r: E.w / 2, h: E.d * 0.7 },
      { kind: 'cyl', m: 'glass', axis: 'y', at: [E.w / 2, E.d * 0.7, E.h / 2], r: E.w / 2 - 0.6, h: E.d * 0.3 - 0.4 },
      { kind: 'cyl', m: 'lit', axis: 'y', at: [E.w / 2, E.d - 0.4, E.h / 2], r: E.w / 2 - 2.4, h: 0.4 },
    ],
  },

  /* ══════════════════════════════════════════════════ displays ═══════
   * The display line has no committed STLs (the enclosures are still in
   * development — dev_*.stl is gitignored on purpose). Where the case CAD
   * states its own seat, the figure is MEASURED off that CAD anyway
   * (`assembled: true` — gen_assembled_dims.py renders the parts from the
   * .scad and commits the union's bounds), so a knob edit moves the figure;
   * the rest are sketched from their panel records. Either way they trace
   * to no committed STL, so they are marked prototype, not shipping, and
   * the ledger says so. */
  {
    id: 'device.canary-display-watch',
    title: 'Canary Watch Station',
    role: 'device', of: 'canary-display-watch',
    // v0.2 CAD (canary_watch_station.scad), measured: drum + snap bezel as
    // seated (assembled_dims.json — Ø49.0 x 23.19 today: drum 21.0 + bezel
    // face 2.2, less the 0.01 its edge chamfer overlaps), and the glass in
    // the measured face aperture (bez_ap_d, A.face). The v0.1 "screwed drum"
    // sketch said Ø52 x 21.8, dimensions the measured board could never
    // seat; the v0.2 sketch that replaced it was typed from a comment, so a
    // disc_d edit in the manifest moved the case and not this. The stand is
    // its own part, not the puck's envelope.
    assembled: true,
    frame: 'scad-wall',
    build: (E, P, A) => {
      const [s0] = A.seams;   // the drum rim: the bezel face rides from here out
      const ap = A.face.w / 2; // the bezel's aperture radius (bez_ap_d / 2), measured
      return [
        // the drum, back cap to rim, and the snap-bezel face riding on it
        { kind: 'cyl', m: 'shell2', axis: 'y', at: [E.w / 2, 0, E.h / 2], r: E.w / 2, h: s0 },
        { kind: 'cyl', m: 'shell', axis: 'y', at: [E.w / 2, s0 - EPS, E.h / 2], r: E.w / 2, h: E.d - s0 + EPS },
        // the round glass in the bezel's aperture, and the lit face on it —
        // the 2.2 lit inset is a drawing choice (no CAD number: the panel's
        // active area is not in the case file), sized to read at glyph scale
        { kind: 'cyl', m: 'glass', axis: 'y', at: [E.w / 2, E.d - EPS, E.h / 2], r: ap, h: 0.4 },
        { kind: 'cyl', m: 'lit', axis: 'y', at: [E.w / 2, E.d + 0.4 - EPS, E.h / 2], r: ap - 2.2, h: 0.4 },
      ];
    },
  },
  {
    id: 'device.canary-display-touch169',
    title: 'Canary Nightstand Touch',
    role: 'device', of: 'canary-display-touch169',
    // The panel record in canary_s3_touch169.scad: bonded glass 41.13 x 33.13,
    // 2.6 mm at the edge, on a 37.12 x 29.83 PCB. The case around it is still
    // in development (its STL is gitignored), so the wall is the sketch part.
    sketch: { w: 46.13, d: 17, h: 38.13 },
    sketchNote: 'the 1.69" panel record in canary_s3_touch169.scad (glass 41.13 x 33.13) '
      + 'plus a 2.5 mm bezel wall; no committed case STL yet',
    build: (E) => [
      { kind: 'box', m: 'shell', face: 'y', at: [0, 0, 0], size: [E.w, E.d - 2.6, E.h], r: 3 },
      { kind: 'box', m: 'glass', face: 'y', at: [2.5, E.d - 2.6, 2.5], size: [41.13, 2.2, 33.13], r: 2 },
      { kind: 'box', m: 'lit', face: 'y', at: [5, E.d - 0.4, 5], size: [36.13, 0.4, 28.13], r: 1.5 },
    ],
  },
  {
    id: 'device.canary-display-amoled241',
    title: 'Canary Glance AMOLED',
    role: 'device', of: 'canary-display-amoled241',
    // The vendor's own metal case around the 2.41" 450x600 AMOLED. The
    // active area is computed from the module (61.2 mm diagonal at 3:4 →
    // 36.7 x 49.0); the case envelope is an ESTIMATE from vendor photos —
    // no vendor case CAD is committed, so the wall is the sketch part.
    sketch: { w: 46, d: 13, h: 62 },
    sketchNote: 'the 2.41" 450x600 module (active 36.7 x 49.0, computed from the '
      + '61.2 mm diagonal) inside the vendor metal case, whose envelope is an '
      + 'estimate from vendor photos; no committed case CAD — VERIFY with calipers',
    build: (E) => [
      { kind: 'box', m: 'shell', face: 'y', at: [0, 0, 0], size: [E.w, E.d - 2.4, E.h], r: 3 },
      { kind: 'box', m: 'glass', face: 'y', at: [2, E.d - 2.4, 3.2], size: [42, 2.0, 55.6], r: 2 },
      { kind: 'box', m: 'lit', face: 'y', at: [4.65, E.d - 0.4, 6.5], size: [36.7, 0.4, 49.0], r: 1.5 },
    ],
  },
  {
    id: 'device.canary-display-dash7',
    title: 'Canary Dash 7',
    role: 'device', of: 'canary-display-dash7',
    // The two 7" boards share their whole mechanical interface to the
    // hundredth (canary_s3_lcd7.scad, "Panel variant"): glass 192.96 x 110.76,
    // active area 154.88 x 86.72, PCB 165.72 x 97.60.
    sketch: { w: 204.96, d: 22, h: 122.76 },
    sketchNote: 'the 7" panel record in canary_s3_lcd7.scad (glass 192.96 x 110.76, '
      + 'active 154.88 x 86.72) plus a 6 mm bezel wall; no committed case STL yet',
    build: (E) => [
      { kind: 'box', m: 'shell', face: 'y', at: [0, 0, 0], size: [E.w, E.d - 3, E.h], r: 5 },
      { kind: 'box', m: 'glass', face: 'y', at: [6, E.d - 3, 6], size: [192.96, 2.6, 110.76], r: 3 },
      { kind: 'box', m: 'lit', face: 'y', at: [25, E.d - 0.4, 18], size: [154.88, 0.4, 86.72], r: 2 },
    ],
  },
  {
    id: 'device.canary-display-dash',
    title: 'Canary Dash',
    role: 'device', of: 'canary-display-dash',
    // The printed Dash case (canary_dash_display.scad), measured off its CAD
    // as assembled (assembled_dims.json): the back with its four dock pads
    // on the wall face, the frame seated on it. This used to be drawn from
    // the vendor board mesh (`board: 'waveshare_4_3b'`, 118 x 79 x 38.9 —
    // the Waveshare board with its own case, not ours), while the Lab's
    // registry typed a third number for the case (113.7 x 73.6 x 16.0) that
    // had lost its corner lobes, its thicker back and its pads. One source
    // now: the case the Dash is sold to be printed in.
    assembled: true,
    frame: 'scad-wall',
    build: (E, P, A) => {
      const [s0, s1] = A.seams;   // pad tips -> back plate -> frame
      // the view window the bezel lip frames (view_l x view_w, measured —
      // A.face), centered on the case as the CAD cuts it: the glass is the
      // whole story from across the room, so it is the CAD's number
      const { w: vw, h: vh } = A.face;
      const wx = (E.w - vw) / 2, wz = (E.h - vh) / 2;
      return [
        // the dock pads' shadow gap: four low pads, massed as one inset block
        // — what holds the case off the wall, not a thicker back. Only its
        // depth band (s0, the pad height) is the CAD's; the block's footprint
        // is a drawing choice, roughly the cradle_dx x cradle_dy pad span
        { kind: 'box', m: 'dark', face: 'y', at: [E.w * 0.18, 0, E.h * 0.22], size: [E.w * 0.64, s0 + EPS, E.h * 0.56], r: 3 },
        { kind: 'box', m: 'shell2', face: 'y', at: [0, s0, 0], size: [E.w, s1 - s0 + EPS, E.h], r: 5 },
        { kind: 'box', m: 'shell', face: 'y', at: [0, s1, 0], size: [E.w, E.d - s1, E.h], r: 5 },
        { kind: 'box', m: 'glass', face: 'y', at: [wx, E.d - EPS, wz], size: [vw, 0.4, vh], r: 2 },
        // the lit area 3 inside the window: a drawing choice (the panel's
        // active area is not in the case file)
        { kind: 'box', m: 'lit', face: 'y', at: [wx + 3, E.d + 0.4 - EPS, wz + 3], size: [vw - 6, 0.4, vh - 6], r: 1.5 },
      ];
    },
  },
  {
    id: 'device.canary-display-nightstand',
    title: 'Canary Nightstand',
    role: 'device', of: 'canary-display-nightstand-s3',
    // The S3-LCD-1.47 hallway body (canary_s3_lcd147.scad): a USB-A stick that
    // plugs straight into a wall outlet, portrait glass on its face. Every
    // number below is the SCAD's own derived outer shell, which it echoes at
    // render time — "Outer shell: xo x yo x bez_h mm (the back plate seats
    // FLUSH with the rim, so the bezel height is the whole thickness); plug
    // adds usb_proud mm" — rather than a proportion guessed to look right:
    //   xo    = board_w 20.32 + 2*tol_slide 0.20 + 2*wall 2.1 = 24.92
    //   yo    = board_l 36.37 + 2*tol_slide 0.20 + 2*wall 2.1 = 40.97
    //   bez_h = face_t 1.0 + cav_d 8.20                       =  9.20
    // The case is still in development (its STL export is gitignored, like the
    // rest of the display line), so this is a sketch and the ledger says so.
    sketch: { w: 24.92, d: 9.2, h: 40.97 },
    sketchNote: 'the S3-LCD-1.47 wall body in canary_s3_lcd147.scad: outer shell '
      + '24.92 x 40.97 (board cavity + 2.1 mm walls), 9.20 mm thick (the back '
      + 'plate seats flush with the rim), plus the 12.2 mm of USB-A plug that '
      + 'stands clear of the plug-end wall; no committed case STL yet',
    build: (E) => {
      // The plug is not a detail — it is 12.2 mm on a 41 mm body and the
      // reason this product is shaped the way it is (it hangs off an outlet
      // rather than standing on a surface). A figure that left it off would
      // read as a different, free-standing device.
      const usbW = 12.0;                 // series-A shell width  (usb_shell_w)
      const usbH = 4.5;                  // series-A shell height (usb_shell_h)
      const usbFree = 12.2;              // usb_proud 14.0 - usb_wall 1.8
      // The shell straddles the PCB, so it centers on the board's mid-plane:
      // z_usb = face_t + lcd_rise + pcb_t/2 back from the face.
      const usbY = E.d - (1.0 + 3.65 + 1.6 / 2) - usbH / 2;
      return [
        { kind: 'box', m: 'shell', face: 'y', at: [0, 0, 0], size: [E.w, E.d - 1.0, E.h], r: 3.2 },
        // the plug, below the body — its own material so the metal shell does
        // not read as more case
        { kind: 'box', m: 'dark', face: 'y', at: [(E.w - usbW) / 2, usbY, -usbFree],
          size: [usbW, usbH, usbFree + EPS] },
        // the 1.47" portrait window: the bezel face overlaps the module
        // border (lcm 19.39 x 36.28) and the window shows the active area
        // (aa 17.39 x 32.35)
        { kind: 'box', m: 'glass', face: 'y', at: [(E.w - 19.39) / 2, E.d - 1.0, (E.h - 36.28) / 2],
          size: [19.39, 0.9, 36.28], r: 1.5 },
        { kind: 'box', m: 'lit', face: 'y', at: [(E.w - 17.39) / 2, E.d - 0.4, (E.h - 32.35) / 2],
          size: [17.39, 0.4, 32.35], r: 1 },
      ];
    },
  },
  {
    id: 'device.canary-display-nightstand-c6',
    title: 'Canary Nightstand C6',
    role: 'device', of: 'canary-display-nightstand-c6',
    // The C6-LCD-1.47 pocket case (canary_c6_display.scad) in the build the
    // file defaults to and its Lab preview renders: headers="none", the
    // stripped board. The case is still in development (its dev_*.stl export
    // is gitignored), so the wall is the sketch part; the numbers are the
    // SCAD's own derived ones (its echo: "outer 25.12 x 41.17 x 14.05"):
    //   xo    = board_w 20.32 + 2*tol_slide 0.20 + 2*wall 2.2       = 25.12
    //   yo    = board_l 36.37 + 2*tol_slide 0.20 + 2*wall 2.2       = 41.17
    //   depth = face_t 2.0 + lcd_rise 3.65 + pcb_t 1.6 + back_stack 4.8
    //           + back_t 2.0                                          = 14.05
    // What makes it read as this case and not its 1.47 siblings is its
    // outline: the "ears" (the BOOT/RST channels bulge each side wall) and
    // the "chin" (the back-mounted USB-C shell's overhang bulges the bottom
    // wall). No plug stands off it — the S3 stick ends in a series-A plug,
    // this board takes its cable in a socket.
    // The C6 manifest does not name this figure yet: naming it maps the C6
    // board in the firmware's figure table (fleet_figures.h, and a
    // CANARY_FIGURE_HARDWARE line in the board's pins.h) — a firmware change
    // of its own. Today it draws the Lab card.
    sketch: { w: 25.12, d: 14.05, h: 41.17 },
    sketchNote: 'the C6-LCD-1.47 pocket case in canary_c6_display.scad, stripped-board build '
      + '(headers="none", the file default): shell 25.12 x 41.17 (board cavity + 2.2 mm walls), '
      + '14.05 mm deep (bezel 12.05 + back 2.0), plus the 1.0 mm button ears and the 1.1 mm '
      + 'USB-C chin; no committed case STL yet',
    build: (E) => {
      const backT = 2.0;       // back_t — the snap-on back, its own print
      const earBump = 1.0;     // ear_bump = btn_proud 1.8 + tol_slide 0.2 + ear_skin 1.2 - wall 2.2
      const earW = 7.4;        // ear_w = btn_ch_w 3.4 + 4
      const earZ = 7.0 + 2.4;  // btn_up 7.0 from the board's USB end, which sits tol_slide + wall up
      const chin = 1.1;        // chin_bump = usb_proud 1.9 + tol_slide 0.2 + ear_skin 1.2 - wall 2.2
      const chinW = 13.15;     // chin_w = usb_shell_w 9.15 + 4
      const faceY = E.d - 1.0; // the glass stands where its 1.47 siblings' does
      return [
        { kind: 'box', m: 'shell2', face: 'y', at: [0, 0, 0], size: [E.w, backT, E.h], r: 3 },
        { kind: 'box', m: 'shell', face: 'y', at: [0, backT - EPS, 0], size: [E.w, faceY - backT + EPS, E.h], r: 3 },
        // the two ears and the chin, bezel bulges the whole bezel deep
        { kind: 'box', m: 'shell', face: 'y', at: [-earBump, backT, earZ - earW / 2], size: [earBump + EPS, faceY - backT, earW] },
        { kind: 'box', m: 'shell', face: 'y', at: [E.w - EPS, backT, earZ - earW / 2], size: [earBump + EPS, faceY - backT, earW] },
        { kind: 'box', m: 'shell', face: 'y', at: [(E.w - chinW) / 2, backT, -chin], size: [chinW, faceY - backT, chin + EPS] },
        // the 1.47" module under the face (lcm 19.39 x 36.28) and the window
        // onto its active area (aa 17.39 x 32.35), centered as the bezel cuts it
        { kind: 'box', m: 'glass', face: 'y', at: [(E.w - 19.39) / 2, faceY, (E.h - 36.28) / 2], size: [19.39, 0.9, 36.28], r: 1.5 },
        { kind: 'box', m: 'lit', face: 'y', at: [(E.w - 17.39) / 2, E.d - 0.4, (E.h - 32.35) / 2], size: [17.39, 0.4, 32.35], r: 1 },
      ];
    },
  },
  {
    id: 'device.canary-nightlight',
    title: 'Canary Nightlight',
    role: 'device', of: 'canary-nightlight',
    // The C3-LCD-1.47 pocket case (canary_c3_lcd147.scad): one YELLOW case —
    // the print contract is slot 1 = BODY, bezel AND lid, with black reserved
    // for the wordmark on the back, which this camera never sees — with the
    // portrait glass high on its face and a white light band wrapping the
    // glass end, up both long walls and around the top. The case is still in
    // development (its dev_*.stl export is gitignored on purpose), so the
    // wall is the sketch part; the plan numbers are the SCAD's own derived
    // outer box, at the depth of the AS-SHIPPED pillars build.
    sketch: { w: 25.12, d: 14.25, h: 41.17 },
    sketchNote: 'the C3-LCD-1.47 pocket case in canary_c3_lcd147.scad: shell '
      + '25.12 x 41.17 (board cavity + 2.2 mm walls), 14.25 mm deep as shipped '
      + '(the pillars build: face + glass rise + PCB + brass pillars and their '
      + 'gap + the lid plate); no committed case STL yet',
    build: (E) => {
      const bandT = 2.8;        // seam_h — the white U's thickness
      const bandY = E.d - 4.4;  // just behind the glass face, where the light lives
      const proud = 0.55;       // the band stands clear of the walls it wraps
      return [
        { kind: 'box', m: 'accent', face: 'y', at: [0, 0, 0], size: [E.w, E.d - 1.3, E.h], r: 3 },
        // ONE U, massed as three proud sticks (left, right, across the top) so
        // no white face ever ties with a yellow one. The USB end stays plain.
        { kind: 'box', m: 'shell', face: 'y', at: [-proud, bandY, 3], size: [proud + EPS, bandT, E.h - 3 - EPS] },
        { kind: 'box', m: 'shell', face: 'y', at: [E.w - EPS, bandY, 3], size: [proud + EPS, bandT, E.h - 3 - EPS] },
        { kind: 'box', m: 'shell', face: 'y', at: [-proud, bandY, E.h - EPS], size: [E.w + 2 * proud, bandT, proud + EPS] },
        // the 1.47" portrait window, high on the front — the clock face,
        // seated in the depth the way the other displays' glass is
        { kind: 'box', m: 'glass', face: 'y', at: [(E.w - 19.4) / 2, E.d - 1.3, E.h - 37.4], size: [19.4, 0.9, 34.4], r: 1.5 },
        { kind: 'box', m: 'lit', face: 'y', at: [(E.w - 17.4) / 2, E.d - 0.4, E.h - 36.4], size: [17.4, 0.4, 32.4], r: 1 },
      ];
    },
  },
];

/* ─────────────────────────────────────────────────────────────── concepts
 * Everything in registry.json whose `kind` is "concept" gets a figure too —
 * because the whole point of the ladder is that an idea is VISIBLE, sitting
 * next to the shipping devices in the same catalog, drawn from the same
 * camera, and unmistakably not a product. Their massing is a one-line
 * proportion sketch: honest about being a sketch, and enough to carry "this
 * one is a post, this one is a puck, this one clips to a collar."
 *
 * The generator turns these into ghost figures (dashed wireframe, no fill,
 * no shadow). See docs/design/FLEET_FIGURES.md §"The confidence ladder". */

export const CONCEPT_MASSING = {
  'canary-fence-guard': { w: 44, d: 30, h: 96, note: 'post-mounted node with a stub antenna' },
  'canary-guardian': { w: 40, d: 28, h: 78, note: 'low-power sibling of the fence node' },
  'canary-ranger': { w: 52, d: 34, h: 84, note: 'radar head over a battery body' },
  'canary-feeder': { w: 120, d: 90, h: 150, note: 'perch + hopper' },
  'canary-litter': { w: 300, d: 220, h: 40, note: 'load-cell mat under the tray' },
  'canary-paw': { w: 70, d: 26, h: 70, note: 'trained paw button' },
  'canary-clime': { w: 56, d: 26, h: 56, note: 'air-quality puck' },
  'canary-hearth': { w: 46, d: 34, h: 46, note: 'thermal eye' },
  'canary-chore': { w: 34, d: 14, h: 34, note: 'stick-on accelerometer tag' },
  'canary-poolwatch': { w: 62, d: 40, h: 96, note: 'poolside camera post' },
  'canary-curbwatch': { w: 96, d: 52, h: 120, note: 'radar + starlight camera on a solar mast' },
  'canary-gatekeeper': { w: 38, d: 18, h: 62, note: 'gate tag' },
  'canary-vision-pro': { w: 46, d: 46, h: 62, note: 'reCamera Pro in a printed sleeve' },
  'canary-vision-lite': { w: 40, d: 40, h: 52, note: 'reCamera 2002w in a printed sleeve' },
  'canary-vehicle-guard': { w: 44, d: 22, h: 66, note: 'in-cabin IMU node' },
  'canary-vehicle': { w: 92, d: 62, h: 30, note: 'SBC + CAN hat' },
};

export function conceptFigure(id, meta) {
  const s = CONCEPT_MASSING[id];
  if (!s) return null;
  return {
    id: `device.${id}`,
    title: meta.name || id,
    role: 'device',
    of: id,
    sketch: { w: s.w, d: s.d, h: s.h },
    sketchNote: s.note,
    build: (E) => [{
      kind: 'box', m: 'shell', face: 'y', at: [0, 0, 0],
      size: [E.w, E.d, E.h], r: Math.min(E.w, E.h) * 0.18,
    }],
  };
}
