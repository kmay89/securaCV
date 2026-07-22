// Track a two-port Canary Vision bring-up so the flasher can insist on BOTH
// flashes and never let you walk away half-done. A Vision is two chips on two
// ports — the ESP32 (Vision firmware) and the Grove Vision AI V2 / WE2 camera
// module (its person-detection model) — flashed in two separate connect cycles.
// This remembers which of the two you've done so the done screen can say
// "1 of 2 — now the other port" and celebrate only when both are in.
//
// Scope is this bring-up on this tab: held in sessionStorage so an accidental
// reload between ports doesn't lose your place, and gone when the tab closes.
// It holds NO secrets — just two booleans — so there's no disk-privacy trade
// (unlike wifi-memory's opt-in persist tier). Degrades to in-memory when
// sessionStorage is absent (Node import graph) or blocked (private mode).
//
// Starting a fresh pair is automatic in two ways, so a stale flag can never
// combine with an unrelated board into a false "both done":
//   · once both ports are done, the next flash begins a new pair (you're onto
//     the next board), so a batch just works; and
//   · an INCOMPLETE pair you walked away from goes stale after one bring-up
//     window (STALE_MS) and reads as empty — so half-flashing a Vision, then
//     much later flashing some other kit's other half in the same tab, does NOT
//     read as a completed pair. The UI's explicit "set up another board" path
//     also resets immediately (flash.js), for when you don't want to wait.
//
// Pure + storage/clock-injectable so the behaviour is proven in host tests
// (tests/vision_session.test.js) without a real sessionStorage or wall clock.

const STORE_KEY = "scv-vision";
// One bring-up: comfortably longer than flashing both ports (find the module,
// plug into the right port, burn), shorter than coming back for something else.
const STALE_MS = 30 * 60 * 1000;
const defaultNow = () => Date.now();

/**
 * @param {Storage|null} storage  sessionStorage-shaped backend, or null when it
 *   is absent/blocked — then only the in-memory tier works and nothing throws.
 * @param {() => number} [now]    clock (ms), injectable for host tests.
 */
export function makeVisionSession(storage, now = defaultNow) {
  let mem = { esp32: false, we2: false, t: 0 }; // in-memory mirror / fallback

  const readRaw = () => {
    try {
      const raw = storage && storage.getItem(STORE_KEY);
      if (!raw) return { ...mem };
      const o = JSON.parse(raw);
      return { esp32: !!(o && o.esp32), we2: !!(o && o.we2), t: Number(o && o.t) || 0 };
    } catch { return { ...mem }; }
  };
  const write = (p) => {
    mem = { esp32: !!p.esp32, we2: !!p.we2, t: Number(p.t) || 0 };
    try { if (storage) storage.setItem(STORE_KEY, JSON.stringify(mem)); } catch { /* private mode / absent */ }
  };

  /**
   * The two flags flashed so far this pair — { esp32, we2 }. A half-done pair
   * that's gone stale (older than one bring-up) reads as empty, so it can't
   * combine with a later unrelated flash into a false completion. A COMPLETE
   * pair never expires here — it's shown done, then the next markDone opens a
   * fresh pair.
   */
  const parts = () => {
    const p = readRaw();
    const half = (p.esp32 || p.we2) && !(p.esp32 && p.we2);
    if (half && now() - p.t > STALE_MS) return { esp32: false, we2: false };
    return { esp32: p.esp32, we2: p.we2 };
  };

  /**
   * Record that `part` ("esp32" | "we2") is now flashed. If the previous pair
   * was already complete (or a stale half — dropped by parts()), this starts a
   * fresh pair so the count can't stay stuck at "2 of 2". Unknown parts ignored.
   */
  const markDone = (part) => {
    if (part !== "esp32" && part !== "we2") return parts();
    const cur = parts();
    const base = cur.esp32 && cur.we2 ? { esp32: false, we2: false } : cur;
    base[part] = true;
    base.t = now();
    write(base);
    return { esp32: base.esp32, we2: base.we2 };
  };

  /** Forget the current pair (start over). */
  const reset = () => { write({ esp32: false, we2: false, t: 0 }); };

  return { parts, markDone, reset };
}

// Default singleton over the browser's sessionStorage (absent/blocked → in-memory
// tier only, never throws).
const browserSession = (() => { try { return globalThis.sessionStorage || null; } catch { return null; } })();
export const visionSession = makeVisionSession(browserSession);
