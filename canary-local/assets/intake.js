// canary-local/assets/intake.js — customs, for a board we've never met.
//
// A board bought from a marketplace arrives carrying *someone else's* flash
// contents and *someone else's* eFuse state. This module is the read-only
// intake check that runs before we write a byte: what silicon is this, has it
// been provisioned by a previous owner, is the flash the size it claims, and
// what did it ship running?
//
// Everything here is PURE — the caller does the I/O (esptool `readReg` /
// `readFlash`) and hands the bytes in. That keeps the whole verdict testable
// without a board on the desk, which matters because the interesting cases
// (secure boot already burned, a 4 MB chip sold as 16 MB) are exactly the ones
// nobody has lying around.
//
// What this deliberately does NOT do:
//   - It never WRITES an eFuse. The flasher's un-brickable promise rests on
//     never issuing a burn (docs/browser_flasher.md § Why it's safe to offer);
//     reading block 0 is a plain register read and keeps that promise intact.
//   - It does not check the MAC against a vendor OUI list. We can't ship a
//     list we can verify, and a wrong one would cry wolf on honest boards, so
//     we assert only the structural invariants that are true by definition.
//   - It does not claim to recognize arbitrary firmware. It matches a handful
//     of well-known project names and says "unrecognized" otherwise, which is
//     a prompt to look, not an accusation.

// ── eFuse block 0: where the security bits live ─────────────────────────────
// Block 0 read registers start at EFUSE_BASE + 0x02C and run 6 words (esptool
// espefuse/efuse/esp32s3/mem_definition.py: BLOCK0 at __base_rd_regs + 0x02C,
// length 6). Bit N of block 0 is bit (N % 32) of word floor(N / 32) — word 0
// is WR_DIS, word 1 is the first repeat-data register.
//
// EFUSE_BASE itself is NOT hardcoded here: the caller passes
// `esploader.chip.EFUSE_BASE`, so the address always agrees with the esptool
// build we vendored rather than with a constant that can drift.
export const EFUSE_BLOCK0_RD_OFFSET = 0x02c;
export const EFUSE_BLOCK0_WORDS = 6;

export function efuseBlock0Addrs(efuseBase) {
  if (!Number.isFinite(efuseBase)) return [];
  const base = efuseBase + EFUSE_BLOCK0_RD_OFFSET;
  return Array.from({ length: EFUSE_BLOCK0_WORDS }, (_, i) => base + 4 * i);
}

// Pull a field out of the block-0 words. Widths here are ≤ 16 bits and never
// straddle a word boundary in the tables below, but the shift-and-carry is
// written generally so a future field can't silently truncate.
export function efuseField(words, bit, width) {
  if (!Array.isArray(words) || !Number.isFinite(bit) || !Number.isFinite(width)) return null;
  let out = 0;
  for (let i = 0; i < width; i++) {
    const abs = bit + i;
    const w = words[Math.floor(abs / 32)];
    if (typeof w !== "number") return null;
    if ((w >>> (abs % 32)) & 1) out |= 1 << i;
  }
  return out >>> 0;
}

// Several ESP32 counter eFuses are read by the PARITY of their set bits, not
// by their numeric value: ESP-IDF's flash-encryption check walks the bits and
// flips a flag for each one set (esp_efuse_fields.c), so 0b001 and 0b111 are
// "enabled" while 0b011 is "disabled again". The CSV comment
// "…when 1 or 3 bits are set" counts BITS, not values — reading it as the
// values 1 and 3 gets the common 0b011 case exactly backwards.
export function oddParity(v) {
  let odd = false;
  for (let x = v >>> 0; x; x >>>= 1) if (x & 1) odd = !odd;
  return odd;
}

// The security fields, with block-0 bit offsets taken verbatim from ESP-IDF's
// components/efuse/<chip>/esp_efuse_table.csv. Only fields that are ZERO on a
// factory-fresh part are listed: Espressif does burn eFuses at the factory
// (flash/PSRAM capability, wafer version, and the WR_DIS bits protecting
// them), but those live in blocks 1–2 and in fields we deliberately don't
// read — so a non-zero reading here means a *person* burned it, not the
// factory. That distinction is the whole value of the check; a field whose
// factory default we weren't sure of would turn every honest board into a
// false alarm, so it isn't in this table.
const COMMON_EFUSES = [
  { key: "SECURE_BOOT_EN", bit: 116, width: 1, severity: "stop",
    label: "Secure boot",
    meaning: "This board is locked to somebody else's signing key. Its ROM will refuse our bootloader, and there is no way to undo it — eFuses are one-way." },
  { key: "SPI_BOOT_CRYPT_CNT", bit: 82, width: 3, severity: "stop",
    label: "Flash encryption",
    meaning: "Flash is encrypted with a key burned into this chip. Reads come back as ciphertext and a plaintext image won't boot. Not reversible.",
    // Odd number of BITS set = encryption on (0b001, 0b111); an even count
    // means it was turned on and then off again (0b011). Both mean a person
    // was here, so an even count is still worth saying out loud — just not
    // a reason to refuse the board.
    active: (v) => oddParity(v),
    touched: { severity: "attention",
      meaning: "Flash encryption was switched on and then off again by a previous owner. It isn't active now, so the board still works — but this chip has been through somebody's provisioning, and the counter can only be burned a couple more times." } },
  { key: "DIS_DOWNLOAD_MODE", bit: 128, width: 1, severity: "stop",
    label: "Download mode disabled",
    meaning: "Somebody burned away the USB recovery path. This is the one state a Canary cannot be flashed out of." },
  { key: "ENABLE_SECURITY_DOWNLOAD", bit: 133, width: 1, severity: "stop",
    label: "Secure download mode",
    meaning: "Download mode is restricted to a reduced command set — no flash reads, no backup, so we can't verify what's on it." },
  { key: "SECURE_BOOT_AGGRESSIVE_REVOKE", bit: 117, width: 1, severity: "attention",
    label: "Aggressive key revocation",
    meaning: "Set alongside secure boot by a previous owner." },
  { key: "DIS_DOWNLOAD_MANUAL_ENCRYPT", bit: 52, width: 1, severity: "attention",
    label: "Manual encryption disabled",
    meaning: "Part of a flash-encryption lockdown someone applied." },
  { key: "DIS_PAD_JTAG", bit: 51, width: 1, severity: "attention",
    label: "JTAG permanently disabled",
    meaning: "Hardware debug was burned off. Harmless to run, but a factory-fresh board never has this set." },
  // Also a bit-counter, and ESP-IDF's "odd number 1 means disable" wording
  // suggests the same parity rule as the crypt counter — but we could not
  // confirm that against IDF source the way we did for SPI_BOOT_CRYPT_CNT.
  // It doesn't matter for what we actually claim: we are not reporting
  // whether JTAG is off right now, we are reporting that a factory-fresh
  // board reads zero here and this one doesn't. Any non-zero value is a
  // person's fingerprint whichever way the parity falls, so that is the
  // claim the copy makes.
  { key: "SOFT_DIS_JTAG", bit: 48, width: 3, severity: "attention",
    label: "JTAG lock bits",
    meaning: "Somebody burned the JTAG soft-disable bits. A factory-fresh board reads zero here. (We don't claim what state debug is in right now — only that this chip left the factory untouched and isn't untouched any more.)" },
  { key: "SECURE_VERSION", bit: 142, width: 16, severity: "attention",
    label: "Anti-rollback floor",
    meaning: "An anti-rollback minimum was burned in. The board will refuse firmware it considers older — including, possibly, ours." },
];

// The USB/JTAG switch moved between chip generations, so it's the one field
// that can't be shared. (S3: DIS_USB_JTAG 118, DIS_USB_SERIAL_JTAG 119.
// C3/C6: DIS_USB_JTAG 41; C6 also has DIS_USB_SERIAL_JTAG at 43.)
const USB_EFUSES = {
  "ESP32-S3": [
    { key: "DIS_USB_JTAG", bit: 118, width: 1 },
    { key: "DIS_USB_SERIAL_JTAG", bit: 119, width: 1 },
  ],
  "ESP32-C3": [
    { key: "DIS_USB_JTAG", bit: 41, width: 1 },
  ],
  "ESP32-C6": [
    { key: "DIS_USB_JTAG", bit: 41, width: 1 },
    { key: "DIS_USB_SERIAL_JTAG", bit: 43, width: 1 },
  ],
};

const USB_META = {
  severity: "attention",
  label: "USB debug disabled",
  meaning: "The USB-JTAG path was burned off by a previous owner. A new board never has this set.",
};

export function securityFieldsFor(chip) {
  const usb = USB_EFUSES[chip];
  if (!usb) return null;             // a chip we have no verified table for
  return [...COMMON_EFUSES, ...usb.map((f) => ({ ...USB_META, ...f }))];
}

// Decode block 0 into a verdict. `words` is the 6-word read; a chip we have no
// verified bit table for returns `supported: false` rather than guessing.
export function readSecurityEfuses(chip, words) {
  const table = securityFieldsFor(chip);
  if (!table) return { supported: false, chip, fields: [], burned: [], virgin: null };
  if (!Array.isArray(words) || words.length < EFUSE_BLOCK0_WORDS) {
    return { supported: true, chip, fields: [], burned: [], virgin: null, unread: true };
  }
  // Three states, not two. "active" is the dangerous reading (encryption on,
  // secure boot on). "touched" is a field that was burned but isn't currently
  // in force — it can't hurt the install, but it proves a person was here, and
  // silently folding it into "clean" would hand a used board a clean bill of
  // health. Only fields that can actually be un-set declare a `touched` rule;
  // for everything else burned means active.
  const fields = table.map((f) => {
    const value = efuseField(words, f.bit, f.width);
    const active = f.active ? f.active(value) : value > 0;
    const touched = value > 0;
    let state = "clean", severity = null, meaning = null;
    if (active) { state = "active"; severity = f.severity; meaning = f.meaning; }
    else if (touched && f.touched) {
      state = "touched"; severity = f.touched.severity; meaning = f.touched.meaning;
    }
    return { key: f.key, label: f.label, value, state, severity, meaning,
             burned: state !== "clean" };
  });
  const burned = fields.filter((f) => f.burned);
  return { supported: true, chip, words: words.slice(0, EFUSE_BLOCK0_WORDS),
           fields, burned, virgin: burned.length === 0 };
}

// Turn an eFuse scan into findings the verdict understands. A burned field's
// `severity` IS its level — the two vocabularies are deliberately the same —
// but the mapping lives here so every caller renders the same sentence.
export function efuseFindings(scan) {
  if (!scan || !scan.supported) return [];
  return (scan.burned || []).map((f) => ({
    level: f.severity,
    label: f.state === "active"
      ? `${f.label} is already burned in`
      : `${f.label} was burned and undone`,
    detail: f.meaning,
    key: f.key,
  }));
}

// ── is the flash really the size it says? ───────────────────────────────────
// A flash chip reports its capacity in its SPI JEDEC id, and a relabeled part
// simply lies: address lines above the real capacity wrap around, so the top
// of the "16 MB" chip is a mirror of the bottom of the real 4 MB one. Reading
// both ends and comparing catches it without writing anything.
//
// The catch is that it's only conclusive when offset 0 holds something
// distinguishable. On a blank (all-0xFF) chip both ends match legitimately, so
// we say "inconclusive" rather than crying fraud — and the caller can re-run
// it after the write, when the bootloader gives the head a fingerprint.
export function looksUniform(bytes) {
  if (!bytes || bytes.length === 0) return true;
  const first = bytes[0];
  for (let i = 1; i < bytes.length; i++) if (bytes[i] !== first) return false;
  return true;
}

function sameBytes(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

// WHERE you read matters, and getting it wrong makes the check useless. If a
// 4 MB part claims 16 MB, address 0xFFF000 (declared − 4 KB) wraps modulo the
// REAL 4 MB to 0x3FF000 — the top of the real part, which holds something
// different from offset 0, so a naive "compare the two ends" probe sees two
// unequal blocks and happily calls a counterfeit genuine.
//
// The addresses that actually alias offset 0 are the candidate capacities
// themselves: on a real 4 MB die, address 0x400000 IS address 0. So we probe
// each power-of-two capacity below the declared size and look for the one
// that mirrors the head.
export function flashAliasCandidates(declaredBytes) {
  const out = [];
  for (let size = 256 * 1024; size < declaredBytes; size *= 2) out.push(size);
  return out;
}

export function flashAliasVerdict({ declaredBytes, head, probes } = {}) {
  if (!declaredBytes || !head || !Array.isArray(probes) || !probes.length) {
    return { level: "unknown", label: "Flash size not checked" };
  }
  if (looksUniform(head)) {
    return { level: "inconclusive",
      label: "Flash size can't be confirmed yet — the chip is blank",
      detail: "Every byte at the start of the chip is the same, so there's no pattern to " +
              "look for further up. On a blank part a mirror and an honest chip read " +
              "identically. The check becomes conclusive after firmware is written." };
  }
  // The smallest aliasing candidate is the real capacity: a 4 MB die mirrors
  // at 4, 8 and 12 MB, so the first hit is the true size.
  const hit = probes.find((p) => p && p.bytes && sameBytes(head, p.bytes));
  if (hit) {
    return { level: "stop",
      label: `Flash is ${formatSize(hit.atBytes)}, not the ${formatSize(declaredBytes)} it claims`,
      detail: `Reading at ${formatSize(hit.atBytes)} returns the same bytes as offset zero — ` +
              "the address lines are wrapping, which is exactly what a relabeled flash " +
              "part does. Anything written past the real capacity would be silently lost." };
  }
  // "Clear" is a positive claim about EVERY capacity below the declared size,
  // so it needs a probe at every candidate. A caller whose reads stopped short
  // (a flaky cable, a partial dump) gets "inconclusive" — missing evidence
  // must never read as a passed check. (Same rule as the desktop twin,
  // desktop/src-tauri/src/intake.rs.)
  const probed = new Set(probes.filter((p) => p && p.bytes).map((p) => p.atBytes));
  const unread = flashAliasCandidates(declaredBytes).find((at) => !probed.has(at));
  if (unread !== undefined) {
    return { level: "inconclusive",
      label: `Flash size can't be fully confirmed — the probes stop short of the ${formatSize(declaredBytes)} claim`,
      detail: `Capacities at and above ${formatSize(unread)} were never read. Nothing that ` +
              "was probed aliases, but an unread capacity can't be vouched for." };
  }
  return { level: "clear", label: `Flash reads a genuine ${formatSize(declaredBytes)}` };
}

function formatSize(bytes) {
  if (!bytes) return "?";
  const mb = bytes / (1024 * 1024);
  return Number.isInteger(mb) ? `${mb} MB` : `${mb.toFixed(1)} MB`;
}

// ── the MAC, checked only where the answer is definitional ──────────────────
// We assert the three things an IEEE-assigned station MAC cannot be, rather
// than testing membership of a vendor list we'd have to keep honest. A clone
// with a hand-rolled MAC usually trips one of these; one that doesn't is
// caught by the duplicate check instead.
export function macChecks(mac) {
  const bytes = parseMac(mac);
  if (!bytes) return { level: "unknown", label: "No MAC read" };
  if (bytes.every((b) => b === 0x00) || bytes.every((b) => b === 0xff)) {
    return { level: "stop", label: "MAC is blank",
      detail: "An all-zero or all-ones MAC means the address was never programmed — " +
              "a hallmark of a cloned or reject part." };
  }
  if (bytes[0] & 0x01) {
    return { level: "stop", label: "MAC is a multicast address",
      detail: "The low bit of the first byte marks a multicast address, which can never " +
              "be a factory-assigned station MAC." };
  }
  if (bytes[0] & 0x02) {
    return { level: "attention", label: "MAC is locally administered",
      detail: "This address was assigned by software, not burned in at the factory." };
  }
  return { level: "clear", label: "MAC is a factory-assigned address" };
}

export function parseMac(mac) {
  if (Array.isArray(mac)) return mac.length === 6 ? mac.map((n) => n & 0xff) : null;
  const parts = String(mac || "").trim().split(/[:\-]/);
  if (parts.length !== 6) return null;
  const bytes = parts.map((p) => parseInt(p, 16));
  return bytes.some((n) => !Number.isFinite(n)) ? null : bytes;
}

// Have we already onboarded this exact MAC? Two "new" boards with one address
// is a much louder signal than anything a vendor list would give us, and the
// roster is already keyed by MAC for the batch guard.
export function duplicateMacCheck(roster, mac) {
  if (!mac || !Array.isArray(roster)) return { level: "clear" };
  const hit = roster.find((r) => r && r.mac && r.mac.toLowerCase() === String(mac).toLowerCase());
  if (!hit) return { level: "clear" };
  return { level: "attention", label: "This MAC has been seen before",
    detail: `A board with this exact address was already onboarded this session (#${hit.n}). ` +
            `Reflashing the same board is normal; two different boards sharing one address is not.` };
}

// ── what did it arrive running? ─────────────────────────────────────────────
// Matched on the `esp_app_desc_t` project name and partition labels — real
// strings read off the board, not hashes of images we'd have to claim to have
// cataloged. Anything unmatched is reported as unrecognized, which is an
// invitation to look at the backup, not a verdict.
export const KNOWN_STOCK = [
  { match: /^micropython/i, name: "MicroPython" },
  { match: /^circuitpython/i, name: "CircuitPython" },
  { match: /^(esp-at|at)$/i, name: "Espressif AT firmware" },
  { match: /^arduino/i, name: "an Arduino sketch" },
  { match: /^esphome/i, name: "ESPHome" },
  { match: /^tasmota/i, name: "Tasmota" },
  { match: /^(hello[-_ ]?world|blink)/i, name: "a factory test sketch" },
];

// `read` is the outcome of actually pulling the partition sector off the chip:
// "ok" (we got bytes), or "failed" (the read threw). These must stay distinct
// from what we found in those bytes — treating a failed read as an empty chip
// would turn missing evidence into the single cleanest verdict on the page,
// which is the exact inversion this whole module exists to avoid.
export function shippedWith({ projectName, blank, read = "ok" } = {}) {
  if (read !== "ok") {
    return { level: "inconclusive", kind: "unread",
      label: "We couldn't read what's on the chip",
      detail: "The partition sector wouldn't come back over USB, so we don't know what " +
              "this board arrived running — that's unchecked, not clean. Worth a retry " +
              "on a different cable or port. The full erase below removes whatever is " +
              "there either way." };
  }
  if (blank) {
    return { level: "clear", kind: "blank", label: "The chip arrived erased",
      detail: "Nothing was on it to run. This is the cleanest state a board can arrive in." };
  }
  if (!projectName) {
    return { level: "attention", kind: "unreadable", label: "Arrived with firmware we couldn't identify",
      detail: "There's something on the chip but no readable app descriptor. The full erase " +
              "below removes it either way; the backup keeps a copy if you want to look." };
  }
  const hit = KNOWN_STOCK.find((k) => k.match.test(projectName));
  if (hit) {
    return { level: "clear", kind: "known", name: hit.name,
      label: `Arrived running ${hit.name}`,
      detail: "A common stock image for boards sold this way." };
  }
  return { level: "attention", kind: "unknown", name: projectName,
    label: `Arrived running “${projectName}” — we don't recognize it`,
    detail: "Not proof of anything: plenty of honest vendors ship their own demo. " +
            "It is worth a look at the backup before you erase it, though." };
}

// ── the overall verdict ─────────────────────────────────────────────────────
// `stop` means don't use this board — every one of those is a state a full
// erase cannot fix, because eFuses are one-way. `attention` means it isn't
// factory-fresh and you should know why. `clear` means nothing we can check
// looks wrong, which is not the same as "proven safe" — see the doc.
const ORDER = ["clear", "inconclusive", "attention", "stop"];
const RANK = { unknown: 0, clear: 0, inconclusive: 1, attention: 2, stop: 3 };

export function intakeVerdict(parts = []) {
  const findings = parts.filter((p) => p && RANK[p.level] >= 1);
  const worst = parts.reduce((n, p) => Math.max(n, RANK[(p && p.level)] || 0), 0);
  const level = ORDER[worst];
  return { level, findings, headline: HEADLINE[level] };
}

const HEADLINE = {
  clear: "Nothing on this board looks wrong",
  inconclusive: "Mostly clear — one check couldn't finish",
  attention: "This board is not factory-fresh",
  stop: "Don't use this board",
};

// Was the board cold when we met it — did it come up in the ROM's download
// mode without ever executing the firmware that shipped on it?
//
// This is the most valuable step in the whole flow, and it is the one the
// *user* performs, not us: holding BOOT while plugging in means the resident
// image never runs an instruction, so it can never present itself as a
// keyboard and type at the desktop.
//
// We cannot detect it after the fact, and we don't pretend to. On a native-USB
// board the ROM's download mode and a stock hwcdc firmware present the *same*
// USB descriptor (docs/browser_flasher.md § What the Canary is called over
// USB), so the port tells us nothing. So `heldBoot` is what the user told us
// on the way in, and the verdict says which it is rather than implying a
// measurement. A chip with nothing on it had nothing to run either way.
export function coldBootVerdict({ heldBoot, hadResidentFirmware } = {}) {
  if (!hadResidentFirmware) {
    return { level: "clear", label: "Nothing was on the chip to run",
      detail: "The board arrived erased, so there was no resident firmware to execute " +
              "when you plugged it in." };
  }
  if (heldBoot) {
    return { level: "clear", label: "You brought it up cold",
      detail: "You held BOOT, so the chip went straight into the ROM's download mode and " +
              "the firmware it shipped with never executed a single instruction." };
  }
  return { level: "attention", label: "Its own firmware ran before we met it",
    detail: "You plugged in without holding BOOT, so whatever shipped on the chip booted " +
            "for a moment first — long enough to present itself to your computer however " +
            "it liked. Nothing here can undo that after the fact. Next time hold BOOT " +
            "while the cable goes in and it never gets the chance." };
}

// A board is on its "first contact" until something WE trust says otherwise.
// First contact is what makes the full erase mandatory — a normal install
// writes only the regions the image covers, so anything sitting in a
// partition we don't touch would survive onto a board the user now believes
// is theirs.
//
// Note what is deliberately NOT consulted here: the firmware already on the
// chip. An earlier version of this waived the erase when the resident
// `esp_app_desc_t` named a known SecuraCV product — but that string sits in
// writable flash on an untrusted board, so a hostile image only had to call
// itself "canary-wap" to skip the very erase that would have removed it. The
// check was doing the attacker's work for them.
//
// Two things can waive it, and both originate outside the board:
//   - `rosterHit`    — this exact MAC was onboarded by US, earlier this session
//   - `ownerClaimed` — the human said "this is my board, keep its data"
// Everything else, including a board confidently announcing itself as one of
// ours, is first contact.
export function isFirstContact({ rosterHit, ownerClaimed } = {}) {
  if (rosterHit) return false;
  if (ownerClaimed) return false;
  return true;
}
