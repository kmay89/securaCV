// canary-local/assets/we2-core.js — the module flasher's DOM-free core.
//
// The Grove Vision AI V2's Himax HX6538 ("WiseEye2", WE2) flashes over a
// plain-serial bootloader: reset the chip, interrupt auto-boot, pick menu
// option 1 ("Xmodem download and burn FW image"), then send 128-byte
// CRC-16/XMODEM blocks. A non-zero burn address is aimed with a one-block
// preamble transfer (magic C0 5A + little-endian address + 5A C0). This is
// the same wire protocol Seeed's own open-source web flasher
// (SenseCraft-Web-Toolkit, MIT) and Himax's xmodem_send.py implement — this
// file is a clean-room JS mirror of that observed protocol, written so every
// piece is pure and testable under `node --test` (tests/we2.test.js, the
// repo convention): CRC, packet framing, the preamble block, the chunker,
// the bootloader state machine (injectable transport + no real timers in
// tests), and the AT `\r{json}\n` frame parser used for post-flash
// verification. No DOM, no Web Serial import — we2-flash.js owns that.
//
// Protocol constants (all confirmed against the reference implementations):
//   serial: CH343 (USB vid 0x1a86 pid 0x55d3) · 921600 baud · 8N1
//   reset:  RTS low, 100 ms, RTS high
//   enter:  spam '1' every 10 ms until "Xmodem download and burn FW image",
//           then one more '1' selects option 1
//   xmodem: SOH · 128-byte blocks · CRC-16/XMODEM (poly 0x1021, init 0) ·
//           0x1A padding · 'C' starts CRC mode · ACK/NAK/CAN · single EOT
//   aim:    preamble block C0 5A <addr LE u32> <offset LE u32> 5A C0, 0xFF-
//           padded — its own complete XMODEM session, answered 'n' to stay
//   done:   "Do you want to end file transmission and reboot system" → 'y'
//   model:  the SSCMA custom-model slot is 0x400000 (SenseCraft's own)

// ── constants ─────────────────────────────────────────────────────────────
export const WE2 = {
  USB_VID: 0x1a86,
  USB_PID: 0x55d3,
  BAUD: 921600,
  SOH: 0x01,
  EOT: 0x04,
  ACK: 0x06,
  NAK: 0x15,
  CAN: 0x18,
  CRC_START: 0x43, // 'C'
  FILLER: 0x1a,
  BLOCK: 128,
  MODEL_ADDR: 0x400000,
  ERASE_SLOTS: [0x300000, 0x400000],
  MENU_LINE: "Xmodem download and burn FW image",
  PROMPT_LINE: "Do you want to end file transmission and reboot system",
  SPAM_INTERVAL_MS: 10,
  MENU_WINDOW_MS: 2500,   // toolkit uses 100 ms; we retry the whole dance instead of failing that fast
  SETTLE_MS: 100,
  BYTE_TIMEOUT_MS: 60000,
  PROMPT_TIMEOUT_MS: 10000,
  MAX_TIMEOUTS: 10,
  MAX_ERRORS: 30,
};

// ── CRC-16/XMODEM: poly 0x1021, init 0x0000, no reflect, no xorout ───────
export function crc16xmodem(bytes) {
  let crc = 0;
  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i] << 8;
    for (let b = 0; b < 8; b++) {
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

// ── one wire packet: SOH · blk · ~blk · 128 data · CRC hi · CRC lo ───────
export function xmodemPacket(blockNum, data128) {
  if (data128.length !== WE2.BLOCK) throw new Error("packet data must be 128 bytes");
  const p = new Uint8Array(3 + WE2.BLOCK + 2);
  p[0] = WE2.SOH;
  p[1] = blockNum & 0xff;
  p[2] = 0xff - (blockNum & 0xff);
  p.set(data128, 3);
  const crc = crc16xmodem(data128);
  p[3 + WE2.BLOCK] = crc >> 8;
  p[4 + WE2.BLOCK] = crc & 0xff;
  return p;
}

// ── split a payload into 0x1A-padded 128-byte blocks ─────────────────────
export function chunkBlocks(bytes) {
  const out = [];
  for (let o = 0; o < bytes.length; o += WE2.BLOCK) {
    const b = new Uint8Array(WE2.BLOCK);
    b.fill(WE2.FILLER);
    b.set(bytes.subarray(o, Math.min(o + WE2.BLOCK, bytes.length)));
    out.push(b);
  }
  if (out.length === 0) { const b = new Uint8Array(WE2.BLOCK); b.fill(WE2.FILLER); out.push(b); }
  return out;
}

// ── the burn-address preamble block (its own one-block XMODEM session) ───
export function preambleBlock(addr, offset = 0) {
  const b = new Uint8Array(WE2.BLOCK);
  b.fill(0xff);
  b[0] = 0xc0; b[1] = 0x5a;
  b[2] = addr & 0xff;
  b[3] = (addr >> 8) & 0xff;
  b[4] = (addr >> 16) & 0xff;
  b[5] = (addr >> 24) & 0xff;
  b[6] = offset & 0xff;
  b[7] = (offset >> 8) & 0xff;
  b[8] = (offset >> 16) & 0xff;
  b[9] = (offset >> 24) & 0xff;
  b[10] = 0x5a; b[11] = 0xc0;
  return b;
}

// ── AT wire helpers (SSCMA framing: \r{json}\n) ──────────────────────────
export function atCommand(body) {
  return "AT+" + body + "\r";
}

// Incremental parser for the SSCMA reply stream. Feed it raw bytes; it
// yields every complete `\r{ ... }\n` JSON frame (events AND replies) and
// tolerates menu noise / partial frames across reads.
export function makeAtParser() {
  let buf = [];
  let inFrame = false;
  return {
    feed(bytes, onFrame) {
      for (const c of bytes) {
        if (!inFrame) {
          if (c === 0x0d) { buf = [0x0d]; inFrame = "cr"; }
          continue;
        }
        if (inFrame === "cr") {
          if (c === 0x7b) { buf.push(c); inFrame = true; }
          else if (c === 0x0d) { buf = [0x0d]; }
          else { inFrame = false; buf = []; }
          continue;
        }
        buf.push(c);
        if (c === 0x0a && buf[buf.length - 2] === 0x7d) {
          const text = String.fromCharCode(...buf.slice(1, -1));
          inFrame = false; buf = [];
          try { onFrame(JSON.parse(text)); } catch { /* torn frame — drop */ }
        }
        if (buf.length > 1 << 20) { inFrame = false; buf = []; } // runaway guard
      }
    },
  };
}

// ── the flasher state machine ────────────────────────────────────────────
// transport contract (all async unless noted):
//   readByte(timeoutMs) -> int | null        one byte or null on timeout
//   readUntil(needle, timeoutMs) -> string   accumulated text (may lack needle)
//   write(uint8array) / writeString(str)
//   clear()                                   drop pending RX (sync ok)
//   setRTS(bool)
//   sleep(ms)
// Everything observable is reported through onLog(line) / onProgress(0..1).
export class We2Flasher {
  constructor(transport, { onLog = () => {}, onProgress = () => {} } = {}) {
    this.t = transport;
    this.onLog = onLog;
    this.onProgress = onProgress;
  }

  async hardReset() {
    await this.t.setRTS(false);
    await this.t.sleep(100);
    await this.t.setRTS(true);
  }

  // Reset, interrupt auto-boot with a '1' drip, select menu option 1.
  async enterBootloader() {
    this.onLog("resetting the module (RTS pulse)…");
    await this.hardReset();
    this.t.clear();
    this.onLog("catching the bootloader menu…");
    const found = await this.t.readUntil(WE2.MENU_LINE, WE2.MENU_WINDOW_MS, () => {
      this.t.writeString("1"); // dripped by the transport every SPAM_INTERVAL_MS
    });
    if (!found.includes(WE2.MENU_LINE)) {
      throw new Error("no bootloader menu — is this the module's USB-C port (not the XIAO's)?");
    }
    this.t.writeString("1"); // select: Xmodem download and burn FW image
    await this.t.sleep(WE2.SETTLE_MS);
    this.t.clear();
    this.onLog("bootloader ready — option 1 selected");
  }

  // Wait for the receiver's 'C' (CRC-mode invitation).
  async waitStart() {
    for (let tries = 0; tries < WE2.MAX_TIMEOUTS; tries++) {
      const c = await this.t.readByte(WE2.BYTE_TIMEOUT_MS);
      if (c === WE2.CRC_START) return;
      if (c === WE2.CAN) throw new Error("bootloader canceled the transfer");
      // stray menu echo / extra bytes: keep reading
    }
    throw new Error("bootloader never invited the transfer ('C')");
  }

  // One complete XMODEM session for `bytes` (already framed by caller).
  async xmodemSend(bytes, progressBase = 0, progressSpan = 1) {
    await this.waitStart();
    const blocks = chunkBlocks(bytes);
    let blockNum = 1;
    let errors = 0;
    for (let i = 0; i < blocks.length; ) {
      const pkt = xmodemPacket(blockNum, blocks[i]);
      await this.t.write(pkt);
      const r = await this.t.readByte(WE2.BYTE_TIMEOUT_MS);
      if (r === WE2.ACK) {
        i += 1; blockNum = (blockNum + 1) & 0xff; errors = 0;
        this.onProgress(progressBase + (progressSpan * i) / blocks.length);
        continue;
      }
      if (r === WE2.CAN) throw new Error("transfer canceled by the bootloader");
      errors += 1; // NAK, stray byte, or timeout → resend this block
      if (errors > WE2.MAX_ERRORS) throw new Error("too many transfer errors — check the cable and retry");
    }
    // single EOT, expect ACK (resend on NAK)
    for (let errorsEot = 0; ; errorsEot++) {
      await this.t.write(new Uint8Array([WE2.EOT]));
      const r = await this.t.readByte(WE2.BYTE_TIMEOUT_MS);
      if (r === WE2.ACK) break;
      if (errorsEot > WE2.MAX_TIMEOUTS) throw new Error("bootloader never acknowledged end of transfer");
    }
  }

  // The bootloader's post-transfer question; 'n' stays in for another
  // transfer, 'y' reboots into the burned image.
  async answerPrompt(answer) {
    const s = await this.t.readUntil(WE2.PROMPT_LINE, WE2.PROMPT_TIMEOUT_MS);
    if (!s.includes(WE2.PROMPT_LINE)) throw new Error("bootloader never asked to finish — retry the flash");
    this.t.writeString(answer);
  }

  // Burn `bytes` at flash address `addr`. addr 0 = the firmware region
  // (no preamble); anything else is aimed with the preamble session first.
  async flashAt(bytes, addr, { finish = true, progressBase = 0, progressSpan = 1 } = {}) {
    await this.enterBootloader();
    if (addr !== 0) {
      this.onLog("aiming the burn at 0x" + addr.toString(16) + "…");
      this.t.clear();
      await this.xmodemSend(preambleBlock(addr), progressBase, 0);
      await this.answerPrompt("n"); // stay in the bootloader for the payload
      this.t.clear();
    }
    this.onLog("sending " + bytes.length + " bytes over XMODEM…");
    await this.xmodemSend(bytes, progressBase, progressSpan);
    // The bar sits at 100 % while the bootloader digests the transfer and
    // offers its prompt (up to ~10 s) — name the pause, or it reads as a hang.
    this.onLog("all blocks acknowledged — waiting for the bootloader's prompt (up to ~10 s)…");
    await this.answerPrompt(finish ? "y" : "n");
    if (finish) {
      this.onLog("done — rebooting the module");
      await this.t.sleep(2000);
      await this.hardReset();
      await this.t.sleep(500);
    }
  }

  // The one call the UI makes: burn a model into the SSCMA custom slot.
  async flashModel(bytes) {
    await this.flashAt(bytes, WE2.MODEL_ADDR);
  }
}

// ── model metadata (what SenseCraft stores with AT+INFO after a flash) ───
// The SSCMA firmware keeps a base64 JSON blob describing the loaded model;
// dashboards read it back with AT+INFO?. Ours says exactly what we flashed.
export function modelInfoJson({ version, sha256 }) {
  return {
    name: "Person Detection",
    version: version || "",
    category: "Object Detection",
    algorithm: "Swift-YOLO (tiny)",
    description: "SecuraCV-pinned person model (SSCMA model zoo, MIT)",
    classes: ["person"],
    model_type: "TFLite (Vela, Ethos-U55)",
    sha256: sha256 || "",
    source: "securacv.com/lab — vendored from Seeed SSCMA model zoo",
  };
}

// ── live-preview detection formatting (the bench "wow") ─────────────────────
// The pinned Vision model detects one class; single source for the label map so
// the bench preview reads "person · 92%", not "class 0 · 85".
export const WE2_CLASSES = ["person"];

// Normalize an SSCMA detection box — [x, y, w, h, score, target] — into a
// labeled object the preview draws. `score` is already 0-100 (a percent).
// Unknown targets get a generic label instead of breaking; garbage is dropped.
// Pure + host-tested (tests/we2.test.js).
export function formatDetections(boxes, classes = WE2_CLASSES) {
  const out = [];
  for (const b of boxes || []) {
    if (!Array.isArray(b) || b.length < 6) continue;
    const [x, y, w, h, score, target] = b;
    if (![x, y, w, h].every((n) => typeof n === "number" && Number.isFinite(n))) continue;
    const label = (classes && classes[target]) || "object";
    const pct = Math.max(0, Math.min(100, Math.round(Number(score) || 0)));
    out.push({ x, y, w, h, score: pct, label, text: `${label} · ${pct}%` });
  }
  return out;
}

// Per-detection paint for the bench canvas: a stable identity color per box
// index (so two people stay visually distinct frame to frame) and a confidence
// band (ok ≥ 60, soft 35-59, faint < 35) that picks stroke weight/glow. Pure —
// the canvas code just applies it. Host-tested (tests/we2.test.js).
const PREVIEW_HUES = [140, 45, 200, 320, 20, 260, 80, 175]; // green first — the pinned person model leads with it
export function stylizeDetections(boxes, classes = WE2_CLASSES) {
  return formatDetections(boxes, classes).map((d, i) => {
    const hue = PREVIEW_HUES[i % PREVIEW_HUES.length];
    const band = d.score >= 60 ? "ok" : d.score >= 35 ? "soft" : "faint";
    return {
      ...d,
      n: i + 1,
      band,
      color: `hsl(${hue} 90% ${band === "ok" ? 66 : 58}%)`,
      fill: `hsl(${hue} 65% 14% / ${band === "ok" ? 0.92 : 0.85})`,
    };
  });
}

// The confidence-meter model: what the meter under the preview shows. `top` is
// the best score in frame, `threshold` the module's own reporting floor
// (AT+TSCORE) rendered as a tick — a box below it never even arrives, which is
// exactly what the meter teaches. Pure + host-tested.
export function meterModel(boxes, classes = WE2_CLASSES, threshold = 50) {
  const dets = formatDetections(boxes, classes);
  const top = dets.reduce((m, d) => Math.max(m, d.score), 0);
  const thr = Math.max(0, Math.min(100, Math.round(Number(threshold) || 0)));
  return {
    count: dets.length,
    top,
    threshold: thr,
    // margin: how far the best hit clears the floor (negative never happens on
    // the wire — the module already filtered — but the math stays honest).
    margin: dets.length ? top - thr : null,
    level: !dets.length ? "idle" : top >= 60 ? "ok" : top >= 35 ? "soft" : "faint",
    label: detectionSummary(boxes, classes),
  };
}

// A one-line readout of what it sees right now — pluralized, with the top
// confidence — for the "it sees you!" moment. "watching…" when nothing's found.
export function detectionSummary(boxes, classes = WE2_CLASSES) {
  const dets = formatDetections(boxes, classes);
  if (!dets.length) return "watching… nothing yet";
  const counts = {};
  let top = 0;
  for (const d of dets) { counts[d.label] = (counts[d.label] || 0) + 1; if (d.score > top) top = d.score; }
  const pluralize = (word, n) =>
    n === 1 ? word : word === "person" ? "people" : word.endsWith("s") ? word : word + "s";
  const parts = Object.entries(counts).map(([label, n]) => `${n} ${pluralize(label, n)}`);
  return `${parts.join(", ")} · ${top}% confident`;
}
