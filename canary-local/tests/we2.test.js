// canary-local/tests/we2.test.js — the module flasher's honesty gate.
//
// The WE2 (Grove Vision AI V2 / Himax HX6538) flasher must speak the exact
// wire protocol Seeed's own open-source flasher speaks; these tests pin the
// DOM-free core (assets/we2-core.js) byte for byte:
//   · CRC-16/XMODEM against the classic known-answer vector
//   · packet framing (SOH · blk · ~blk · data · CRC hi/lo)
//   · the burn-address preamble block (C0 5A <addr LE> <off LE> 5A C0 · FF…)
//   · 0x1A padding + block wrap-around at 255
//   · the full happy path against a scripted fake bootloader: reset → '1'
//     drip → menu → option 1 → 'C' → preamble session → 'n' → 'C' → payload
//     → EOT → prompt → 'y' — asserting the fake's flash contains the exact
//     payload at 0x400000
//   · NAK-resend, CAN-abort and wrong-port failure modes
//   · the AT \r{json}\n incremental frame parser (split frames, noise)

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const core = () => import("../assets/we2-core.js");

// ── CRC + framing ──────────────────────────────────────────────────────────
test("crc16xmodem matches the known-answer vector", async () => {
  const { crc16xmodem } = await core();
  const ascii = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0));
  // CRC-16/XMODEM("123456789") = 0x31C3 — the catalog check value
  assert.strictEqual(crc16xmodem(ascii("123456789")), 0x31c3);
  assert.strictEqual(crc16xmodem(new Uint8Array(0)), 0x0000);
  // init 0 means all-zero data stays 0 — and one bit flips it (error detection)
  assert.strictEqual(crc16xmodem(new Uint8Array(128)), 0x0000);
  const flipped = new Uint8Array(128); flipped[64] = 1;
  assert.notStrictEqual(crc16xmodem(flipped), 0x0000);
});

test("xmodemPacket frames SOH · blk · ~blk · 128 data · CRC hi/lo", async () => {
  const { xmodemPacket, crc16xmodem, WE2 } = await core();
  const data = new Uint8Array(128).map((_, i) => i & 0xff);
  const p = xmodemPacket(1, data);
  assert.strictEqual(p.length, 133);
  assert.strictEqual(p[0], WE2.SOH);
  assert.strictEqual(p[1], 1);
  assert.strictEqual(p[2], 0xfe);
  assert.deepStrictEqual([...p.subarray(3, 131)], [...data]);
  const crc = crc16xmodem(data);
  assert.strictEqual(p[131], crc >> 8);
  assert.strictEqual(p[132], crc & 0xff);
  // block numbers ride a Uint8Array: 256 wraps to 0, complement stays honest
  const p256 = xmodemPacket(256, data);
  assert.strictEqual(p256[1], 0);
  assert.strictEqual(p256[2], 0xff);
});

test("preambleBlock is C0 5A <addr LE> <off LE> 5A C0, FF-padded", async () => {
  const { preambleBlock } = await core();
  const b = preambleBlock(0x400000);
  assert.strictEqual(b.length, 128);
  assert.deepStrictEqual([...b.subarray(0, 12)],
    [0xc0, 0x5a, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0xc0]);
  assert.ok([...b.subarray(12)].every((x) => x === 0xff));
  // a second slot Seeed's eraser touches
  assert.deepStrictEqual([...preambleBlock(0x300000).subarray(2, 6)], [0x00, 0x00, 0x30, 0x00]);
});

test("chunkBlocks pads the tail with 0x1A and never yields zero blocks", async () => {
  const { chunkBlocks, WE2 } = await core();
  const blocks = chunkBlocks(new Uint8Array(300).fill(7));
  assert.strictEqual(blocks.length, 3);
  assert.ok(blocks.every((b) => b.length === 128));
  assert.strictEqual(blocks[2][43], 7);                       // last payload byte (300 - 256 - 1)
  assert.ok([...blocks[2].subarray(44)].every((x) => x === WE2.FILLER));
  assert.strictEqual(chunkBlocks(new Uint8Array(0)).length, 1);
});

// ── the scripted fake bootloader ──────────────────────────────────────────
// Implements the transport contract and behaves like the WE2 ROM/бootloader
// as both reference implementations observe it. Records everything burned.
function makeFakeBootloader(opts = {}) {
  const state = {
    stage: "app",           // app → menu → xmodem → prompt
    rx: [],                 // bytes the host may read
    flash: new Map(),       // addr -> Uint8Array (assembled burns)
    burnAddr: 0,            // 0 = firmware region default
    pkts: [],               // raw packets seen (for framing asserts)
    resets: 0,
    log: [],
    nakFirstDataBlock: !!opts.nakFirstDataBlock,
    cancelOnBlock: opts.cancelOnBlock || 0,
    neverMenu: !!opts.neverMenu,
  };
  const pushStr = (s) => { for (const ch of s) state.rx.push(ch.charCodeAt(0)); };

  let pending = [];           // current packet accumulation
  let expect = 0;             // bytes still expected for the packet
  let dataBlocksSeen = 0;
  let assembled = [];

  const finishTransfer = () => {
    const bytes = Uint8Array.from(assembled);
    // a 128-byte C0 5A … 5A C0 block is the aim preamble, not a payload
    if (bytes.length === 128 && bytes[0] === 0xc0 && bytes[1] === 0x5a &&
        bytes[10] === 0x5a && bytes[11] === 0xc0) {
      state.burnAddr = bytes[2] | (bytes[3] << 8) | (bytes[4] << 16) | (bytes[5] << 24);
      state.log.push("aimed@0x" + state.burnAddr.toString(16));
    } else {
      state.flash.set(state.burnAddr, bytes);
      state.log.push("burned " + bytes.length + "B @0x" + state.burnAddr.toString(16));
    }
    assembled = [];
    dataBlocksSeen = 0;
    state.stage = "prompt";
    pushStr("\r\nDo you want to end file transmission and reboot system? (y)\r\n");
  };

  const onByte = (b) => {
    if (state.stage === "menu-wait") {
      // like the silicon: the banner prints in response to the boot being
      // interrupted — the first '1' surfaces the menu, the next selects
      if (b === 0x31) {
        state.stage = "menu-shown";
        pushStr("\r\nWiseEye2 boot\r\n1. Xmodem download and burn FW image\r\n2. …\r\n");
      }
      return;
    }
    if (state.stage === "menu-shown") {
      if (b === 0x31) state.stage = "xmodem"; // option 1 → receiver ('C' via readByte)
      return;
    }
    if (state.stage === "xmodem") {
      if (expect === 0) {
        if (b === 0x01) { pending = [b]; expect = 132; return; } // SOH + 132 more
        if (b === 0x04) { // EOT
          state.rx.push(0x06);
          finishTransfer();
          return;
        }
        return; // stray byte mid-handshake
      }
      pending.push(b); expect -= 1;
      if (expect === 0) {
        const pkt = Uint8Array.from(pending);
        state.pkts.push(pkt);
        dataBlocksSeen += 1;
        if (state.cancelOnBlock && dataBlocksSeen === state.cancelOnBlock) {
          state.rx.push(0x18); return; // CAN
        }
        if (state.nakFirstDataBlock && dataBlocksSeen === 1) {
          state.nakFirstDataBlock = false;
          state.rx.push(0x15); return; // NAK once → host must resend
        }
        // verify CRC like the ROM does
        const data = pkt.subarray(3, 131);
        let crc = 0;
        for (const x of data) { crc ^= x << 8; for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff; }
        if (((pkt[131] << 8) | pkt[132]) !== crc || pkt[1] !== (0xff - pkt[2])) { state.rx.push(0x15); return; }
        assembled.push(...data);
        state.rx.push(0x06); // ACK
      }
      return;
    }
    if (state.stage === "prompt") {
      if (b === 0x6e) { // 'n' → back to receiving
        state.stage = "xmodem";
        state.rx.push(0x43);
      } else if (b === 0x79) { // 'y' → reboot to app
        state.stage = "app";
        state.log.push("rebooted");
      }
      return;
    }
  };

  const transport = {
    async setRTS(v) {
      if (v === true) { // rising edge = reset release; boot can be interrupted
        state.resets += 1;
        if (!state.neverMenu) state.stage = "menu-wait";
      }
    },
    async sleep() {},
    clear() { state.rx = []; },
    async write(bytes) { for (const b of bytes) onByte(b); },
    writeString(s) { for (const ch of s) onByte(ch.charCodeAt(0)); },
    async readByte() {
      // fake time: rx is always "already arrived"; null models a timeout.
      // A quiet receiver streams 'C' invitations, like the real bootloader.
      if (state.rx.length) return state.rx.shift();
      if (state.stage === "xmodem" && expect === 0) return 0x43;
      return null;
    },
    async readUntil(needle, _timeout, tick) {
      let s = "";
      for (let i = 0; i < 50; i++) {
        if (tick) tick();
        while (state.rx.length) s += String.fromCharCode(state.rx.shift());
        if (s.includes(needle)) return s;
      }
      return s;
    },
  };
  return { transport, state };
}

test("happy path: aim at 0x400000, burn the exact payload, reboot", async () => {
  const { We2Flasher, WE2 } = await core();
  const { transport, state } = makeFakeBootloader();
  const payload = new Uint8Array(1000).map((_, i) => (i * 7) & 0xff);
  const logs = [];
  const progress = [];
  const f = new We2Flasher(transport, { onLog: (l) => logs.push(l), onProgress: (p) => progress.push(p) });
  await f.flashModel(payload);

  assert.ok(state.log.includes("aimed@0x400000"), "preamble must aim the burn: " + state.log);
  const burned = state.flash.get(WE2.MODEL_ADDR);
  assert.ok(burned, "nothing burned at the model slot");
  // burned image = payload + 0x1A padding to the block boundary
  assert.strictEqual(burned.length, Math.ceil(1000 / 128) * 128);
  assert.deepStrictEqual([...burned.subarray(0, 1000)], [...payload]);
  assert.ok([...burned.subarray(1000)].every((x) => x === WE2.FILLER));
  assert.ok(state.log.includes("rebooted"), "must answer 'y' and reboot");
  assert.ok(state.resets >= 2, "reset to enter + reset after finish");
  assert.strictEqual(progress[progress.length - 1], 1);
});

test("a NAK forces a resend of the same block, then the burn still matches", async () => {
  const { We2Flasher, WE2 } = await core();
  const { transport, state } = makeFakeBootloader({ nakFirstDataBlock: true });
  const payload = new Uint8Array(256).fill(0xab);
  const f = new We2Flasher(transport, {});
  await f.flashModel(payload);
  const burned = state.flash.get(WE2.MODEL_ADDR);
  assert.deepStrictEqual([...burned.subarray(0, 256)], [...payload]);
  // preamble pkt + NAKed block + its resend + second block = 4 packets min
  assert.ok(state.pkts.length >= 4, "expected a resend, saw " + state.pkts.length + " packets");
});

test("CAN aborts the transfer with a clear error", async () => {
  const { We2Flasher } = await core();
  const { transport } = makeFakeBootloader({ cancelOnBlock: 2 });
  const f = new We2Flasher(transport, {});
  await assert.rejects(() => f.flashModel(new Uint8Array(300)), /canceled/);
});

test("no bootloader menu (wrong port) fails with the teaching error", async () => {
  const { We2Flasher } = await core();
  const { transport } = makeFakeBootloader({ neverMenu: true });
  const f = new We2Flasher(transport, {});
  await assert.rejects(() => f.flashModel(new Uint8Array(10)), /module's USB-C port/);
});

// ── AT frame parser ────────────────────────────────────────────────────────
test("makeAtParser yields frames across split reads and ignores noise", async () => {
  const { makeAtParser, atCommand } = await core();
  const p = makeAtParser();
  const got = [];
  const feedStr = (s) => p.feed(Uint8Array.from(s, (c) => c.charCodeAt(0)), (f) => got.push(f));
  feedStr("menu noise\r\n");
  feedStr('\r{"type":0,"name":"VER?","code":0,"data"');   // torn across reads
  feedStr(':{"software":"2024.01"}}\n');
  feedStr('junk\r{"type":1,"name":"INVOKE","code":0,"data":{"boxes":[[10,20,30,40,95,0]]}}\nmore');
  assert.strictEqual(got.length, 2);
  assert.strictEqual(got[0].data.software, "2024.01");
  assert.deepStrictEqual(got[1].data.boxes[0], [10, 20, 30, 40, 95, 0]);
  assert.strictEqual(atCommand('TSCORE=70'), "AT+TSCORE=70\r");
});

test("modelInfoJson names exactly what we flash (person, one class)", async () => {
  const { modelInfoJson } = await core();
  const info = modelInfoJson({ version: "1.0.0", sha256: "ab".repeat(32) });
  assert.deepStrictEqual(info.classes, ["person"]);
  assert.match(info.name, /Person Detection/);
  assert.strictEqual(info.sha256.length, 64);
});

// ── anti-rot: the catalog, the engine, and the release pipeline agree ───────
// The burn address, USB ids and baud live in exactly one place — we2-core.js.
// gen_flash.py copies them into flash.json (drift-gated in canary-local.yml)
// and the release workflow reads flash.json to stamp the manifest. This test
// slams the door on a hand-edit of flash.json that diverges from the engine:
// if these disagree, the flasher would burn to an address the manifest never
// promised. It also guards the address the workflow greps for.
test("flash.json we2_module mirrors the engine's own constants", async () => {
  const { WE2 } = await core();
  const flash = JSON.parse(readFileSync(join(__dirname, "..", "devices", "flash.json"), "utf8"));
  const m = flash.we2_module;
  assert.ok(m, "flash.json lost its we2_module block");
  const asHex = (n) => "0x" + n.toString(16);
  assert.strictEqual(m.model_addr, asHex(WE2.MODEL_ADDR), "catalog burn address ≠ engine MODEL_ADDR");
  assert.strictEqual(m.baud, WE2.BAUD, "catalog baud ≠ engine BAUD");
  assert.strictEqual(m.usb_vid, asHex(WE2.USB_VID), "catalog USB vid ≠ engine USB_VID");
  assert.strictEqual(m.usb_pid, asHex(WE2.USB_PID), "catalog USB pid ≠ engine USB_PID");
  // the erase pass must clear the very slot we then burn (Seeed's flasher does)
  assert.ok(WE2.ERASE_SLOTS.includes(WE2.MODEL_ADDR), "MODEL_ADDR not among ERASE_SLOTS");
  // the flasher fetches the pinned model from the firmware release TAG
  // (fw-v<train>), never /latest/ — a native-app release would shadow it
  assert.match(m.manifest_url, /\/releases\/download\/fw-v[\d.]+\/manifest-vision-model\.json$/);
});

// The workflow that stamps the manifest must read the burn address FROM
// flash.json (single source), not hardcode a second copy. Grep the workflow
// so a future edit that reintroduces a literal 0x… address is caught here.
test("the model-release workflow reads the burn address from flash.json", () => {
  const wf = readFileSync(
    join(__dirname, "..", "..", ".github", "workflows", "vision-model-release.yml"), "utf8");
  assert.match(wf, /flash\.json/, "workflow must derive the burn address from flash.json");
  assert.match(wf, /we2_module/, "workflow must read the we2_module block");
  // no stray hardcoded burn address (0x400000) outside a comment line
  for (const line of wf.split("\n")) {
    const code = line.split("#")[0];
    assert.ok(!/0x400000/.test(code),
      "workflow hardcodes 0x400000 — read it from flash.json instead: " + line.trim());
  }
});
