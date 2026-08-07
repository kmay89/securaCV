// derive.mjs — a Canary's name, derived from its key instead of rolled.
//
// THE PROBLEM THIS FIXES
//   The Hatchery mints a lovely certificate at first flash — a name, a house,
//   an Nth-of-its-name lineage — and then keeps it in the flasher's own
//   preferences. The device never learns its name. Nothing syncs it. So the
//   iPhone app, holding the same Canary in its hand, had no way to show the
//   certificate at all, and any name it displayed would have been a second,
//   different name for the same bird. A certificate that lives in one app's
//   local storage is not a certificate; it is a nickname.
//
// THE FIX, AND WHY IT CANNOT ROT
//   Derive the whole certificate from the one thing the device already is:
//   its Ed25519 public-key fingerprint. The name becomes a rendering of the
//   key — the same input on any surface, at any time, forever. Nothing is
//   stored, nothing is synced, and there is nothing to drift, because every
//   client computes it rather than remembering it. An already-flashed Canary
//   gets its certificate the moment a client can see its key; a factory reset
//   that mints a NEW key is honestly a new bird, and says so.
//
// THE RNG
//   Deliberately NOT a cryptographic one. The fingerprint is already a
//   SHA-256 digest, so this only has to spread it fairly and identically in
//   two languages. FNV-1a seeds an xorshift32 stream: eight lines, no crypto
//   import, synchronous in a browser, and trivially portable to Swift (see
//   ios/Shared/BirthCertificate.swift, which is pinned against the vectors
//   this module generates).
//
// COMPATIBILITY
//   A board with no key yet — a blank chip mid-flash — has nothing to derive
//   from, so `mintCertificate` in hatchery.js keeps its random path for that
//   case. The moment a device has an identity, the derived name wins, and the
//   re-roll button retires with it: you cannot re-roll a key.

/** FNV-1a (32-bit) over a string. The seed, not the randomness. */
export function fnv1a(str) {
  let h = 0x811c9dc5;
  for (let i = 0; i < str.length; i++) {
    h ^= str.charCodeAt(i) & 0xff;
    // h *= 16777619, in 32-bit lanes so JS and Swift agree exactly.
    h = (h + ((h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24))) >>> 0;
  }
  return h >>> 0;
}

/**
 * xorshift32, as a [0,1) generator. Seeded from the fingerprint; a zero seed
 * is nudged because xorshift32 has a fixed point there.
 * @param {string} seedText
 * @returns {() => number}
 */
export function seededRng(seedText) {
  let s = fnv1a(String(seedText)) || 0x9e3779b9;
  return () => {
    s ^= (s << 13) >>> 0; s >>>= 0;
    s ^= s >>> 17;
    s ^= (s << 5) >>> 0; s >>>= 0;
    return (s >>> 0) / 0x100000000;
  };
}

/**
 * The certificate for a device that has an identity. Same field order and the
 * same order of RNG consumption as the random mint it replaces (base → title?
 * → house → title → motto), so a reader comparing the two sees one algorithm
 * with two seeds rather than two algorithms.
 *
 * `lineage` is derived too: with no session history to count, "Nth of its
 * name" comes from the key as well, which is the honest version — the number
 * was never a fact about the world, only about the order you happened to
 * flash things in.
 *
 * @param {object} hatch          parsed hatch.json
 * @param {object} [opts]
 * @param {string} opts.fingerprint  the device's hex pubkey fingerprint (its identity)
 * @param {{name?:string}} [opts.product]
 * @param {string} [opts.deviceId]   the provisioned slug → the Ring ID, if there is one
 * @returns {object|null}
 */
export function deriveCertificate(hatch, opts = {}) {
  const h = hatch;
  const fp = String(opts.fingerprint || "").trim().toLowerCase();
  if (!h || !Array.isArray(h.first) || !h.first.length || !fp) return null;

  const rng = seededRng(fp);
  const pick = (a) => a[Math.floor(rng() * a.length)];

  const base = pick(h.first);
  const withTitle = Array.isArray(h.titles) && h.titles.length &&
    rng() < (typeof h.title_chance === "number" ? h.title_chance : 0.6);
  const house = (Array.isArray(h.house) && h.house.length) ? pick(h.house) : "";
  const ordinals = Array.isArray(h.ordinals) ? h.ordinals : [];
  // Ordinals are 1-based in the spec (index 0 is the empty string), so the
  // pick is over the real entries only.
  const nth = ordinals.length > 1 ? 1 + Math.floor(rng() * (ordinals.length - 1)) : 1;
  const ordinal = ordinals[nth] || ("the " + nth + "th");
  const title = withTitle ? pick(h.titles) + " " : "";
  const motto = (Array.isArray(h.mottoes) && h.mottoes.length) ? pick(h.mottoes) : "";

  return {
    base,
    name: title + base + (house ? " " + house : ""),
    species: (opts.product && opts.product.name) || "Canary",
    lineage: ordinal + " of its name" + (house ? ", " + house.replace(/^the /, "") : ""),
    // The ring id is the device's real slug when it has one; otherwise the
    // fingerprint IS the identity, so show it rather than inventing a code.
    ringId: String(opts.deviceId || "").trim() || fp.toUpperCase(),
    motto,
    craft: (h.certificate && h.certificate.craft) || "",
    /** How this certificate came to be — surfaces refuse to claim a birthday
     *  they don't know (see the iOS card: "paired on", never "born on"). */
    derived: true,
  };
}
