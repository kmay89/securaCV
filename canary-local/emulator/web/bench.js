// canary-local/emulator/web/bench.js — the physical bench around the board.
//
// Everything on a real bench that the firmware CANNOT see, modeled where
// it actually lives: outside the silicon boundary. The USB cable, the
// battery and its slide switch, the BOOT/RESET buttons, and the little
// hardwired lights (PWR on the rail, CHG/DONE on the charge chip) are
// power-plane facts — the ESP32 only ever experiences their consequences
// (the rail coming up, a reset with a strapping pin sampled). So this
// state machine drives the page's boot/kill machinery the same way the
// power plane drives silicon, and the wasm firmware inside stays 100%
// unmodified — the emulator remains the firmware.
//
// Honesty rules, same as everywhere on this page:
//   - The charge/power LEDs are NOT firmware-controllable, here or on the
//     real board — they are wired to the rail / charge chip. The bench
//     lets you try to prove otherwise and fail.
//   - The mask-ROM boot banner ("waiting for download") is staged by the
//     bench (the ROM is the one program we can't compile to wasm). It is
//     the board's own chip's, named by its bench block (`rom.chip`), each
//     line quoted from an Espressif source cited below, except the
//     ESP32-S3's two download-mode lines, kept from before A59 and not
//     re-sourced (its voice lists them `unsourced`); where no source quotes
//     a banner, the bench prints none and says so. Everything after the
//     banner is the real firmware talking. The bench's honesty ribbon says
//     exactly that, per board (benchRibbon()).
//
// DOM-free on purpose: tested under Node (repo convention — CI runs the
// exact shipped source). Hardware facts (which LEDs, what the switch
// gates) come from the device registry's `bench` block, never from here.

// Demo-friendly battery pace at rate ×1: full charge in ~2 minutes,
// full-to-empty on battery in ~11 minutes. The bench's fast-forward
// multiplies both — the point is watching CHG hand over to DONE and a
// dying battery brown out, not simulating a datasheet.
export const CHARGE_PCT_PER_SEC = 0.8;
export const DRAIN_PCT_PER_SEC = 0.15;

// ── The mask ROM's own voice, per chip (sweep A59) ──────────────────────
// A board's bench block names its chip (`rom.chip`) and the strapping pin
// its BOOT button pulls low (`rom.boot_strap`); the banner is looked up here
// by that chip, never assumed. Every line below is quoted, byte for byte,
// from the Espressif source cited beside it (fetched for A59 from
// github.com/espressif/esp-idf at tag v5.4), except the kinds a voice lists
// in `unsourced`: the ESP32-S3's two download-mode lines, kept from before
// A59 and not re-sourced. `boot:` is the chip's strap
// sample as the quoted board's pins read it: the BOOT strap high runs the
// app (SPI_FAST_FLASH_BOOT); held low through reset, the ROM sits waiting
// for a flasher. The RESET button is a chip power-on reset (the enable
// pin), so it prints the power-on banner, as the bench always has.
// A kind whose banner no source quotes is null: romBanner() then prints a
// one-line note saying so, never another chip's banner in its place.
export const ROM_VOICES = {
  esp32s3: {
    name: "ESP32-S3",
    // https://github.com/espressif/esp-idf/blob/v5.4/docs/en/security/esp32s3_log.inc
    // (the flash-encryption guide's ESP32-S3 boot logs). Lines 6-8, its
    // first boot:
    //   ESP-ROM:esp32s3-20210327 / Build:Mar 27 2021 /
    //   rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)
    // Lines 70-72, its restart ("already_en_enc"): the same two lines, then
    //   rst:0x3 (RTC_SW_SYS_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)
    head: "ESP-ROM:esp32s3-20210327\nBuild:Mar 27 2021\n",
    poweron: "rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)\n",
    swreset: "rst:0x3 (RTC_SW_SYS_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)\n",
    // Not in that source: this is the bench's download line as it stood
    // before A59, kept; the esp-idf v5.4 docs and examples quote no ESP32-S3
    // download-mode banner to check it against. Listed in `unsourced`, so the
    // ribbon says so on every S3 card.
    download: "rst:0x1 (POWERON),boot:0x0 (DOWNLOAD(USB/UART0))\nwaiting for download\n",
    unsourced: ["download"],
  },
  esp32c3: {
    name: "ESP32-C3",
    // https://github.com/espressif/esp-idf/blob/v5.4/examples/security/security_features_app/README.md
    // (an example whose only supported target is the ESP32-C3), lines
    // 328-330, under "Example Output" / "ROM bootloader verifying software
    // bootloader":
    //   ESP-ROM:esp32c3-api1-20210207 / Build:Feb  7 2021 /
    //   rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)
    // (two spaces before the 7, as the source has it).
    head: "ESP-ROM:esp32c3-api1-20210207\nBuild:Feb  7 2021\n",
    poweron: "rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)\n",
    // No esp-idf v5.4 doc or example quotes a production ESP32-C3's
    // software-reset or download-mode banner (docs/en/security/
    // esp32c3_log.inc shows a pre-release chip's log: no ESP-ROM line and a
    // different strap sample), so the bench prints neither and says why.
    swreset: null,
    download: null,
    unsourced: [],
  },
};

const KINDS = new Set(["poweron", "reset", "swreset", "download"]);

/**
 * The ROM banner a reset of `kind` prints on the board whose bench block's
 * `rom` is given ({ chip, boot_strap }). A chip with no voice above prints
 * a note instead of a banner; with no `rom` at all, nothing.
 * @param kind "poweron" | "reset" | "swreset" (the firmware's ESP.restart()) | "download"
 */
export function romBanner(kind, rom) {
  if (!KINDS.has(kind) || !rom || !rom.chip) return "";
  const voice = ROM_VOICES[rom.chip];
  const strap = rom.boot_strap ? `BOOT (${rom.boot_strap})` : "BOOT";
  if (!voice) {
    return `(no mask-ROM banner on file for ${rom.chip}: the bench prints none rather than another chip's)\n`;
  }
  const line = kind === "reset" ? voice.poweron : voice[kind];
  if (line) return voice.head + line;
  if (kind === "download") {
    return `(${voice.name} ROM: ${strap} was low at reset, so it waits for a flasher. Its download-mode ` +
      "banner is not reproduced: no Espressif source the Lab quotes prints it)\n";
  }
  return `(${voice.name} ROM: its banner for this reset is not reproduced: the only ${voice.name} ` +
    "banner the Lab quotes from Espressif is the power-on one)\n";
}

// ── The honesty ribbon (sweep A59 review) ───────────────────────────────
// The bench's ribbon says what is staged and what is real, per board: the
// chip whose ROM it stages (its bench block's `rom`), which of that chip's
// banners are quoted from Espressif, which are the bench's own, and which it
// prints a note for instead; then which of its lights are hardwired (a
// `rail` or `charger` driver: no firmware can drive them). It used to be one
// sentence for every card ("verbatim ESP32-S3", "PWR/CHG/DONE"), which the
// Nightlight's ESP32-C3 card, with no lights at all, contradicted.
const RIBBON_KINDS = [["poweron", "power-on"], ["swreset", "restart"], ["download", "download-mode"]];
const sayList = (words, joint) =>
  words.length < 2 ? words.join("") : `${words.slice(0, -1).join(", ")} ${joint} ${words[words.length - 1]}`;
const LED_WIRES = { rail: "the power rail", charger: "the charge chip" };
/**
 * The bench's honesty ribbon for one board, from its bench block alone.
 * @param profile the registry entry's `bench` block (rom, leds)
 */
export function benchRibbon(profile) {
  const rom = profile?.rom;
  const voice = rom && rom.chip ? ROM_VOICES[rom.chip] : null;
  let first;
  if (!rom || !rom.chip) {
    first = "Honesty: this bench names no chip, so it stages no mask-ROM banner; everything on its serial is the real firmware.";
  } else if (!voice) {
    first = `Honesty: the Lab quotes no mask-ROM banner for this board's ${rom.chip}, so the bench prints a note where the ROM would speak; everything after it is the real firmware.`;
  } else {
    const unsourced = voice.unsourced || [];
    const quoted = RIBBON_KINDS.filter(([k]) => voice[k] && !unsourced.includes(k)).map(([, w]) => w);
    const own = RIBBON_KINDS.filter(([k]) => voice[k] && unsourced.includes(k)).map(([, w]) => w);
    const none = RIBBON_KINDS.filter(([k]) => !voice[k]).map(([, w]) => w);
    const parts = [];
    if (quoted.length)
      parts.push(`its ${sayList(quoted, "and")} ${quoted.length > 1 ? "banners are" : "banner is"} quoted from Espressif's esp-idf (cited in bench.js)`);
    if (own.length)
      parts.push(`its ${sayList(own, "and")} lines are the bench's own, not checked against an Espressif source`);
    if (none.length)
      parts.push(`no Espressif source the Lab quotes prints its ${sayList(none, "or")} banner, so there the bench prints a note instead`);
    first = `Honesty: the mask-ROM banner is staged by the bench for this board's ${voice.name}: ${parts.join("; ")}. Everything after the banner is the real firmware.`;
  }
  const wired = (profile?.leds || []).filter((l) => LED_WIRES[l.driver]);
  if (!wired.length) return first;
  const labels = sayList(wired.map((l) => l.label), "and");
  const wires = sayList([...new Set(wired.map((l) => LED_WIRES[l.driver]))], "and");
  const many = wired.length > 1;
  return `${first} The ${labels} ${many ? "lights are" : "light is"} hardwired to ${wires} — ` +
    `no firmware here or on your desk can turn ${many ? "them" : "it"} off.`;
}

// ── BOOT held while the app runs (sweep A83) ────────────────────────────
// The strap is sampled at reset on every board; whether the running firmware
// also reads the pin is the board's own story, and its bench block's BOOT
// button says it (`read_while_running`, held to the button's note and to the
// firmware's pin map by bench.test.js): main.cpp's BOOT grammar (a press
// wakes the glass, a hold acknowledges) runs wherever the board's pins.h
// names a BOOT_BUTTON_PIN. The bench's BOOT chip holds the strap only: it is
// not wired to the twin's button input (emu_button), so on a board that reads
// the pin the bench says so rather than implying the firmware saw a press. A
// bench block that does not say gets neither story.
/**
 * The bench's line for BOOT held while the app runs, from the board's bench
 * block (its `rom.boot_strap` and its BOOT button's `read_while_running`).
 * @param profile the registry entry's `bench` block
 */
export function bootHeldLine(profile) {
  const boot = (profile?.buttons || []).find((b) => b.id === "boot");
  const strap = profile?.rom?.boot_strap;
  const name = strap ? `BOOT (${strap})` : "BOOT";
  if (boot?.read_while_running === false) {
    return `${name} held — a strapping pin, sampled only at reset. Running firmware ignores it.`;
  }
  if (boot?.read_while_running === true) {
    return `${name} held — a strapping pin, sampled at reset. This board's firmware also reads it while running, ` +
      "but the bench holds it as the strap only: it is not wired to the twin's button, so the firmware sees no press.";
  }
  return `${name} held — a strapping pin, sampled at reset. This board's bench block does not say whether its ` +
    "running firmware reads it.";
}

// ── The power plane ─────────────────────────────────────────────────────
// Modes: "run" (rail up, app booted), "download" (rail up, ROM waiting —
// BOOT was low at reset), "off" (no rail). The truth table the tests pin:
//
//   rail up  ⇔  USB in  OR  (battery in AND switch ON AND charge > 0)
//
// so the slide switch gates ONLY the battery path (its documented job on
// both boards), the charger runs off USB whenever USB is present (even
// with the switch off), and pulling USB with a healthy switched-on
// battery is a non-event the firmware never notices.
//
// A board whose bench block names no battery (`power.battery` absent: the
// Nightstand stick, the Nightlight's C3-LCD-1.47) has no battery path at
// all, so nothing can fit one: setBattery(true) is refused, the battery
// term stays false, and pulling USB always drops its rail (sweep A54).
export class BenchPower {
  /**
   * @param profile the registry entry's `bench` block (leds/buttons/power)
   * @param cb.onPower  (up, cause) — rail transition; page boots or kills
   * @param cb.onReset  (kind) — "reset" | "download"; page reboots or stages ROM
   * @param cb.onLog    (line) — bench narration for the serial panel
   */
  constructor(profile, cb = {}) {
    this.profile = profile || {};
    this.cb = cb;
    this.usb = true;
    this.batteryPath = !!this.profile.power?.battery; // a board with none can fit none
    this.batteryFitted = this.batteryPath;
    this.switchOn = true;
    this.soc = 62; // arrives part-charged, so CHG has a story to tell
    this.bootHeld = false;
    this.rate = 1; // battery fast-forward (bench knob, not virtual time)
    this.mode = "run"; // the page auto-boots plugged in; bench starts in sync
  }

  powered() {
    return this.usb || (this.batteryFitted && this.switchOn && this.soc > 0);
  }

  /** What is actually holding the rail up right now. */
  source() {
    if (this.usb) return "usb";
    if (this.powered()) return "battery";
    return "none";
  }

  canBoot() {
    return this.powered() && this.mode !== "download";
  }

  // ── The cable, the battery, the switch ────────────────────────────────
  setUsb(plugged) {
    if (this.usb === !!plugged) return;
    this.usb = !!plugged;
    if (this.usb) {
      this._log("USB in — power and serial.");
    } else if (this.powered()) {
      this._log("USB out — riding the battery. The firmware never noticed.");
    }
    this._recompute(this.usb ? "usb-in" : "usb-out");
  }

  setBattery(fitted) {
    if (fitted && !this.batteryPath) {
      this._log("no battery to connect — this board has no battery path; USB is its only power.");
      return;
    }
    if (this.batteryFitted === !!fitted) return;
    this.batteryFitted = !!fitted;
    if (this.batteryFitted) this._log("battery connected.");
    else if (this.powered()) this._log("battery out — USB is carrying the board.");
    this._recompute(this.batteryFitted ? "battery-in" : "battery-out");
  }

  setSwitch(on) {
    if (this.switchOn === !!on) return;
    this.switchOn = !!on;
    if (!this.switchOn && this.usb) {
      this._log(
        "switch OFF — but it only gates the battery path; on USB the board stays up."
      );
    }
    this._recompute(this.switchOn ? "switch-on" : "switch-off");
  }

  // ── The buttons ───────────────────────────────────────────────────────
  setBootHeld(held) {
    if (this.bootHeld === !!held) return;
    this.bootHeld = !!held;
    if (this.bootHeld && this.mode === "run") this._log(bootHeldLine(this.profile));
  }

  pressReset() {
    if (!this.powered()) {
      this._log("RESET pressed — no power, nothing to reset.");
      return;
    }
    const kind = this.bootHeld ? "download" : "reset";
    this.mode = this.bootHeld ? "download" : "run";
    if (kind === "download") {
      const strap = this.profile.rom?.boot_strap;
      this._log(`RESET with BOOT${strap ? ` (${strap})` : ""} low — the ROM boots to download mode instead of the app.`);
    } else {
      this._log("RESET — RAM clears; NVS flash keeps everything it learned.");
    }
    this.cb.onReset?.(kind);
  }

  /** The firmware's own ESP.restart() — mode bookkeeping only. */
  firmwareRestart() {
    if (this.mode === "run" && this.bootHeld) {
      // Even a software reset re-samples the straps.
      this.mode = "download";
      this.cb.onReset?.("download");
    }
  }

  // ── The battery model ─────────────────────────────────────────────────
  tick(dtMs) {
    if (!this.batteryFitted) return;
    const dt = (dtMs / 1000) * this.rate;
    if (this.usb) {
      // The charger runs whenever USB is present — the switch gates the
      // battery's OUTPUT, not its charging.
      if (this.soc < 100) {
        this.soc = Math.min(100, this.soc + CHARGE_PCT_PER_SEC * dt);
        if (this.soc >= 100) this._log("battery full — the charger stops.");
      }
    } else if (this.switchOn && this.soc > 0 && this.mode !== "off") {
      this.soc = Math.max(0, this.soc - DRAIN_PCT_PER_SEC * dt);
      if (this.soc <= 0) {
        this._log("Brownout detector was triggered — the battery is empty.");
        this._recompute("battery-empty");
      }
    }
  }

  // ── The lights ────────────────────────────────────────────────────────
  // Each LED's driver names the wire it hangs off — and none of those
  // wires reach a GPIO the firmware could drive:
  //   rail    → lit whenever the 3.3 V rail is up (either source)
  //   charger → the charge chip's status pins (CHG while filling,
  //             DONE when full; CHG hunts/flickers with no cell fitted
  //             where the board's own docs say so)
  //   gpio    → the one honest exception: firmware COULD drive it, and
  //             this firmware deliberately leaves it dark
  /** @returns {Object} led id → "on" | "off" | "flicker" */
  leds() {
    const out = {};
    for (const led of this.profile.leds || []) {
      out[led.id] = this._ledState(led);
    }
    return out;
  }

  _ledState(led) {
    switch (led.driver) {
      case "rail":
        return this.powered() ? "on" : "off";
      case "charger": {
        if (!this.usb) return "off"; // the charger only runs on USB
        if (led.id === "done")
          return this.batteryFitted && this.soc >= 100 ? "on" : "off";
        // charge-in-progress LED
        if (!this.batteryFitted)
          return led.flicker_without_battery ? "flicker" : "off";
        return this.soc < 100 ? "on" : "off";
      }
      case "gpio":
        return "off"; // drivable in principle; this firmware never does
    }
    return "off";
  }

  // ── internals ─────────────────────────────────────────────────────────
  _recompute(cause) {
    const wasOff = this.mode === "off";
    if (!this.powered()) {
      if (!wasOff) {
        this.mode = "off";
        this.cb.onPower?.(false, cause);
      }
      return;
    }
    if (wasOff) {
      // Power arriving re-samples the straps, like any cold boot.
      this.mode = this.bootHeld ? "download" : "run";
      this.cb.onPower?.(true, cause);
    }
  }

  _log(line) {
    this.cb.onLog?.(line);
  }
}
