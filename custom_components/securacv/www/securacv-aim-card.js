/*
 * securacv-aim-card.js
 * ---------------------------------------------------------------------------
 * A Lovelace card for aiming a SecuraCV Canary Vision device: a boxes-only
 * live view of what the Grove Vision AI V2 module is detecting — bounding-box
 * wireframe, confidence, and the voxel grid the firmware coarsens into —
 * WITHOUT ever rendering pixels. The firmware streams coordinates and scores
 * on `securacv/<device_id>/aim` while its "Aim assist" switch is on (off by
 * default, 10-minute auto-off); this card subscribes to that topic over HA's
 * MQTT websocket API and draws the frame.
 *
 * Why boxes-only: SenseCraft's preview streams raw JPEG frames to a laptop
 * plugged into the module's own USB port — fine for the one-time bench setup,
 * but it pauses I2C events to the host and needs physical access. This card
 * works in situ over the device's normal (local) MQTT path and preserves the
 * product's no-pixels posture: the firmware never exports frames, so there is
 * nothing here to leak.
 *
 * Design notes, in step with securacv-timeline-card.js:
 *   - No build step, no dependencies. Importable under Node so the pure
 *     data-shaping helpers are unit-testable with `node --test`; the custom
 *     element registers only when a browser `customElements` registry exists.
 *   - The MQTT websocket subscription (`mqtt/subscribe`) requires an HA admin
 *     user — the same permission HA's own "listen to a topic" dev tool needs.
 *     Non-admin users get an honest explanation instead of a broken canvas.
 *
 * Card config:
 *   type: custom:securacv-aim-card
 *   device_id: canary_vision_001       # required — the firmware device id
 *   prefix: securacv                   # optional — MQTT base prefix
 *   switch_entity: switch.xxx          # optional — the Aim assist switch;
 *                                      #   auto-discovered when omitted
 *   title: "Aim camera"                # optional
 */
(function () {
  "use strict";

  // How long after the last aim payload the view is considered stale. The
  // firmware publishes at ~5 Hz with a person in frame and 1 Hz idle, so 5 s
  // of silence means the switch is off, the device is offline, or aim
  // auto-off kicked in.
  const STALE_AFTER_MS = 5000;

  /**
   * Normalize one aim payload (parsed JSON) into render-ready geometry, or
   * null when the payload is not a usable aim frame. Coordinates mirror the
   * firmware's convention (vision_mgr: bbox x,y = top-left corner in a
   * fw × fh frame; voxel row/col from the box centre).
   */
  function normalizeAimPayload(data) {
    if (!data || typeof data !== "object" || Array.isArray(data)) return null;
    const fw = Number(data.fw) > 0 ? Number(data.fw) : 240;
    const fh = Number(data.fh) > 0 ? Number(data.fh) : 240;
    const rows = Number(data.rows) > 0 ? Number(data.rows) : 3;
    const cols = Number(data.cols) > 0 ? Number(data.cols) : 3;
    const present = data.present === true || data.present === "true";
    const out = {
      present,
      frameW: fw,
      frameH: fh,
      rows,
      cols,
      score: Number.isFinite(Number(data.score)) ? Number(data.score) : 0,
      voxelR: Number.isFinite(Number(data.vr)) ? Number(data.vr) : -1,
      voxelC: Number.isFinite(Number(data.vc)) ? Number(data.vc) : -1,
      box: null,
    };
    if (present) {
      const x = Number(data.x), y = Number(data.y);
      const w = Number(data.w), h = Number(data.h);
      if ([x, y, w, h].every(Number.isFinite) && w > 0 && h > 0) {
        out.box = { x, y, w, h };
      }
    }
    return out;
  }

  /**
   * Map a frame-space box to canvas-space, clamped so a box that pokes past
   * the frame edge (models do this near boundaries) still draws sanely.
   * Returns {x, y, w, h} in canvas pixels, or null without a box.
   */
  function boxToCanvasRect(frame, canvasW, canvasH) {
    if (!frame || !frame.box) return null;
    const sx = canvasW / frame.frameW;
    const sy = canvasH / frame.frameH;
    let x = frame.box.x * sx;
    let y = frame.box.y * sy;
    let w = frame.box.w * sx;
    let h = frame.box.h * sy;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > canvasW) w = canvasW - x;
    if (y + h > canvasH) h = canvasH - y;
    if (w <= 0 || h <= 0) return null;
    return { x, y, w, h };
  }

  /**
   * Pick the aim-assist switch entity for a device. Priority: explicit
   * config → an entity whose id ends in `_aim_assist` and contains the
   * device id (HA slugifies "SecuraCV Canary Vision <id>" + "Aim assist"
   * into e.g. switch.securacv_canary_vision_<id>_aim_assist).
   */
  function discoverSwitchEntity(states, deviceId, configured) {
    if (configured) return configured;
    if (!states || !deviceId) return null;
    const slug = String(deviceId).toLowerCase().replace(/[^a-z0-9]+/g, "_");
    const candidates = Object.keys(states).filter(
      (id) =>
        id.startsWith("switch.") &&
        id.endsWith("_aim_assist") &&
        id.includes(slug)
    );
    return candidates.length ? candidates.sort()[0] : null;
  }

  /** Human status line for the current card state. */
  function statusLine(state) {
    if (!state.deviceId) return "Set device_id in the card config.";
    if (state.subscribeError === "not-admin") {
      return "Live view needs an HA admin user (MQTT websocket access).";
    }
    if (state.subscribeError) return "MQTT subscription failed — see logs.";
    if (!state.switchOn) return "Aim assist is off — flip the switch to stream boxes.";
    if (state.stale) return "Waiting for aim frames… (device offline or auto-off)";
    if (state.frame && state.frame.present) {
      return `Person detected — score ${state.frame.score}%`;
    }
    return "Streaming — no person in frame.";
  }

  // --- Node export surface (pure helpers only; no DOM) ----------------------
  const helpers = {
    STALE_AFTER_MS,
    normalizeAimPayload,
    boxToCanvasRect,
    discoverSwitchEntity,
    statusLine,
  };
  if (typeof module !== "undefined" && module.exports) {
    module.exports = helpers;
  }

  // --- Browser custom element ----------------------------------------------
  if (typeof HTMLElement === "undefined" || typeof customElements === "undefined") {
    return; // Node/test context: stop here, the helpers above are enough.
  }

  const CARD_STYLE = `
    :host { display: block; }
    ha-card { padding: 16px; }
    .header { display: flex; align-items: center; justify-content: space-between; gap: 8px; }
    .title { font-size: 1.1rem; font-weight: 600; color: var(--primary-text-color); }
    .status { margin-top: 8px; font-size: 0.85rem; color: var(--secondary-text-color); }
    .stage { margin-top: 12px; position: relative; width: 100%; }
    canvas { display: block; width: 100%; height: auto; border-radius: 8px;
             background: var(--secondary-background-color, #1c1c1c); }
    button.toggle { cursor: pointer; border: none; border-radius: 14px;
             padding: 6px 14px; font-size: 0.85rem; font-weight: 600;
             background: var(--primary-color, #03a9f4); color: var(--text-primary-color, #fff); }
    button.toggle.on { background: var(--error-color, #e53935); }
    button.toggle[disabled] { opacity: 0.5; cursor: default; }
  `;

  class SecuraCVAimCard extends HTMLElement {
    constructor() {
      super();
      this.attachShadow({ mode: "open" });
      this._config = {};
      this._hass = null;
      this._frame = null;
      this._lastFrameAt = 0;
      this._unsub = null;
      this._subscribedTopic = null;
      this._subscribingTopic = null;
      this._subscribeError = null;
      this._staleTimer = null;
      this._built = false;
    }

    setConfig(config) {
      if (!config || !config.device_id) {
        throw new Error("securacv-aim-card: device_id is required");
      }
      this._config = Object.assign({ prefix: "securacv", title: "Aim camera" }, config);
      this._subscribedTopic = null; // force re-subscribe on reconfigure
    }

    getCardSize() {
      return 5;
    }

    set hass(hass) {
      this._hass = hass;
      this._maybeSubscribe();
      this._render();
    }

    disconnectedCallback() {
      this._teardownSubscription();
      if (this._staleTimer) {
        clearInterval(this._staleTimer);
        this._staleTimer = null;
      }
    }

    connectedCallback() {
      // Re-arm after Lovelace re-attaches the element (tab switches).
      if (!this._staleTimer) {
        this._staleTimer = setInterval(() => this._render(), 1000);
      }
      this._maybeSubscribe();
    }

    _teardownSubscription() {
      if (this._unsub) {
        try {
          this._unsub();
        } catch (_e) {
          /* connection already gone */
        }
        this._unsub = null;
      }
      this._subscribedTopic = null;
    }

    async _maybeSubscribe() {
      if (!this._hass || !this._config.device_id) return;
      const topic = `${this._config.prefix}/${this._config.device_id}/aim`;
      if (this._subscribedTopic === topic || this._subscribingTopic === topic) return;
      // Track the in-flight topic and re-check it after the await: a
      // reconfigure or disconnect while the subscription is in flight must
      // not commit a stale subscription (wrong topic) or leak the websocket
      // subscription of a detached element.
      this._subscribingTopic = topic;
      this._teardownSubscription();
      try {
        // Same websocket command HA's dev-tools "listen to a topic" uses.
        // Requires an admin user; non-admins get a clear message instead.
        const unsub = await this._hass.connection.subscribeMessage(
          (msg) => this._onAimMessage(msg),
          { type: "mqtt/subscribe", topic }
        );
        if (!this.isConnected || this._subscribingTopic !== topic) {
          try {
            unsub();
          } catch (_e) {
            /* connection already gone */
          }
          return;
        }
        this._unsub = unsub;
        this._subscribedTopic = topic;
        this._subscribeError = null;
      } catch (err) {
        if (this._subscribingTopic === topic) {
          this._subscribeError =
            err && err.code === "unauthorized" ? "not-admin" : String(err && err.message || err);
        }
      } finally {
        if (this._subscribingTopic === topic) {
          this._subscribingTopic = null;
        }
      }
      this._render();
    }

    _onAimMessage(msg) {
      let data = null;
      try {
        data = JSON.parse(msg.payload);
      } catch (_e) {
        return;
      }
      const frame = normalizeAimPayload(data);
      if (!frame) return;
      this._frame = frame;
      this._lastFrameAt = Date.now();
      this._render();
    }

    _switchEntityId() {
      return discoverSwitchEntity(
        this._hass ? this._hass.states : null,
        this._config.device_id,
        this._config.switch_entity
      );
    }

    _switchOn() {
      const id = this._switchEntityId();
      if (!id || !this._hass) return false;
      const st = this._hass.states[id];
      return !!st && st.state === "on";
    }

    _toggleSwitch() {
      const id = this._switchEntityId();
      if (!id || !this._hass) return;
      const service = this._switchOn() ? "turn_off" : "turn_on";
      this._hass.callService("switch", service, { entity_id: id });
    }

    _render() {
      if (!this.shadowRoot) return;
      const switchId = this._switchEntityId();
      const switchOn = this._switchOn();
      const stale = Date.now() - this._lastFrameAt > STALE_AFTER_MS;
      const status = statusLine({
        deviceId: this._config.device_id,
        subscribeError: this._subscribeError,
        switchOn,
        stale,
        frame: stale ? null : this._frame,
      });

      if (!this._built) {
        this.shadowRoot.innerHTML = `
          <style>${CARD_STYLE}</style>
          <ha-card>
            <div class="header">
              <span class="title"></span>
              <button class="toggle" type="button"></button>
            </div>
            <div class="stage"><canvas width="480" height="480"></canvas></div>
            <div class="status"></div>
          </ha-card>`;
        this._built = true;
        this.shadowRoot
          .querySelector("button.toggle")
          .addEventListener("click", () => this._toggleSwitch());
      }

      this.shadowRoot.querySelector(".title").textContent = this._config.title;
      const btn = this.shadowRoot.querySelector("button.toggle");
      btn.textContent = switchOn ? "Stop aiming" : "Start aiming";
      btn.classList.toggle("on", switchOn);
      btn.disabled = !switchId;
      btn.title = switchId || "Aim assist switch entity not found";
      this.shadowRoot.querySelector(".status").textContent = status;

      this._draw(stale ? null : this._frame);
    }

    _draw(frame) {
      const canvas = this.shadowRoot.querySelector("canvas");
      if (!canvas) return;
      const ctx = canvas.getContext("2d");
      const W = canvas.width;
      const H = canvas.height;
      const css = getComputedStyle(this);
      const gridColor = css.getPropertyValue("--divider-color").trim() || "#444";
      const boxColor = css.getPropertyValue("--primary-color").trim() || "#03a9f4";
      const hotColor = css.getPropertyValue("--warning-color").trim() || "#ff9800";

      ctx.clearRect(0, 0, W, H);

      const rows = frame ? frame.rows : 3;
      const cols = frame ? frame.cols : 3;

      // Voxel cell highlight (the coarse claim the firmware actually emits).
      // globalAlpha instead of hex-suffix alpha: getComputedStyle resolves
      // theme colors to rgb(...)/rgba(...), where "+ '33'" is invalid CSS.
      if (frame && frame.present && frame.voxelR >= 0 && frame.voxelC >= 0) {
        ctx.save();
        ctx.fillStyle = hotColor;
        ctx.globalAlpha = 0.2;
        ctx.fillRect(
          (frame.voxelC * W) / cols,
          (frame.voxelR * H) / rows,
          W / cols,
          H / rows
        );
        ctx.restore();
      }

      // Voxel grid lines.
      ctx.strokeStyle = gridColor;
      ctx.lineWidth = 1;
      for (let c = 1; c < cols; c++) {
        ctx.beginPath();
        ctx.moveTo((c * W) / cols, 0);
        ctx.lineTo((c * W) / cols, H);
        ctx.stroke();
      }
      for (let r = 1; r < rows; r++) {
        ctx.beginPath();
        ctx.moveTo(0, (r * H) / rows);
        ctx.lineTo(W, (r * H) / rows);
        ctx.stroke();
      }

      // Bounding-box wireframe + score.
      const rect = frame ? boxToCanvasRect(frame, W, H) : null;
      if (rect) {
        ctx.strokeStyle = boxColor;
        ctx.lineWidth = 3;
        ctx.strokeRect(rect.x, rect.y, rect.w, rect.h);
        ctx.fillStyle = boxColor;
        ctx.font = "16px sans-serif";
        const label = `${frame.score}%`;
        const ly = rect.y > 20 ? rect.y - 6 : rect.y + rect.h + 18;
        ctx.fillText(label, rect.x + 2, ly);
      }
    }
  }

  customElements.define("securacv-aim-card", SecuraCVAimCard);
  window.customCards = window.customCards || [];
  window.customCards.push({
    type: "securacv-aim-card",
    name: "SecuraCV Aim Camera",
    description:
      "Boxes-only live aiming view for Canary Vision — bounding box + voxel grid, never pixels.",
  });
})();
