// canary-local/tests/turned_glass.mjs — the turned glasses the browser probes
// boot (F184, F204, F206), read from the sources, never typed in a probe.
//
// Two flavors wear a saved rotation from their first frame (main.cpp turns
// the glass after lvgl_port_init and before the splash):
//   * the dash, in software (lvgl_port_set_rotation): glass_settings.h's
//     ROT_PORTRAIT turns its 800x480 panel to the 480x800 portrait glass;
//   * the nightlight, in hardware (display_set_rotation, a MADCTL write, and
//     lvgl_port_set_panel_rotation): io/orientation.h's Orient::R90 stands
//     its 180x320 panel on its edge as the 320x180 landscape glass.
// The panel is the one build.sh wires for the flavor (its pin map's
// LCD_WIDTH x LCD_HEIGHT) and a quarter turn swaps its sides. `corners` says
// whether the Join scene's QR card's rounded corners are held inside the
// halo's stroke: the landscape nightlight's layout keeps them 2 px inside
// (F157); the 800 px dash glass's reach past its ring in either orientation
// (F155, an open decision), so there they are printed, not held.
//
// Pure functions over the source texts, so onboard.test.js holds them on the
// real tree and on edited copies; onboard_probe.mjs and boot_probe.mjs call
// turnedGlasses() with the files read from disk (readTurnedSources).

/**
 * The board build.sh compiles a display flavor against: the boards/<id> of
 * the PINS_DIR in that flavor's branch of the wiring block. The dash is the
 * block's `else` (no branch names it). Returns the board id, or null.
 */
export function flavorBoard(buildSh, flavor) {
  if (flavor === "dash") {
    return /\belse\s+PINS_DIR="\$FW\/boards\/([^"]+)\/pins"\s+CFG_DIR="\$FW\/configs\/canary-display\/dash"/
      .exec(buildSh)?.[1] ?? null;
  }
  const esc = flavor.replace(/[^a-z0-9]/g, "");
  const branch = new RegExp(
    `(?:^|\\n)\\s*(?:el)?if \\[\\[ "\\$FLAVOR" == "${esc}" \\]\\]; then\\n((?:[^\\n]*\\n)*?)\\s*(?:elif|else|fi)\\b`)
    .exec(buildSh)?.[1] ?? "";
  return /PINS_DIR="\$FW\/boards\/([^"]+)\/pins"/.exec(branch)?.[1] ?? null;
}

/** The panel a pin map names: {w, h} from LCD_WIDTH / LCD_HEIGHT, or null. */
export function pinsPanel(pinsH) {
  const w = Number(/^#define LCD_WIDTH\s+(\d+)/m.exec(pinsH)?.[1]);
  const h = Number(/^#define LCD_HEIGHT\s+(\d+)/m.exec(pinsH)?.[1]);
  return w > 0 && h > 0 ? { w, h } : null;
}

/**
 * Every turned glass the probes walk, from the sources:
 *   buildSh       canary-local/emulator/build.sh
 *   glassSettings firmware/projects/canary-display/include/canary/glass_settings.h
 *   orientation   firmware/projects/canary-display/include/canary/io/orientation.h
 *   pinsH(board)  the text of firmware/boards/<board>/pins/pins.h
 * Returns [{flavor, rotation, name, panel, glass, corners}]; throws naming
 * the first fact it cannot read.
 */
export function turnedGlasses({ buildSh, glassSettings, orientation, pinsH }) {
  const turn = (flavor, rotation, name, corners) => {
    const board = flavorBoard(buildSh, flavor);
    if (!board) throw new Error(`build.sh wires no pin map for the ${flavor} flavor`);
    const panel = pinsPanel(pinsH(board));
    if (!panel) throw new Error(`boards/${board}/pins/pins.h names no LCD_WIDTH/LCD_HEIGHT`);
    // A quarter turn swaps the panel's sides.
    const glass = rotation % 2 ? { w: panel.h, h: panel.w } : { ...panel };
    return { flavor, rotation, name, panel, glass, corners };
  };
  const portrait = Number(/\bROT_PORTRAIT\s*=\s*(\d+)/.exec(glassSettings)?.[1]);
  if (!(portrait >= 0)) throw new Error("glass_settings.h names no ROT_PORTRAIT");
  const landscape = Number(/\bR90\s*=\s*(\d+)/.exec(orientation)?.[1]);
  if (!(landscape >= 0)) throw new Error("io/orientation.h names no Orient::R90");
  return [
    turn("dash", portrait, "portrait", false),
    turn("nightlight", landscape, "landscape", true),
  ];
}

/** Read the sources turnedGlasses() takes, from a checkout at `root`. */
export async function readTurnedSources(root, readFile) {
  const read = (rel) => readFile(`${root}/${rel}`, "utf8");
  const [buildSh, glassSettings, orientation] = await Promise.all([
    read("canary-local/emulator/build.sh"),
    read("firmware/projects/canary-display/include/canary/glass_settings.h"),
    read("firmware/projects/canary-display/include/canary/io/orientation.h"),
  ]);
  const pins = {};
  for (const flavor of ["dash", "nightlight"]) {
    const board = flavorBoard(buildSh, flavor);
    if (board) pins[board] = await read(`firmware/boards/${board}/pins/pins.h`);
  }
  return { buildSh, glassSettings, orientation, pinsH: (board) => pins[board] ?? "" };
}

/**
 * The first meeting's lines (F206): every line kHello types into the
 * splash's bubble, from firmware/common/story/story_scripts.h, in order.
 * "%s" is the pseudonym the teller substitutes.
 */
export function helloLines(storyScripts) {
  const beats = /inline constexpr Beat kHelloBeats\[\] = \{([\s\S]*?)\n\};/.exec(storyScripts)?.[1];
  if (!beats) throw new Error("story_scripts.h names no kHelloBeats");
  return [...beats.matchAll(/\{\s*"((?:[^"\\]|\\.)*)",/g)].map((m) => m[1]);
}

/** Whether `text` is the whole of a script line (its "%s" any pseudonym). */
export function isWholeLine(text, line) {
  const parts = line.split("%s").map((p) => p.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"));
  return new RegExp(`^${parts.join("[^\\s]+")}$`).test(text);
}

/**
 * The splash as a run of reads held it (F206): each read {labels, bird,
 * glass} as the probes take them. Returns {reads, bird, seen: [line...],
 * missing: [line...]}: how many reads, how many showed the bird on stage,
 * and which of kHello's lines some read showed whole and which none did.
 */
export function splashCoverage(reads, lines) {
  const seen = new Set();
  let bird = 0;
  for (const st of reads) {
    if (st.bird && st.bird.shown) bird++;
    for (const l of st.labels || []) {
      if (!(l.shown && l.opa > 0)) continue;
      for (const line of lines) if (isWholeLine(l.text, line)) seen.add(line);
    }
  }
  return { reads: reads.length, bird, seen: lines.filter((l) => seen.has(l)),
    missing: lines.filter((l) => !seen.has(l)) };
}
