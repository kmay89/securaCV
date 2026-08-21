// canary-local/tests/intake.test.mjs — customs, for a board we've never met.
//
// The intake check exists for states nobody has on the desk: a board whose
// secure-boot eFuse was burned by a previous owner, a 4 MB flash relabeled as
// 16 MB, a clone with a hand-rolled MAC. So the decoding is pure and the
// evidence is synthetic, and this file is the only place those cases are ever
// exercised.
//
// The eFuse bit offsets are the load-bearing part: they come from ESP-IDF's
// components/efuse/<chip>/esp_efuse_table.csv, and a wrong one would either
// cry wolf on an honest board or quietly miss a locked one. The tests build
// block-0 words bit by bit from the CSV offsets, so a transcription slip in
// intake.js fails here rather than on a user's desk.
//
// Run: node --test canary-local/tests/intake.test.mjs
import { test } from "node:test";
import assert from "node:assert/strict";

const mod = await import("../assets/intake.js");
const {
  EFUSE_BLOCK0_RD_OFFSET, EFUSE_BLOCK0_WORDS, efuseBlock0Addrs, efuseField,
  securityFieldsFor, readSecurityEfuses, looksUniform, flashAliasVerdict,
  macChecks, parseMac, duplicateMacCheck, shippedWith, intakeVerdict, efuseFindings,
  coldBootVerdict, isFirstContact, KNOWN_STOCK, oddParity, flashAliasCandidates,
} = mod;

// A virgin block 0: six zero words. Everything below starts here and burns in
// exactly one field, which is what a previous owner's lockdown looks like.
const virgin = () => new Array(EFUSE_BLOCK0_WORDS).fill(0);

// Set `width` bits of `value` at block-0 bit offset `bit`.
function burn(words, bit, value, width = 1) {
  const out = words.slice();
  for (let i = 0; i < width; i++) {
    if ((value >>> i) & 1) {
      const abs = bit + i;
      out[Math.floor(abs / 32)] |= 1 << (abs % 32);
    }
  }
  return out;
}

// ── the address window ──────────────────────────────────────────────────────

test("block 0 sits at EFUSE_BASE + 0x02C and runs six words", () => {
  // esptool espefuse/efuse/esp32s3/mem_definition.py: BLOCK0 at
  // __base_rd_regs + 0x02C, length 6.
  assert.equal(EFUSE_BLOCK0_RD_OFFSET, 0x02c);
  assert.equal(EFUSE_BLOCK0_WORDS, 6);

  const S3_EFUSE_BASE = 0x60007000;      // esptool-js ESP32S3ROM.EFUSE_BASE
  const addrs = efuseBlock0Addrs(S3_EFUSE_BASE);
  assert.equal(addrs.length, 6);
  assert.equal(addrs[0], 0x6000702c);
  assert.equal(addrs[5], 0x6000702c + 20);
  // Word 1 is the first repeat-data register, which is where RD_DIS (block-0
  // bit 32) lives — the anchor that proves the window isn't off by one.
  assert.equal(addrs[1], 0x60007030);
});

test("efuseField reads a field out of the packed words", () => {
  const w = burn(virgin(), 32, 0b101, 3);   // bit 32 = word 1, bit 0
  assert.equal(efuseField(w, 32, 3), 0b101);
  assert.equal(efuseField(w, 33, 1), 0);
  assert.equal(efuseField(w, 34, 1), 1);
  // A field that straddles a word boundary still reads whole.
  const straddle = burn(virgin(), 30, 0b1111, 4);
  assert.equal(efuseField(straddle, 30, 4), 0b1111);
  // Missing words read as null rather than as zero — "not read" must never
  // look like "read and clean".
  assert.equal(efuseField([0, 0], 116, 1), null);
});

// ── the field table, checked against esp_efuse_table.csv ────────────────────

test("security eFuse offsets match ESP-IDF's tables", () => {
  // Verbatim from components/efuse/<chip>/esp_efuse_table.csv.
  const CSV = {
    "ESP32-S3": {
      SECURE_BOOT_EN: [116, 1], SPI_BOOT_CRYPT_CNT: [82, 3],
      DIS_DOWNLOAD_MODE: [128, 1], ENABLE_SECURITY_DOWNLOAD: [133, 1],
      SECURE_BOOT_AGGRESSIVE_REVOKE: [117, 1], DIS_DOWNLOAD_MANUAL_ENCRYPT: [52, 1],
      DIS_PAD_JTAG: [51, 1], SOFT_DIS_JTAG: [48, 3], SECURE_VERSION: [142, 16],
      DIS_USB_JTAG: [118, 1], DIS_USB_SERIAL_JTAG: [119, 1],
    },
    "ESP32-C3": {
      SECURE_BOOT_EN: [116, 1], SPI_BOOT_CRYPT_CNT: [82, 3],
      DIS_DOWNLOAD_MODE: [128, 1], ENABLE_SECURITY_DOWNLOAD: [133, 1],
      SECURE_BOOT_AGGRESSIVE_REVOKE: [117, 1], DIS_DOWNLOAD_MANUAL_ENCRYPT: [52, 1],
      DIS_PAD_JTAG: [51, 1], SOFT_DIS_JTAG: [48, 3], SECURE_VERSION: [142, 16],
      DIS_USB_JTAG: [41, 1],
    },
    "ESP32-C6": {
      SECURE_BOOT_EN: [116, 1], SPI_BOOT_CRYPT_CNT: [82, 3],
      DIS_DOWNLOAD_MODE: [128, 1], ENABLE_SECURITY_DOWNLOAD: [133, 1],
      SECURE_BOOT_AGGRESSIVE_REVOKE: [117, 1], DIS_DOWNLOAD_MANUAL_ENCRYPT: [52, 1],
      DIS_PAD_JTAG: [51, 1], SOFT_DIS_JTAG: [48, 3], SECURE_VERSION: [142, 16],
      DIS_USB_JTAG: [41, 1], DIS_USB_SERIAL_JTAG: [43, 1],
    },
  };
  for (const [chip, expected] of Object.entries(CSV)) {
    const table = securityFieldsFor(chip);
    assert.ok(table, `no verified eFuse table for ${chip}`);
    const got = Object.fromEntries(table.map((f) => [f.key, [f.bit, f.width]]));
    assert.deepEqual(got, expected, `${chip} security eFuse offsets drifted from ESP-IDF`);
    // Every field must carry the copy the report renders.
    for (const f of table) {
      assert.ok(f.label && f.meaning && f.severity, `${chip}/${f.key} is missing report copy`);
    }
  }
});

test("a chip we have no verified table for says so instead of guessing", () => {
  assert.equal(securityFieldsFor("ESP32-H2"), null);
  const r = readSecurityEfuses("ESP32-H2", virgin());
  assert.equal(r.supported, false);
  assert.equal(r.virgin, null);         // never "clean" by default
});

// ── the decode ──────────────────────────────────────────────────────────────

test("a factory-fresh block 0 reads virgin", () => {
  for (const chip of ["ESP32-S3", "ESP32-C3", "ESP32-C6"]) {
    const r = readSecurityEfuses(chip, virgin());
    assert.equal(r.supported, true);
    assert.equal(r.virgin, true, `${chip} false-alarmed on an all-zero block 0`);
    assert.equal(r.burned.length, 0);
  }
});

test("a short read is 'unread', not 'clean'", () => {
  const r = readSecurityEfuses("ESP32-S3", [0, 0]);
  assert.equal(r.unread, true);
  assert.equal(r.virgin, null);
});

test("secure boot burned by a previous owner is a stop", () => {
  const r = readSecurityEfuses("ESP32-S3", burn(virgin(), 116, 1));
  assert.equal(r.virgin, false);
  const hit = r.burned.find((f) => f.key === "SECURE_BOOT_EN");
  assert.ok(hit, "secure boot went undetected");
  assert.equal(hit.severity, "stop");
  // efuseFindings is what carries a burned field into the verdict — a raw
  // field carries `severity`, and the verdict reads `level`.
  const findings = efuseFindings(r);
  assert.equal(findings.length, 1);
  assert.equal(findings[0].level, "stop");
  assert.equal(intakeVerdict(findings).level, "stop");
});

test("efuseFindings stays silent on a virgin board and on an unsupported chip", () => {
  assert.deepEqual(efuseFindings(readSecurityEfuses("ESP32-S3", virgin())), []);
  assert.deepEqual(efuseFindings(readSecurityEfuses("ESP32-H2", virgin())), []);
  assert.deepEqual(efuseFindings(null), []);
  // Every finding must carry the sentence the report renders.
  for (const f of efuseFindings(readSecurityEfuses("ESP32-S3", burn(virgin(), 116, 1)))) {
    assert.ok(f.label && f.detail && f.level);
  }
});

test("oddParity counts BITS, which is what the eFuse docs mean", () => {
  // The trap: ESP-IDF's "enabled when 1 or 3 bits are set" counts bits, not
  // values. Reading it as the values 1 and 3 inverts the common 0b011 case.
  assert.equal(oddParity(0b000), false);
  assert.equal(oddParity(0b001), true);
  assert.equal(oddParity(0b010), true);
  assert.equal(oddParity(0b011), false);
  assert.equal(oddParity(0b100), true);
  assert.equal(oddParity(0b111), true);
});

test("flash encryption is live only on an ODD number of set bits", () => {
  // ESP-IDF walks the bits and flips a flag per set bit (esp_efuse_fields.c),
  // so the burn ladder 0 → 0b001 → 0b011 → 0b111 alternates on/off/on.
  const f = (v) => readSecurityEfuses("ESP32-S3", burn(virgin(), 82, v, 3))
    .fields.find((x) => x.key === "SPI_BOOT_CRYPT_CNT");
  assert.equal(f(0b000).state, "clean");
  assert.equal(f(0b001).state, "active");   // one bit  → encryption ON
  assert.equal(f(0b011).state, "touched");  // two bits → turned back OFF
  assert.equal(f(0b111).state, "active");   // three    → ON again
  assert.equal(f(0b010).state, "active");   // one bit, whichever bit it is
  assert.equal(f(0b110).state, "touched");
});

test("an encrypted board is refused, a formerly-encrypted one is flagged", () => {
  const scan = (v) => readSecurityEfuses("ESP32-S3", burn(virgin(), 82, v, 3));
  // Live encryption: the board can't take our firmware at all.
  assert.equal(intakeVerdict(efuseFindings(scan(0b001))).level, "stop");
  // Burned then undone: usable, but this chip has been through somebody's
  // provisioning and must not read as factory-fresh.
  const undone = intakeVerdict(efuseFindings(scan(0b011)));
  assert.equal(undone.level, "attention");
  assert.match(undone.findings[0].label, /undone/);
  assert.equal(scan(0b011).virgin, false);
});

test("JTAG lock bits are reported as touched, without claiming a live state", () => {
  // We could not confirm SOFT_DIS_JTAG's parity rule against IDF source the
  // way we did for the crypt counter, so the claim is only the one that holds
  // either way: a factory board reads zero and this one doesn't.
  const f = (v) => readSecurityEfuses("ESP32-S3", burn(virgin(), 48, v, 3))
    .fields.find((x) => x.key === "SOFT_DIS_JTAG");
  assert.equal(f(0).burned, false);
  for (const v of [1, 2, 3, 7]) {
    assert.equal(f(v).burned, true, `SOFT_DIS_JTAG=${v} should read as touched`);
  }
  const table = securityFieldsFor("ESP32-S3").find((x) => x.key === "SOFT_DIS_JTAG");
  assert.ok(!/JTAG is (off|disabled) (right )?now/i.test(table.meaning),
    "the copy must not assert the current JTAG state we can't verify");
});

test("an anti-rollback floor set by a previous owner is caught", () => {
  const r = readSecurityEfuses("ESP32-S3", burn(virgin(), 142, 5, 16));
  const hit = r.burned.find((f) => f.key === "SECURE_VERSION");
  assert.ok(hit);
  assert.equal(hit.value, 5);
  assert.equal(hit.severity, "attention");
});

test("the USB-JTAG field really does move between chips", () => {
  // Burn S3's bit 118 and read it back on a C3 table: it must NOT land on
  // C3's DIS_USB_JTAG (bit 41). This is the check that would have caught
  // pasting one chip's offsets into another's row.
  const s3 = readSecurityEfuses("ESP32-S3", burn(virgin(), 118, 1));
  assert.ok(s3.burned.some((f) => f.key === "DIS_USB_JTAG"));
  const c3 = readSecurityEfuses("ESP32-C3", burn(virgin(), 118, 1));
  assert.ok(!c3.burned.some((f) => f.key === "DIS_USB_JTAG"));
  const c3real = readSecurityEfuses("ESP32-C3", burn(virgin(), 41, 1));
  assert.ok(c3real.burned.some((f) => f.key === "DIS_USB_JTAG"));
});

// ── the flash-size lie ──────────────────────────────────────────────────────

const MB = 1024 * 1024;
const HEAD = Uint8Array.from({ length: 64 }, (_, i) => i);   // distinguishable content
const OTHER = new Uint8Array(64).fill(0xa5);

test("the probe addresses are the candidate capacities, not the declared end", () => {
  // The whole check turns on WHERE we read. On a 4 MB die claiming 16 MB,
  // "declared - 4KB" wraps to 0x3FF000 — the top of the real part, which does
  // NOT mirror offset zero. Only the capacity boundaries alias the head.
  const c = flashAliasCandidates(16 * MB);
  assert.ok(c.includes(4 * MB), "4 MB must be probed — it's the classic fake");
  assert.ok(c.includes(8 * MB));
  assert.ok(!c.includes(16 * MB), "the declared size itself can't alias");
  assert.ok(!c.some((x) => x > 16 * MB));
  assert.ok(!c.includes(16 * MB - 0x1000),
    "declared-minus-a-sector is the address that does NOT work");
  assert.deepEqual(flashAliasCandidates(256 * 1024), []);
});

test("a 4 MB part sold as 16 MB is caught, and named at its real size", () => {
  // Physical 4 MB: reads at 4, 8 and 12 MB all wrap to offset zero.
  const probes = flashAliasCandidates(16 * MB).map((at) => ({
    atBytes: at, bytes: at % (4 * MB) === 0 ? HEAD.slice() : OTHER.slice(),
  }));
  const v = flashAliasVerdict({ declaredBytes: 16 * MB, head: HEAD, probes });
  assert.equal(v.level, "stop");
  assert.match(v.label, /4 MB/);
  assert.match(v.label, /not the 16 MB/);
});

test("the naive probe this replaced would have missed it", () => {
  // Regression guard for the original bug: comparing the head against the
  // last sector of the DECLARED size reads real-part content, not a mirror,
  // so it looks clean. That single non-matching probe must not say "clear"
  // by itself — the candidate boundaries are what carry the verdict, and
  // with none of them probed the honest answer is "inconclusive".
  const naive = [{ atBytes: 16 * MB - 0x1000, bytes: OTHER.slice() }];
  assert.equal(flashAliasVerdict({ declaredBytes: 16 * MB, head: HEAD, probes: naive }).level,
    "inconclusive");
  const real = flashAliasCandidates(16 * MB).map((at) => ({
    atBytes: at, bytes: at % (4 * MB) === 0 ? HEAD.slice() : OTHER.slice(),
  }));
  assert.equal(flashAliasVerdict({ declaredBytes: 16 * MB, head: HEAD, probes: real }).level,
    "stop");
});

test("probes that stop short of the claim never invent a 'clear'", () => {
  // "Clear" is a positive claim about EVERY capacity below the declared
  // size. Hand the verdict only the low candidates (the reads stopped short)
  // and it must say inconclusive, not vouch for capacities it never saw —
  // the same rule the desktop twin (intake.rs) enforces on a truncated dump.
  const partial = flashAliasCandidates(16 * MB)
    .filter((at) => at < 1 * MB)
    .map((at) => ({ atBytes: at, bytes: OTHER.slice() }));
  const v = flashAliasVerdict({ declaredBytes: 16 * MB, head: HEAD, probes: partial });
  assert.equal(v.level, "inconclusive");
  assert.match(v.label, /16 MB/);
  // An alias hit inside the readable range still refuses the board outright.
  const mirrored = partial.concat([{ atBytes: 1 * MB, bytes: HEAD.slice() }]);
  assert.equal(
    flashAliasVerdict({ declaredBytes: 16 * MB, head: HEAD, probes: mirrored }).level,
    "stop");
});

test("a genuinely sized chip reads clear", () => {
  const probes = flashAliasCandidates(8 * MB).map((at) => ({ atBytes: at, bytes: OTHER.slice() }));
  assert.equal(flashAliasVerdict({ declaredBytes: 8 * MB, head: HEAD, probes }).level, "clear");
});

test("a blank chip is inconclusive, never an accusation", () => {
  const blank = new Uint8Array(64).fill(0xff);
  const probes = flashAliasCandidates(4 * MB).map((at) => ({ atBytes: at, bytes: blank.slice() }));
  const v = flashAliasVerdict({ declaredBytes: 4 * MB, head: blank, probes });
  assert.equal(v.level, "inconclusive");
  assert.match(v.detail, /after firmware is written/);
});

test("missing evidence is 'not checked', which never fails the verdict", () => {
  assert.equal(flashAliasVerdict({}).level, "unknown");
  assert.equal(flashAliasVerdict({ declaredBytes: 4 * MB, head: HEAD, probes: [] }).level, "unknown");
  assert.equal(intakeVerdict([flashAliasVerdict({})]).level, "clear");
});

test("looksUniform separates blank from written", () => {
  assert.equal(looksUniform(new Uint8Array(16).fill(0xff)), true);
  assert.equal(looksUniform(new Uint8Array(16)), true);
  assert.equal(looksUniform(Uint8Array.of(0, 0, 1)), false);
});

// ── the MAC ─────────────────────────────────────────────────────────────────

test("MAC checks assert only what is definitional", () => {
  assert.equal(macChecks("7c:df:a1:00:11:22").level, "clear");
  assert.equal(macChecks("00:00:00:00:00:00").level, "stop");
  assert.equal(macChecks("ff:ff:ff:ff:ff:ff").level, "stop");
  assert.equal(macChecks("01:df:a1:00:11:22").level, "stop");       // multicast bit
  assert.equal(macChecks("02:df:a1:00:11:22").level, "attention");  // locally administered
  assert.equal(macChecks(null).level, "unknown");
  assert.equal(macChecks("nonsense").level, "unknown");
});

test("parseMac takes both the string and the byte array esptool returns", () => {
  assert.deepEqual(parseMac("7c:df:a1:00:11:22"), [0x7c, 0xdf, 0xa1, 0, 0x11, 0x22]);
  assert.deepEqual(parseMac("7c-df-a1-00-11-22"), [0x7c, 0xdf, 0xa1, 0, 0x11, 0x22]);
  assert.deepEqual(parseMac([0x7c, 0xdf, 0xa1, 0, 0x11, 0x22]), [0x7c, 0xdf, 0xa1, 0, 0x11, 0x22]);
  assert.equal(parseMac("7c:df:a1"), null);
});

test("two boards sharing one address is louder than any vendor list", () => {
  const roster = [{ n: 1, mac: "7C:DF:A1:00:11:22", product: "Canary" }];
  assert.equal(duplicateMacCheck(roster, "7c:df:a1:00:11:22").level, "attention");
  assert.equal(duplicateMacCheck(roster, "7c:df:a1:00:11:23").level, "clear");
  assert.equal(duplicateMacCheck([], "7c:df:a1:00:11:22").level, "clear");
});

// ── what it arrived running ─────────────────────────────────────────────────

test("stock images are recognized and named", () => {
  assert.equal(shippedWith({ projectName: "micropython" }).kind, "known");
  assert.equal(shippedWith({ projectName: "circuitpython-esp32s3" }).kind, "known");
  assert.equal(shippedWith({ projectName: "esp-at" }).kind, "known");
  assert.equal(shippedWith({ projectName: "blink" }).kind, "known");
  for (const k of KNOWN_STOCK) assert.ok(k.name, "every stock entry needs a display name");
});

test("an unrecognized image is flagged for a look, not called malware", () => {
  const r = shippedWith({ projectName: "vendor_demo_v3" });
  assert.equal(r.kind, "unknown");
  assert.equal(r.level, "attention");
  assert.match(r.detail, /Not proof of anything/);
});

test("an erased chip is the cleanest arrival, not a mystery", () => {
  const r = shippedWith({ blank: true });
  assert.equal(r.kind, "blank");
  assert.equal(r.level, "clear");
});

test("firmware with no readable descriptor is flagged, not assumed blank", () => {
  assert.equal(shippedWith({}).kind, "unreadable");
  assert.equal(shippedWith({}).level, "attention");
});

test("a failed read is unchecked, never 'arrived erased'", () => {
  // The inversion to avoid: a chip we could not read at all reporting as the
  // single cleanest outcome on the page.
  const failed = shippedWith({ read: "failed" });
  assert.equal(failed.kind, "unread");
  assert.equal(failed.level, "inconclusive");
  assert.notEqual(failed.kind, "blank");
  assert.equal(intakeVerdict([failed]).level, "inconclusive");
  // And a read that DID succeed on an empty chip is still the clean answer.
  assert.equal(shippedWith({ read: "ok", blank: true }).level, "clear");
});

// ── cold boot + first contact ───────────────────────────────────────────────

test("cold boot is reported as what the user did, not as a measurement", () => {
  const held = coldBootVerdict({ heldBoot: true, hadResidentFirmware: true });
  assert.equal(held.level, "clear");
  assert.match(held.detail, /never executed/);

  const hot = coldBootVerdict({ heldBoot: false, hadResidentFirmware: true });
  assert.equal(hot.level, "attention");
  assert.match(hot.detail, /hold BOOT/);

  // Nothing on the chip means nothing ran, whichever way it was plugged in.
  assert.equal(coldBootVerdict({ heldBoot: false, hadResidentFirmware: false }).level, "clear");
});

test("first contact is waived only by evidence the board can't forge", () => {
  assert.equal(isFirstContact({}), true);
  // Our own session history — we wrote this exact MAC.
  assert.equal(isFirstContact({ rosterHit: { n: 1 } }), false);
  // A human explicitly claiming the board.
  assert.equal(isFirstContact({ ownerClaimed: true }), false);
});

test("a hostile image cannot talk its way out of the erase", () => {
  // The bug this replaced: the resident esp_app_desc_t project name waived
  // the erase, so an image only had to call itself a known SecuraCV product
  // to skip the wipe that would have removed it. That string lives in
  // writable flash on an untrusted board, so it must not be an input at all.
  for (const claim of ["canary-wap", "Canary Sense", "canary-vision"]) {
    assert.equal(
      isFirstContact({ current: { productName: claim, projectName: claim, version: "2.3.0" } }),
      true,
      `a board announcing itself as "${claim}" must still be first contact`);
  }
  assert.ok(!/\bcurrent\b/.test(isFirstContact.toString()),
    "isFirstContact must not read the board's own firmware claim");
});

// ── the overall verdict ─────────────────────────────────────────────────────

test("the verdict takes the worst finding and lists only what matters", () => {
  const clear = intakeVerdict([{ level: "clear" }, { level: "clear" }]);
  assert.equal(clear.level, "clear");
  assert.equal(clear.findings.length, 0);

  const mixed = intakeVerdict([
    { level: "clear" }, { level: "attention", label: "a" }, { level: "inconclusive", label: "b" },
  ]);
  assert.equal(mixed.level, "attention");
  assert.equal(mixed.findings.length, 2);

  const stop = intakeVerdict([{ level: "attention" }, { level: "stop", label: "locked" }]);
  assert.equal(stop.level, "stop");
  assert.equal(stop.headline, "Don't use this board");
});

test("every verdict level has a headline", () => {
  for (const lvl of ["clear", "inconclusive", "attention", "stop"]) {
    assert.ok(intakeVerdict([{ level: lvl }]).headline, `no headline for ${lvl}`);
  }
});
