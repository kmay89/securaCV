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
//     bench (the ROM is the one program we can't compile to wasm); its
//     text is verbatim what an ESP32-S3 prints. Everything after the
//     banner is the real firmware talking.
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

// The ESP32-S3 mask ROM's own voice, verbatim. `boot:` is the strapping
// sample: GPIO0 high → SPI_FAST_FLASH_BOOT (run the app), GPIO0 held low
// through reset → DOWNLOAD (sit waiting for a flasher, forever).
const ROM_HEAD = "ESP-ROM:esp32s3-20210327\nBuild:Mar 27 2021\n";
export function romBanner(kind) {
  switch (kind) {
    case "poweron":
    case "reset":
      return ROM_HEAD + "rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)\n";
    case "swreset": // ESP.restart() — the firmware asked for this one
      return ROM_HEAD + "rst:0x3 (RTC_SW_SYS_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)\n";
    case "download":
      return ROM_HEAD + "rst:0x1 (POWERON),boot:0x0 (DOWNLOAD(USB/UART0))\nwaiting for download\n";
  }
  return "";
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
    this.batteryFitted = !!this.profile.power?.battery;
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
    if (this.bootHeld && this.mode === "run") {
      this._log(
        "BOOT held — a strapping pin, sampled only at reset. Running firmware ignores it."
      );
    }
  }

  pressReset() {
    if (!this.powered()) {
      this._log("RESET pressed — no power, nothing to reset.");
      return;
    }
    const kind = this.bootHeld ? "download" : "reset";
    this.mode = this.bootHeld ? "download" : "run";
    if (kind === "download") {
      this._log("RESET with BOOT low — the ROM boots to download mode instead of the app.");
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
