// canary-local/assets/board-lab.js — the board, as the real thing.
//
// The device sheet's "Board" tab: the vendor's actual CAD (Seeed's published
// STEP, tessellated to a committed GLB by tools/gen_boards.py) spinning in the
// SAME WebGL viewer the printed enclosure parts use — real soldermask, gold
// pads, shield, connectors, straight from the millimetres the vendor drew.
// Not an illustration of the board; the board.
//
// Data: devices/boards.json (facts recomputed from the committed GLB by the
// page's own loader, so the numbers can't drift from the mesh) + boards/*.glb.
// Everything offline: no CDN, no fetch outside this directory (Invariant IV).

import { DeviceScene } from "./scene3d.js";
import { parseGLB } from "./glb.js";

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};

const GH = "https://github.com/kmay89/securaCV/blob/main/";

// per-material gloss so metals shine and soldermask stays satin — the same
// separation the CAD colours imply (money-shot lighting, honest materials).
// Exported for the Board Room (board-room.js), which stages the same GLBs.
const METAL = new Set(["#8d8d8d", "#7f8e98", "#5a5a5a", "#ada186", "#a6a6a6", "#c7d3e3", "#8a8a8a"]);
const GOLD = new Set(["#e7c863", "#e59833"]);
const BRIGHT = new Set(["#f6f4e9", "#ebebe5", "#ffffff", "#fefefe", "#fdfdfd"]);
const LENS = new Set(["#801407", "#471e00", "#392d1d"]);
export const hexOf = (c) => "#" + c.map((x) => Math.round(x * 255).toString(16).padStart(2, "0")).join("");
export function glossFor(hex) {
  if (METAL.has(hex)) return 0.72;
  if (GOLD.has(hex)) return 0.55;
  if (LENS.has(hex)) return 0.85;
  if (BRIGHT.has(hex)) return 0.28;
  return 0.4;
}

function translate(x, y, z) {
  const o = new Float32Array(16);
  o[0] = o[5] = o[10] = o[15] = 1;
  o[12] = x; o[13] = y; o[14] = z;
  return o;
}

export function buildBoardLab(boardsData, deviceId) {
  const wrap = el("div", "boardlab");
  if (!boardsData) {
    wrap.append(el("p", "muted", "Board catalog unavailable."));
    return wrap;
  }
  // device_board maps a device to a LIST of boards (primary first); a Watch is
  // a plain XIAO stacked in the Round Display, so it carries two. Tolerate the
  // legacy single-string form too.
  const mapped = boardsData.device_board?.[deviceId];
  const bids = (Array.isArray(mapped) ? mapped : mapped ? [mapped] : [])
    .filter((b) => boardsData.boards?.[b]);
  if (!bids.length) {
    wrap.append(el("p", "muted",
      "No board modeled for this device yet — the vendor CAD lands here when it's in the repo."));
    return wrap;
  }

  // >1 board → a pill picker that swaps the board panel; 1 board → just the panel
  const panel = el("div", "boardlab-panel");
  if (bids.length > 1) {
    const picker = el("div", "board-picker pills");
    bids.forEach((b, i) => {
      const pill = el("button", "pill" + (i === 0 ? " on" : ""), boardsData.boards[b].name);
      pill.addEventListener("click", () => {
        for (const x of picker.children) x.classList.remove("on");
        pill.classList.add("on");
        panel.replaceChildren(renderBoardPanel(boardsData, b));
      });
      picker.append(pill);
    });
    wrap.append(picker);
  }
  panel.append(renderBoardPanel(boardsData, bids[0]));
  wrap.append(panel);
  return wrap;
}

// the board panel for one board id (ribbon → 3D → facts → pinout → links)
function renderBoardPanel(boardsData, bid) {
  const wrap = el("div", "boardlab-one");
  const board = boardsData.boards?.[bid];
  if (!board) {
    wrap.append(el("p", "muted", "board unavailable"));
    return wrap;
  }

  // honesty ribbon: what you're looking at and where it came from
  const ribbon = el("div", "board-ribbon");
  ribbon.append(
    el("strong", null, board.name),
    el("span", "muted", `  ${board.vendor}${board.mpn ? " · " + board.mpn : ""}`)
  );
  wrap.append(ribbon);

  const prov = el("p", "ondevice board-prov");
  prov.append(el("strong", null, "What this is: "), document.createTextNode(board.provenance));
  wrap.append(prov);

  // 3D stage
  const stage = el("div", "boardlab-stage");
  const cv = el("canvas", "boardlab-3d");
  const legend = el("div", "boardlab-legend");
  stage.append(cv, legend);
  wrap.append(stage);

  const scene = new DeviceScene(cv, null);
  scene.start();
  cv.__scene = scene; // test/debug handle
  // Stop the render loop when the canvas leaves the DOM (tab switch / sheet
  // close). IntersectionObserver on the canvas fires only on real visibility
  // changes — unlike a MutationObserver on document.body, which the running
  // firmware emulator (serial + MQTT logs) would trip hundreds of times/sec.
  const obs = new IntersectionObserver(() => {
    if (!document.body.contains(cv)) { scene.stop(); obs.disconnect(); }
  });
  obs.observe(cv);

  // dims + facts line
  const dims = board.dims_mm;
  const facts = el("p", "muted boardlab-facts");
  facts.textContent =
    `${dims[0]} × ${dims[1]} × ${dims[2]} mm · ${board.triangles.toLocaleString()} triangles · ${board.parts} parts, from the vendor STEP`;
  wrap.append(facts);

  // load the committed GLB and stage it
  (async () => {
    let parsed;
    try {
      const buf = await (await fetch(board.glb)).arrayBuffer();
      parsed = parseGLB(buf);
    } catch (e) {
      legend.append(el("span", "muted", "board mesh unavailable — run tools/gen_boards.py"));
      return;
    }
    if (!document.body.contains(cv)) return; // sheet closed mid-fetch
    const { parts, bbox } = parsed;
    const [cx, cy, cz] = bbox.center;
    const model = translate(-cx, -cy, -cz);
    for (const p of parts) {
      scene.addMesh({ pos: p.pos, nrm: p.nrm, uv: p.uv, idx: p.idx },
        { color: p.color, gloss: glossFor(hexOf(p.color)), model });
    }
    const maxDim = Math.max(...bbox.size);
    const pose = board.pose || { rx: -0.5, ry: 0.7, dist_factor: 2.7 };
    scene.dist = maxDim * (pose.dist_factor || 2.7);
    scene.rot = { x: pose.rx, y: pose.ry };
    scene.home = { x: pose.rx, y: pose.ry };
    legend.append(el("span", "muted", "drag to orbit · scroll to zoom"));
  })();

  // blurb
  wrap.append(el("p", "body", board.blurb));

  // the pins this board exposes to the firmware — honestly tagged: a "planned"
  // pin is defined in config but not yet driven/read by the current build.
  if (board.pinout?.length) {
    wrap.append(el("h4", null, "Firmware pin map"));
    const tbl = el("div", "pin-table");
    let anyPlanned = false;
    for (const p of board.pinout) {
      const planned = p.status === "planned";
      anyPlanned = anyPlanned || planned;
      const row = el("div", "pin-row" + (planned ? " pin-planned" : ""));
      const label = el("span", "pin-label", p.label);
      if (planned) label.append(el("span", "pin-tag", "planned"));
      row.append(
        label,
        el("code", "pin-pin", p.pin),
        el("code", "pin-gpio", p.gpio || "—"),
        el("span", "pin-use muted", p.use)
      );
      tbl.append(row);
    }
    wrap.append(tbl);
    if (anyPlanned) {
      wrap.append(el("p", "muted fineprint",
        "“planned” — the pin is defined in firmware config but not yet driven or read by this build; wiring it won't do anything until the firmware lands."));
    }
  }

  // sources / downloads
  const links = el("p", "muted fineprint boardlab-links");
  const dl = el("a", null, "download .glb");
  dl.href = board.glb;
  dl.download = bid + ".glb";
  links.append(dl, document.createTextNode(" · "));
  const step = el("a", null, "vendor STEP");
  step.href = GH + board.source_step;
  step.target = "_blank"; step.rel = "noopener";
  links.append(document.createTextNode("source: "), step);
  if (board.doc) {
    const doc = el("a", null, "vendor docs ↗");
    doc.href = board.doc; doc.target = "_blank"; doc.rel = "noopener";
    links.append(document.createTextNode(" · "), doc);
  }
  const room = el("a", null, "pin flags + wiring → the Board Room");
  room.href = "boards.html";
  links.append(document.createTextNode(" · "), room);
  wrap.append(links);

  return wrap;
}
