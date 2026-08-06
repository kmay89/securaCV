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
    build: (E, P) => {
      const back = P['canary_sense_back.stl'];
      const front = P['canary_sense_front.stl'];
      const fx = (back.w - front.w) / 2;
      return [
        { kind: 'box', m: 'shell2', face: 'y', at: [0, 0, 0], size: [back.w, back.d, back.h], r: 2 },
        { kind: 'box', m: 'shell', face: 'y', at: [fx, back.d - EPS, E.h - front.h], size: [front.w, front.d + EPS, front.h], r: 3 },
        {
          kind: 'box', m: 'radome', face: 'y',
          at: [E.w / 2 - 12, back.d + front.d - EPS, E.h - front.h / 2 - 12],
          size: [24, 0.9, 24], r: 1.5,
        },
        { kind: 'disc', m: 'accent', axis: 'y', at: [E.w / 2, back.d + front.d + 0.9 - EPS, E.h - front.h + 6], r: 2, h: 0.6, detail: 'full' },
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
    build: (E, P) => {
      const back = P['canary_vision_enclosure_xiao_indoor_back.stl'];
      const front = P['canary_vision_enclosure_xiao_indoor_front.stl'];
      const top = E.h;
      return [
        { kind: 'box', m: 'shell2', face: 'y', at: [0, 0, 0], size: [back.w, back.d, back.h], r: 2 },
        { kind: 'box', m: 'shell', face: 'y', at: [0, back.d - EPS, top - front.h], size: [front.w, front.d + EPS, front.h], r: 3 },
        { kind: 'disc', m: 'lens', axis: 'y', at: [E.w / 2, back.d + front.d - EPS, top - 14], r: 5.5, h: 1.4 },
        { kind: 'disc', m: 'accent', axis: 'y', at: [E.w / 2, back.d + front.d - EPS, top - front.h + 8], r: 1.8, h: 0.5, detail: 'full' },
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
    build: (E, P) => {
      const back = P['canary_vision_enclosure_devkit_indoor_back.stl'];
      const front = P['canary_vision_enclosure_devkit_indoor_front.stl'];
      return [
        { kind: 'box', m: 'shell2', face: 'y', at: [0, 0, 0], size: [back.w, back.d, back.h], r: 2 },
        { kind: 'box', m: 'shell', face: 'y', at: [0, back.d - EPS, E.h - front.h], size: [front.w, front.d + EPS, front.h], r: 3 },
        { kind: 'disc', m: 'lens', axis: 'y', at: [E.w / 2, back.d + front.d - EPS, E.h - 14], r: 5.5, h: 1.4 },
        { kind: 'disc', m: 'accent', axis: 'y', at: [E.w / 2, back.d + front.d - EPS, E.h - front.h + 8], r: 1.8, h: 0.5, detail: 'full' },
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
    build: (E, P) => {
      const plate = P['canary_vision_doorbell_plate.stl'];
      const body = P['canary_vision_doorbell_body.stl'];
      const face = P['canary_vision_doorbell_face.stl'];
      const bx = (E.w - body.w) / 2;
      const fx = (E.w - face.w) / 2;
      return [
        { kind: 'box', m: 'shell2', face: 'y', at: [(E.w - plate.w) / 2, 0, 0], size: [plate.w, plate.d, plate.h], r: 8 },
        { kind: 'box', m: 'shell', face: 'y', at: [bx, plate.d - EPS, (E.h - body.h) / 2], size: [body.w, body.d + EPS, body.h], r: 12 },
        { kind: 'box', m: 'dark', face: 'y', at: [fx, plate.d + body.d - EPS, (E.h - face.h) / 2], size: [face.w, face.d + EPS, face.h], r: 12 },
        { kind: 'disc', m: 'lens', axis: 'y', at: [E.w / 2, plate.d + body.d + face.d - EPS, E.h - 24], r: 6, h: 1.4 },
        { kind: 'disc', m: 'accent', axis: 'y', at: [E.w / 2, plate.d + body.d + face.d - EPS, 24], r: 6, h: 1.6 },
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
    build: (E, P) => {
      const base = P['canary_wap_enclosure_compact_base.stl'];
      const lid = P['canary_wap_enclosure_compact_lid.stl'];
      return [
        { kind: 'box', m: 'shell2', face: 'y', at: [0, 0, 0], size: [base.w, base.d, base.h], r: 3 },
        { kind: 'box', m: 'shell', face: 'y', at: [0, base.d - EPS, 0], size: [lid.w, lid.d + EPS, lid.h], r: 3 },
        { kind: 'disc', m: 'accent', axis: 'y', at: [E.w / 2, base.d + lid.d - EPS, E.h / 2], r: 2.2, h: 0.6, detail: 'full' },
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
   * development — dev_*.stl is gitignored on purpose), so these are sketched
   * from registry.json's `body_mm` and `glass`. They are marked prototype,
   * not shipping, and the ledger says so. */
  {
    id: 'device.canary-display-watch',
    title: 'Canary Watch Station',
    role: 'device', of: 'canary-display-watch',
    sketch: { w: 52, d: 21.8, h: 52 },
    build: (E) => [
      // a screwed drum looking at you: shell, bezel-inset glass, lit face
      { kind: 'cyl', m: 'shell', axis: 'y', at: [E.w / 2, 0, E.h / 2], r: E.w / 2, h: E.d - 2.4 },
      { kind: 'cyl', m: 'glass', axis: 'y', at: [E.w / 2, E.d - 2.4 - EPS, E.h / 2], r: E.w / 2 - 3.5, h: 2.0 },
      { kind: 'cyl', m: 'lit', axis: 'y', at: [E.w / 2, E.d - 0.4, E.h / 2], r: E.w / 2 - 7, h: 0.4 },
    ],
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
    board: 'waveshare_4_3b',
    build: (E) => [
      { kind: 'box', m: 'shell', face: 'y', at: [0, 0, 0], size: [E.w, E.d * 0.7, E.h], r: 4 },
      { kind: 'box', m: 'glass', face: 'y', at: [3, E.d * 0.7, 3], size: [E.w - 6, E.d * 0.3 - 0.4, E.h - 6], r: 2.5 },
      { kind: 'box', m: 'lit', face: 'y', at: [6.5, E.d - 0.4, 6.5], size: [E.w - 13, 0.4, E.h - 13], r: 1.5 },
    ],
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
