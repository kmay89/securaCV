// canary-local/emulator/web/emu-shell.js — the bench around the glass.
//
// Wraps one wasm firmware module (built by ../build.sh from the REAL
// canary-display sources) in the things a bench provides: power, a
// network, sibling canaries, a serial monitor, a finger. The page talks
// to this class; this class talks to the firmware only through the same
// boundaries silicon would (framebuffer out, touch in, MQTT bytes in/out,
// NVS preseed).
//
// Simulated witnesses publish the same JSON the real fleet publishes
// (spellings straight from mqtt_mgr.cpp's dispatcher), and — where the
// browser supports WebCrypto Ed25519 — they carry REAL signatures over
// the same canonical string trust.cpp rebuilds, so the "Verified ✓"
// badge in the emulator is cryptographic truth end to end.
//
// Vanilla ES module, zero dependencies (repo convention).

const te = new TextEncoder();

function b64url(bytes) {
  let s = "";
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replaceAll("+", "-").replaceAll("/", "_").replaceAll("=", "");
}

function hex(bytes) {
  return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}
const hexEncode = hex;

async function sha256Hex(str) {
  const d = await crypto.subtle.digest("SHA-256", te.encode(str));
  return hex(new Uint8Array(d));
}

// POSIX TZ string for the visitor's current UTC offset (fixed-offset form;
// good for the emulator's "learned from the page" tz_auto). POSIX signs
// are inverted: UTC-7 (Denver summer) is "LT7" ... "LT-2" for UTC+2.
export function browserPosixTz() {
  const offMin = -new Date().getTimezoneOffset(); // +east of UTC
  const sign = offMin >= 0 ? "-" : "+";
  const abs = Math.abs(offMin);
  const hh = Math.floor(abs / 60);
  const mm = abs % 60;
  return `LT${sign}${hh}${mm ? ":" + String(mm).padStart(2, "0") : ""}`;
}

// ── A simulated witness: one sibling canary on the emulated LAN ─────────
// Publishes retained-style rows on the same topics and cadences the real
// firmware families use; keeps a hash chain and signs its head when the
// browser can. All of it is inspectable from the page's MQTT panel.
export class SimWitness {
  constructor({ id, deviceType, name, room, battery = null, rssi = -58 }) {
    this.id = id;
    this.deviceType = deviceType;
    this.name = name;
    this.room = room;
    this.battery = battery;
    this.rssi = rssi;
    this.chainLen = 40 + Math.floor(Math.random() * 900);
    this.online = true;
    this.tampered = false;
    this.key = null; // WebCrypto Ed25519 pair when available
    this.pubHex = null;
  }

  async ensureKeys() {
    if (this.key || this.pubHex) return;
    try {
      this.key = await crypto.subtle.generateKey("Ed25519", true, [
        "sign",
        "verify",
      ]);
      const raw = new Uint8Array(
        await crypto.subtle.exportKey("raw", this.key.publicKey)
      );
      this.pubHex = hex(raw);
    } catch {
      this.key = null; // browser without Ed25519 → witness publishes unsigned
      this.pubHex = null;
    }
  }

  topic(suffix) {
    return `securacv/${this.id}/${suffix}`;
  }

  statusPayload() {
    const p = {
      status: this.online ? "online" : "offline",
      device_type: this.deviceType,
      rssi: this.rssi,
    };
    if (this.battery != null) p.battery_soc = this.battery;
    return p;
  }

  healthPayload() {
    const p = {
      firmware_version: "2.2.0",
      battery: this.battery ?? -1,
      battery_present: this.battery != null,
      uptime_s: 86400 + Math.floor(Math.random() * 9999),
    };
    if (this.pubHex) p.public_key = this.pubHex;
    return p;
  }

  metaPayload() {
    return { name: this.name, room: this.room };
  }

  async chainPayload() {
    await this.ensureKeys();
    const latestHash = await sha256Hex(`${this.id}:${this.chainLen}:demo`);
    const fp = this.pubHex
      ? (await sha256Hex(this.pubHex)).slice(0, 16)
      : "0000000000000000";
    const body = {
      length: this.chainLen,
      latest_hash: latestHash,
      fp,
    };
    if (this.key) {
      // The exact canonical trust.cpp rebuilds before Ed25519::verify.
      const canonical = `securacv-canary-sig|v1|chain|${this.id}|${this.chainLen}|${latestHash}`;
      const sig = new Uint8Array(
        await crypto.subtle.sign("Ed25519", this.key.privateKey, te.encode(canonical))
      );
      body.sig = b64url(sig);
    }
    return body;
  }

  eventPayload(eventName, signed = true) {
    const p = { event: eventName };
    if (signed && this.key) p.signed = true;
    return p;
  }

  tamperPayload(on, kind = "enclosure_tamper") {
    return { state: on ? "on" : "off", kind };
  }
}

// ── The emulator proper ─────────────────────────────────────────────────
export class CanaryEmulator {
  /**
   * @param moduleFactory createCanaryEmu from a dist/*.js build
   * @param opts.canvas   the <canvas> that plays the glass (panel-native
   *                      size; the 3D card textures from it)
   * @param opts.onSerial (text) — serial monitor sink
   * @param opts.onMqtt   ({dir, topic, payload, retained}) — wire panel
   * @param opts.onNetEvent (kind, detail)
   * @param opts.onBacklight (levelFloat 0..1, nightProfile) — glass glow
   * @param opts.onTone   (freqHz) — chime voice (Web Audio outside)
   * @param opts.onReboot () — module needs a fresh boot (ESP.restart)
   */
  constructor(moduleFactory, opts) {
    this.factory = moduleFactory;
    this.opts = opts;
    this.canvas = opts.canvas;
    this.ctx = this.canvas.getContext("2d", { willReadFrequently: false });
    this.module = null;
    this.imageData = null;
    this.fb = { ptr: 0, w: 0, h: 0 };
    this.witnesses = [];
    this.retained = new Map(); // topic → payload (replayed on reconnect)
    this._hb = null;
    this.linkState = { wifi: true, broker: true };
    this.booted = false;
  }

  async start({ provisioned = true, firstMeeting = false, seed = null } = {}) {
    const shell = this;
    this.module = await this.factory({
      onSerial: (t) => shell.opts.onSerial?.(t),
      onFlush: (x, y, w, h) => shell._blit(),
      onDisplayReady: (w, h, round) => shell._displayReady(w, h, round),
      onBacklight: (level, duty13) => shell._backlight(level, duty13),
      onTone: (f) => shell.opts.onTone?.(f),
      onMqttPublish: (topic, payload, retained) => {
        shell.opts.onMqtt?.({ dir: "out", topic, payload, retained });
      },
      onMqttSubscribe: (topic) => shell._onSubscribe(topic),
      onMqttConnection: (up, willTopic, willMsg) =>
        shell.opts.onMqtt?.({
          dir: up ? "connect" : "lwt",
          topic: willTopic,
          payload: up ? "(session up)" : willMsg,
          retained: !up,
        }),
      onNetEvent: (kind, detail) => shell.opts.onNetEvent?.(kind, detail),
      onNvsWrite: (ns, key, hexVal) => shell.opts.onNvsWrite?.(ns, key, hexVal),
      onReboot: () => shell.opts.onReboot?.(),
    });

    const M = this.module;
    this.c = {
      power: M.cwrap("emu_power_on", null, []),
      touch: M.cwrap("emu_touch", null, ["number", "number", "number"]),
      fbPtr: M.cwrap("emu_fb_ptr", "number", []),
      fbW: M.cwrap("emu_fb_width", "number", []),
      fbH: M.cwrap("emu_fb_height", "number", []),
      flushCount: M.cwrap("emu_flush_count", "number", []),
      backlight: M.cwrap("emu_backlight_level", "number", []),
      nightDuty: M.cwrap("emu_backlight_night_duty", "number", []),
      setWifi: M.cwrap("emu_set_wifi", null, ["number", "number"]),
      setBroker: M.cwrap("emu_set_broker", null, ["number"]),
      setTz: M.cwrap("emu_set_tz", null, ["string"]),
      setReferral: M.cwrap("emu_set_broker_referral", null, ["string", "number"]),
      inject: M.cwrap("emu_mqtt_inject", null, ["string", "string", "number"]),
      mqttConnected: M.cwrap("emu_mqtt_connected", "number", []),
      timeScale: M.cwrap("emu_time_scale", null, ["number"]),
      timeStep: M.cwrap("emu_time_step_ms", null, ["number"]),
      epochOffset: M.cwrap("emu_epoch_offset", null, ["number"]),
      seed: M.cwrap("emu_seed", null, ["number"]),
      nvsPreseed: M.cwrap("emu_nvs_preseed_hex", null, [
        "string",
        "string",
        "string",
      ]),
      nvsReset: M.cwrap("emu_nvs_reset", null, []),
      chApply: M.cwrap("emu_apply_character", null, ["number"]),
      chCount: M.cwrap("emu_character_count", "number", []),
      chActive: M.cwrap("emu_character_active", "number", []),
      chAtRing: M.cwrap("emu_character_at_ring", "number", ["number"]),
      // Pointer-returns decoded page-side: under ASYNCIFY, cwrap's own
      // "string" return path can hand back a promise that never settles
      // while the firmware loop keeps re-suspending; a raw pointer is a
      // plain number (sync), and the table strings are static forever.
      chName: M.cwrap("emu_character_name", "number", ["number"]),
      chCaption: M.cwrap("emu_character_caption", "number", ["number"]),
    };

    if (seed != null) this.c.seed(seed >>> 0);
    this.c.setTz(browserPosixTz());

    // Stage the device's memory before power-on.
    if (provisioned) {
      this._nvsPut("securacv", "wifi_ssid", "HomeNet");
      this._nvsPut("securacv", "wifi_pass", "correct-horse");
      this._nvsPut("securacv", "mqtt_host", "hub.local");
      this._nvsPut("securacv", "mqtt_user", "fleet");
      this._nvsPut("securacv", "mqtt_pass", "fleet");
    }
    if (!firstMeeting) {
      this._nvsPut("scv-hello", "met", "\x01");
    }

    this._wireInput();
    this.c.power();
    this.booted = true;
  }

  _nvsPut(ns, key, str) {
    this.c.nvsPreseed(ns, key, hexEncode(te.encode(str)));
  }

  /** Replay a previously captured NVS image (see onNvsWrite) — this is
   *  how the device's memory survives an emulated reboot. */
  nvsRestore(image) {
    for (const [nsKey, hexVal] of image) {
      const slash = nsKey.indexOf("/");
      this.c.nvsPreseed(nsKey.slice(0, slash), nsKey.slice(slash + 1), hexVal);
    }
  }

  _displayReady(w, h, round) {
    this.fb.w = w;
    this.fb.h = h;
    this.round = !!round;
    this.canvas.width = w;
    this.canvas.height = h;
    this.imageData = this.ctx.createImageData(w, h);
    this.opts.onDisplayReady?.(w, h, this.round);
  }

  /** Take this instance off the bench: a replacement module owns the
   *  canvas now. The wasm keeps ticking harmlessly off-stage (a clean
   *  ASYNCIFY teardown isn't worth the ceremony for a demo bench), but
   *  it may no longer draw, glow, log, or speak. */
  retire() {
    this.dead = true;
    this.stopFleetHeartbeat();
    this.opts = {};
  }

  _blit() {
    if (this.dead) return;
    if (!this.imageData) return;
    const ptr = this.c.fbPtr();
    if (!ptr) return;
    const n = this.fb.w * this.fb.h * 4;
    // HEAPU8 view is refreshed each call (memory growth can detach it).
    const heap = new Uint8Array(this.module.HEAPU8.buffer, ptr, n);
    this.imageData.data.set(heap);
    this.ctx.putImageData(this.imageData, 0, 0);
    this.opts.onFrame?.();
  }

  _backlight(level, duty13) {
    if (this.dead) return;
    let glow;
    let night = false;
    if (duty13 >= 0) {
      night = true;
      glow = Math.min(1, duty13 / 8191);
      // The 13-bit night profile's lowest steps are intentionally faint;
      // lift them into something a monitor can show while keeping order.
      if (duty13 > 0) glow = 0.06 + glow * 0.4;
    } else {
      glow = Math.max(0, Math.min(1, level / 255));
    }
    this.opts.onBacklight?.(glow, night);
  }

  _onSubscribe(topic) {
    this.opts.onMqtt?.({ dir: "sub", topic, payload: "", retained: false });
    // Broker behavior: retained rows replay the moment the wildcard lands.
    for (const [t, payload] of this.retained) {
      if (topicMatches(topic, t)) this._deliver(t, payload);
    }
  }

  _deliver(topic, payload) {
    this.c.inject(topic, payload, payload.length);
    this.opts.onMqtt?.({ dir: "in", topic, payload, retained: false });
  }

  // ── Public: LAN controls ──────────────────────────────────────────────
  setWifi(up, rssi = -52) {
    this.linkState.wifi = up;
    this.c.setWifi(up ? 1 : 0, rssi);
  }
  setBroker(up) {
    this.linkState.broker = up;
    this.c.setBroker(up ? 1 : 0);
  }
  setReferral(host, port = 1883) {
    this.c.setReferral(host, port);
  }
  setTimeScale(x) {
    this.c.timeScale(x);
  }
  stepTime(ms) {
    this.c.timeStep(ms);
  }

  // ── Public: the Character ring, straight from the firmware table ──────
  // Same knob the on-glass picker turns (the setting persists through
  // emulated reboots via the debounced commit, like the real glass).
  // All three tolerate the boot window: during an emulated reboot the
  // replacement instance exists before start() finishes wiring this.c,
  // and a click landing in that gap must be a no-op, not a TypeError.
  applyCharacter(id) {
    this.c?.chApply(id);
  }
  // Value-returning calls are awaited: under ASYNCIFY a call that lands
  // while the firmware is suspended in its loop-sleep comes back as a
  // Promise, and a bench must read the dial, not the dial's IOU.
  async activeCharacter() {
    if (!this.c) return null;
    return await this.c.chActive();
  }
  async characterRing() {
    if (!this.c) return [];
    const out = [];
    const n = await this.c.chCount();
    for (let i = 0; i < n; i++) {
      const id = await this.c.chAtRing(i);
      const namePtr = await this.c.chName(id);
      const capPtr = await this.c.chCaption(id);
      out.push({
        id,
        name: namePtr ? this.module.UTF8ToString(namePtr) : "",
        caption: capPtr ? this.module.UTF8ToString(capPtr) : "",
      });
    }
    return out;
  }

  /** Stage the emulated wall clock at a given local hour (e.g. 10 to see
   *  the day face, 23 for quiet hours) without touching the visitor's
   *  machine. The firmware's own schedule does the rest. */
  setLocalHour(hour) {
    const now = new Date();
    const cur = now.getHours() + now.getMinutes() / 60;
    let deltaH = hour - cur;
    if (deltaH < -12) deltaH += 24;
    this.c.epochOffset(deltaH * 3600);
  }

  /** Publish as the emulated LAN's broker: retained rows persist and
   *  replay on the display's (re)subscription, exactly like Mosquitto. */
  publish(topic, payloadObj, { retained = true } = {}) {
    const payload =
      typeof payloadObj === "string" ? payloadObj : JSON.stringify(payloadObj);
    if (retained) this.retained.set(topic, payload);
    if (this.c.mqttConnected()) this._deliver(topic, payload);
  }

  // ── Public: fleet staging ─────────────────────────────────────────────
  async addWitness(w) {
    this.witnesses.push(w);
    await w.ensureKeys();
    this.publish(w.topic("status"), w.statusPayload());
    this.publish(w.topic("health"), w.healthPayload());
    this.publish(w.topic("meta"), w.metaPayload());
    this.publish(w.topic("chain"), await w.chainPayload());
  }

  /** Heartbeats: keep the staged fleet warm so staleness deadlines only
   *  trip when a scenario silences someone on purpose. */
  startFleetHeartbeat(periodMs = 20000) {
    this.stopFleetHeartbeat();
    this._hb = setInterval(async () => {
      for (const w of this.witnesses) {
        if (!w.online) continue;
        this.publish(w.topic("status"), w.statusPayload());
      }
    }, periodMs);
  }
  stopFleetHeartbeat() {
    if (this._hb) clearInterval(this._hb);
    this._hb = null;
  }

  async witnessEvent(w, name, { signed = true } = {}) {
    this.publish(w.topic("events"), w.eventPayload(name, signed), {
      retained: false,
    });
    // A real event advances the chain; republish the signed head.
    w.chainLen += 1;
    this.publish(w.topic("chain"), await w.chainPayload());
  }

  witnessTamper(w, on, kind) {
    w.tampered = on;
    this.publish(w.topic("tamper"), w.tamperPayload(on, kind));
  }

  witnessSilence(w) {
    w.online = false; // heartbeats stop; staleness ladder takes over
  }
  async witnessRevive(w) {
    w.online = true;
    this.publish(w.topic("status"), w.statusPayload());
  }

  householdAck(byDisplayId = "canary_watch_002") {
    this.publish(
      "securacv/fleet/ack",
      { at: Math.floor(Date.now() / 1000), by: byDisplayId },
      { retained: false }
    );
  }

  // ── Input ─────────────────────────────────────────────────────────────
  _wireInput() {
    const cv = this.canvas;
    const pos = (ev) => {
      const r = cv.getBoundingClientRect();
      const x = Math.round(((ev.clientX - r.left) / r.width) * this.fb.w);
      const y = Math.round(((ev.clientY - r.top) / r.height) * this.fb.h);
      return [Math.max(0, Math.min(this.fb.w - 1, x)),
              Math.max(0, Math.min(this.fb.h - 1, y))];
    };
    let down = false;
    cv.addEventListener("pointerdown", (ev) => {
      down = true;
      cv.setPointerCapture(ev.pointerId);
      const [x, y] = pos(ev);
      this.c.touch(1, x, y);
    });
    cv.addEventListener("pointermove", (ev) => {
      if (!down) return;
      const [x, y] = pos(ev);
      this.c.touch(1, x, y);
    });
    const up = (ev) => {
      if (!down) return;
      down = false;
      const [x, y] = pos(ev);
      this.c.touch(0, x, y);
    };
    cv.addEventListener("pointerup", up);
    cv.addEventListener("pointercancel", up);
  }

  // Synthetic gestures for guided tours ("watch what a long-press does").
  async tap(x, y) {
    this.c.touch(1, x, y);
    await sleep(90);
    this.c.touch(0, x, y);
  }
  async longPress(x, y, ms = 1100) {
    this.c.touch(1, x, y);
    await sleep(ms);
    this.c.touch(0, x, y);
  }
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

// MQTT single-level wildcard match (enough for securacv/+/suffix).
export function topicMatches(filter, topic) {
  const f = filter.split("/");
  const t = topic.split("/");
  for (let i = 0; i < f.length; i++) {
    if (f[i] === "#") return true;
    if (f[i] === "+") {
      if (t[i] === undefined) return false;
      continue;
    }
    if (f[i] !== t[i]) return false;
  }
  return f.length === t.length;
}

// The default demo household — three witnesses across the family's
// sensing modalities, named like a home, matching the registry copy.
export function demoFleet() {
  return [
    new SimWitness({
      id: "canary_vision_frontdoor",
      deviceType: "canary-vision",
      name: "Front Door",
      room: "Porch",
      battery: 84,
      rssi: -61,
    }),
    new SimWitness({
      id: "canary_sense_nursery",
      deviceType: "canary-sense",
      name: "Nursery",
      room: "Bedroom",
      battery: null,
      rssi: -47,
    }),
    new SimWitness({
      id: "canary_wap_garage",
      deviceType: "canary-wap",
      name: "Garage",
      room: "Garage",
      battery: 67,
      rssi: -72,
    }),
  ];
}
