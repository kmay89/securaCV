// The Hatchery — the whimsical name + birth certificate a Canary earns when it
// hatches (first successful flash). The name is WHIMSY ONLY: the device's
// functional id stays its stable provisioned slug; this is a display/certificate
// layer, the same one the native app mints.
//
// Both the browser Lab and the native app draw from ONE committed spec
// (canary-local/devices/hatch.json — the native app embeds the very same file at
// build time via build.rs), and the tiny assembly below mirrors the native app's
// mintCertificate (desktop/src/app.js) call-for-call, so the birthing moment is
// identical on both surfaces and stays in sync when the spec changes. The set of
// spec fields each side reads is drift-gated in tests/desktop_parity.test.js.
//
// Pure + RNG-injectable, so it's host-tested and deterministic (tests/
// hatchery.test.js). The default RNG is Math.random; tests pass a seeded one.
//
// UPDATE — a name that can't drift: when the caller knows the device's
// fingerprint, the certificate is DERIVED from it rather than rolled, so
// every surface computes one name for one bird and nothing has to be stored
// or synced. See tools/hatchery/derive.mjs for the argument and the
// algorithm; the random assembly below is now only for a board that has no
// key yet.

import { deriveCertificate } from "../tools/hatchery/derive.mjs";

// A ring code — the certificate's id when a board wasn't provisioned with a real
// device id. PREFIX-XXX-XXX from a 24-bit value.
export function genRing(prefix, rng = Math.random) {
  const hex = Math.floor(rng() * 0x1000000).toString(16).padStart(6, "0").slice(0, 6).toUpperCase();
  return `${prefix}-${hex.slice(0, 3)}-${hex.slice(3)}`;
}

// Pick a base name not yet used this session (and never `avoid`); once the pool
// is spent, reuse is allowed rather than failing.
export function pickFreshBase(first, usedBases, avoid, rng = Math.random) {
  const spent = new Set(usedBases || []);
  if (avoid) spent.add(avoid);
  const open = first.filter((b) => !spent.has(b));
  const pool = open.length ? open : first;
  return pool[Math.floor(rng() * pool.length)];
}

/**
 * Mint a certificate from the hatch spec. Mirrors the native app's assembly,
 * including the order it consumes randomness (base → title? → house → ring →
 * title → motto), so the same spec + RNG yields the same bird.
 * @param {object} hatch  the parsed hatch.json spec
 * @param {object} [opts]
 * @param {{name?:string}} [opts.product]  for the species line
 * @param {string} [opts.deviceId]  the provisioned slug → the Ring ID (else generated)
 * @param {Array<{base?:string}>} [opts.fleet]  prior hatches, for the "Nth of its name" ordinal
 * @param {Iterable<string>} [opts.usedBases]  base names already spent this session
 * @param {string} [opts.avoidBase]  a base to never pick (re-roll)
 * @param {() => number} [opts.rng]  [0,1) RNG (injectable for tests)
 * @param {number} [opts.now]  timestamp to stamp (caller passes Date.now())
 * @returns {object|null}  the certificate, or null if the spec has no names
 */
export function mintCertificate(hatch, opts = {}) {
  // A device with a KEY names itself: the certificate is derived from its
  // fingerprint, so the phone, this Lab and the Mac Flasher all render the
  // same bird without syncing anything (tools/hatchery/derive.mjs). The
  // random path below survives for the only case that has no identity yet —
  // a blank chip mid-flash. Re-roll retires with it: you can't re-roll a key.
  if (opts && opts.fingerprint) {
    const derived = deriveCertificate(hatch, opts);
    if (derived) return { ...derived, ts: opts.now || 0 };
  }
  const h = hatch;
  if (!h || !Array.isArray(h.first) || !h.first.length) return null;
  const rng = opts.rng || Math.random;
  const pick = (a) => a[Math.floor(rng() * a.length)];
  const fleet = opts.fleet || [];

  const base = pickFreshBase(h.first, opts.usedBases, opts.avoidBase, rng);
  const withTitle = Array.isArray(h.titles) && h.titles.length &&
    rng() < (typeof h.title_chance === "number" ? h.title_chance : 0.6);
  const house = (Array.isArray(h.house) && h.house.length) ? pick(h.house) : "";
  const nth = fleet.filter((c) => c && c.base === base).length + 1;
  const ordinals = Array.isArray(h.ordinals) ? h.ordinals : [];
  const ordinal = ordinals[nth] || ("the " + nth + "th");
  const deviceId = String(opts.deviceId || "").trim();
  const ringId = deviceId || genRing(h.ring_prefix || "CNRY", rng);

  return {
    base,
    name: (withTitle ? pick(h.titles) + " " : "") + base + (house ? " " + house : ""),
    species: (opts.product && opts.product.name) || "Canary",
    lineage: ordinal + " of its name" + (house ? ", " + house.replace(/^the /, "") : ""),
    ringId,
    motto: (Array.isArray(h.mottoes) && h.mottoes.length) ? pick(h.mottoes) : "",
    craft: (h.certificate && h.certificate.craft) || "",
    ts: opts.now || 0,
  };
}
