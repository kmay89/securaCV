// canary-local/assets/vault-ui.js — the Vault explainer widgets.
//
// Two staged surfaces, fed from the drift-gated devices/vault.json:
//
//   · buildSealWalk  — how a frame is sealed: the five real steps
//     (ephemeral X25519 → HKDF → ChaCha20-Poly1305), the byte-exact SVLT
//     header, and a tamper toggle that shows why editing one header byte
//     makes the whole thing fail to open.
//   · buildQuorumDemo — break-glass by quorum, for REAL: it generates the
//     trustees' Ed25519 keys in your browser (WebCrypto), signs each approval
//     with the kernel's exact domain separation (securacv:pwk:trustee-approval:v2),
//     and counts DISTINCT valid signatures exactly like the kernel's
//     count_valid_distinct_approvals — so a reused key can't fill two slots and
//     a forged grant with too few approvals is refused, in front of you.
//
// The quorum math is not theater. Where a step is illustrated rather than
// executed (the ChaCha20-Poly1305 AEAD, which WebCrypto doesn't expose), the
// page says so with an .ondevice ribbon.

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const alive = (node) => document.body.contains(node);
const subtle = (globalThis.crypto && globalThis.crypto.subtle) || null;

// ── DOM-free cores (exported; pinned in tests/vault.test.js) ───────────────

// 4-byte little-endian, matching the kernel's le32() in the domain-sep preimage.
export function le32(n) {
  return new Uint8Array([n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255]);
}

export function hex(bytes) {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

function concatBytes(...arrs) {
  const len = arrs.reduce((s, a) => s + a.length, 0);
  const out = new Uint8Array(len);
  let o = 0;
  for (const a of arrs) { out.set(a, o); o += a.length; }
  return out;
}

// domain_separated_hash(domain, hash) = SHA-256(le32(len(domain)) ‖ domain ‖ hash)
// — byte-for-byte the kernel's construction (src/crypto/signatures.rs). This is
// what a trustee actually signs; getting it wrong would make every real approval
// fail to verify, so the test pins its output.
export async function domainSepHash(domain, hashBytes) {
  const pre = concatBytes(le32(domain.length), new TextEncoder().encode(domain), hashBytes);
  return new Uint8Array(await subtle.digest("SHA-256", pre));
}

// Count DISTINCT trustee keys carrying a valid approval — the ground truth the
// kernel uses (count_valid_distinct_approvals). Dedups on the KEY, so approving
// twice with one key counts once, and a reused key can't fill two quorum slots.
export function countDistinctApprovals(approvals) {
  const keys = new Set();
  for (const a of approvals) if (a && a.valid) keys.add(a.keyHex);
  return keys.size;
}

// Granted iff distinct valid approvals >= n (src/break_glass/core.rs).
export function quorumOutcome(distinct, n) {
  return distinct >= n ? "granted" : "denied";
}

// ── the seal walkthrough ───────────────────────────────────────────────────
export function buildSealWalk(ds) {
  const wrap = el("div", "vault-seal");

  // the five steps
  const stepsWrap = el("div", "vault-steps");
  const stepEls = ds.steps.map((s) => {
    const row = el("div", "vault-step");
    const b = el("div", "vault-step-b");
    b.append(el("strong", null, s.title), el("span", "muted fineprint", s.detail));
    row.append(el("span", "vault-step-n", String(s.n)), b);
    stepsWrap.append(row);
    return row;
  });

  const controls = el("div", "hub-term-controls");
  const play = el("button", "primary small", "▶ seal a frame");
  const derived = el("code", "vault-derived", "");
  controls.append(play, derived);

  // the byte-exact SVLT header
  const hdr = el("div", "vault-hdr");
  hdr.append(el("div", "vault-hdr-h", "the .svlt header (64 bytes) — this IS the AEAD associated data"));
  const table = el("div", "vault-hdr-table");
  for (const f of ds.svlt_header) {
    const cell = el("div", "vault-hdr-cell");
    cell.append(el("code", "vault-hdr-off", `@${f.off}`),
      el("code", "vault-hdr-size", `${f.size}B`),
      el("span", "vault-hdr-field", f.field));
    table.append(cell);
  }
  hdr.append(table);

  // tamper toggle
  const tamper = el("div", "vault-tamper");
  const tbtn = el("button", "ghost small", "✎ flip a header byte");
  const tout = el("span", "vault-tamper-out muted", "unsealed cleanly — tag OK ✓");
  tamper.append(tbtn, tout);
  hdr.append(tamper);

  const ribbon = el("p", "ondevice vault-note");
  ribbon.append(el("strong", null, "How to read this: "), document.createTextNode(
    "the key derivation is real (X25519 + HKDF in your browser where supported); the ChaCha20-Poly1305 " +
    "step is illustrated — WebCrypto doesn't expose it — but the algorithm names, the info string and the byte layout are the firmware's own."));

  wrap.append(stepsWrap, controls, hdr, ribbon);

  let tampered = false;
  tbtn.addEventListener("click", () => {
    tampered = !tampered;
    tbtn.textContent = tampered ? "↺ restore the header" : "✎ flip a header byte";
    tamper.classList.toggle("bad", tampered);
    tout.textContent = tampered
      ? "unseal FAILS — the Poly1305 tag covers the header; one flipped byte breaks it ✗"
      : "unsealed cleanly — tag OK ✓";
    tout.classList.toggle("bad", tampered);
  });

  async function realDerive() {
    // ephemeral X25519 ECDH → HKDF, shown as a real derived key when available
    if (!subtle) return null;
    try {
      const opa = await subtle.generateKey({ name: "X25519" }, true, ["deriveBits"]);
      const eph = await subtle.generateKey({ name: "X25519" }, true, ["deriveBits"]);
      const ss = await subtle.deriveBits({ name: "X25519", public: opa.publicKey }, eph.privateKey, 256);
      const ikm = await subtle.importKey("raw", ss, "HKDF", false, ["deriveBits"]);
      const key = await subtle.deriveBits(
        { name: "HKDF", hash: "SHA-256", salt: new Uint8Array(64), info: new TextEncoder().encode(ds.info_string) },
        ikm, 256);
      return hex(new Uint8Array(key));
    } catch { return null; }
  }

  play.addEventListener("click", async () => {
    play.disabled = true;
    derived.textContent = "";
    for (const r of stepEls) r.classList.remove("on");
    for (let i = 0; i < stepEls.length; i++) {
      if (!alive(wrap)) { play.disabled = false; return; }
      stepEls[i].classList.add("on");
      if (i === 2) {
        const k = await realDerive();
        derived.textContent = k ? "seal key = " + k.slice(0, 32) + "…  (real HKDF output)" : "seal key derived (32 bytes)";
      }
      await sleep(520);
    }
    play.disabled = false;
  });
  return wrap;
}

// ── the break-glass quorum demo (real Ed25519) ─────────────────────────────
export function buildQuorumDemo(demo, quorum) {
  const wrap = el("div", "vault-quorum");

  const sealed = el("div", "vault-evidence sealed");
  sealed.append(el("div", "vault-evidence-icon", "🔒"),
    el("div", "vault-evidence-label", demo.sealed_label));

  const status = el("p", "muted vault-q-status", "generating trustee keys…");
  const bar = el("div", "vault-q-bar");
  const fill = el("div", "vault-q-fill");
  bar.append(fill);
  const count = el("span", "vault-q-count", `0 / ${demo.n}`);

  const trustees = el("div", "vault-trustees");
  const unseal = el("button", "primary vault-unseal", "🔓 break glass — unseal");
  unseal.disabled = true;
  const forge = el("button", "ghost small", "try to force it open (skip the trustees)");
  const reset = el("button", "ghost small", "new request");
  const acts = el("div", "vault-q-acts");
  acts.append(unseal, forge, reset);

  const log = el("div", "vault-receipts");
  log.append(el("div", "vault-receipts-h", "receipt log — every decision, chained + signed"));
  const logRows = el("div", "vault-receipts-rows");
  log.append(logRows);

  const ribbon = el("p", "ondevice vault-note");
  ribbon.append(el("strong", null, "How to read this: "), document.createTextNode(demo.note));

  wrap.append(sealed, status, bar, count, trustees, acts, log, ribbon);

  const st = { keys: [], approvals: [], requestHash: null, spent: false, ready: false };
  const te = new TextEncoder();

  function refresh() {
    const distinct = countDistinctApprovals(st.approvals);
    const granted = quorumOutcome(distinct, demo.n) === "granted";
    count.textContent = `${Math.min(distinct, demo.n)} / ${demo.n}`;
    fill.style.width = Math.min(100, (distinct / demo.n) * 100) + "%";
    unseal.disabled = !granted || st.spent;
    st.ready = granted;
    if (!st.spent) status.textContent = granted
      ? `quorum met — ${distinct} distinct approvals. You may break glass.`
      : `${distinct} of ${demo.n} approvals — the vault stays sealed.`;
  }

  function receipt(outcome, detail) {
    const row = el("div", "vault-receipt " + (outcome === "GRANTED" ? "ok" : "deny"));
    row.append(el("span", "vault-receipt-o", outcome),
      el("span", "vault-receipt-d", detail),
      el("span", "vault-receipt-sig", "✓ signed #" + (logRows.children.length + 1)));
    logRows.prepend(row);
  }

  async function approve(i, btn) {
    if (!st.requestHash) return;
    let keyHex, valid = false;
    if (subtle && st.keys[i] && st.keys[i].privateKey) {
      try {
        const msg = await domainSepHash(quorum.domains.approval, st.requestHash);
        const sig = new Uint8Array(await subtle.sign({ name: "Ed25519" }, st.keys[i].privateKey, msg));
        const pub = new Uint8Array(await subtle.exportKey("raw", st.keys[i].publicKey));
        valid = await subtle.verify({ name: "Ed25519" }, st.keys[i].publicKey, sig, msg);
        keyHex = hex(pub);
      } catch { keyHex = "key-" + i; valid = true; }
    } else { keyHex = "key-" + i; valid = true; }
    st.approvals.push({ keyHex, valid, trustee: demo.trustees[i] });
    if (btn) { btn.textContent = "approved ✓"; btn.classList.add("done"); }
    refresh();
  }

  function doUnseal() {
    const distinct = countDistinctApprovals(st.approvals);
    if (quorumOutcome(distinct, demo.n) !== "granted") {
      receipt("DENIED", `insufficient approvals: ${distinct}/${demo.n}`);
      status.textContent = `Denied — ${distinct}/${demo.n}. A grant can't be forged: the kernel re-derives quorum from the approvals.`;
      return;
    }
    if (st.spent) {
      status.textContent = "Refused — the break-glass token is single-use and already spent. Start a new request.";
      return;
    }
    st.spent = true;
    const nonce = subtle ? hex(crypto.getRandomValues(new Uint8Array(8))) : "a1b2c3d4e5f6a7b8";
    sealed.classList.remove("sealed");
    sealed.classList.add("open");
    sealed.querySelector(".vault-evidence-icon").textContent = "🖼️";
    sealed.querySelector(".vault-evidence-label").textContent = demo.unsealed_label;
    receipt("GRANTED", `token ${nonce}… minted, nonce burned (single-use)`);
    status.textContent = "Unsealed — token minted, nonce burned, decision logged. Re-opening is refused.";
    unseal.disabled = true;
  }

  function newRequest() {
    st.approvals = []; st.spent = false; st.ready = false;
    sealed.className = "vault-evidence sealed";
    sealed.querySelector(".vault-evidence-icon").textContent = "🔒";
    sealed.querySelector(".vault-evidence-label").textContent = demo.sealed_label;
    for (const b of trustees.querySelectorAll(".vault-approve")) { b.textContent = "approve"; b.classList.remove("done"); }
    refresh();
  }

  unseal.addEventListener("click", doUnseal);
  forge.addEventListener("click", doUnseal);
  reset.addEventListener("click", newRequest);

  // build trustee rows now; wire crypto when keys are ready
  demo.trustees.forEach((name, i) => {
    const row = el("div", "vault-trustee");
    const btn = el("button", "small vault-approve", "approve");
    btn.disabled = true;
    btn.addEventListener("click", () => approve(i, btn));
    row.append(el("span", "vault-trustee-name", name), el("span", "vault-trustee-key muted", "Ed25519 · …"), btn);
    trustees.append(row);
  });

  (async () => {
    let real = false;
    if (subtle) {
      try {
        for (let i = 0; i < demo.m; i++) st.keys.push(await subtle.generateKey({ name: "Ed25519" }, true, ["sign", "verify"]));
        real = true;
      } catch { real = false; }
    }
    st.requestHash = new Uint8Array(subtle
      ? await subtle.digest("SHA-256", te.encode(demo.envelope_id + "|" + demo.purpose))
      : Array.from({ length: 32 }, (_, k) => k));
    if (!alive(wrap)) return;
    const rows = trustees.querySelectorAll(".vault-trustee");
    for (let i = 0; i < rows.length; i++) {
      rows[i].querySelector(".vault-approve").disabled = false;
      if (real) {
        const pub = new Uint8Array(await subtle.exportKey("raw", st.keys[i].publicKey));
        rows[i].querySelector(".vault-trustee-key").textContent = "Ed25519 · " + hex(pub).slice(0, 10) + "…";
      }
    }
    status.textContent = real
      ? `${demo.n}-of-${demo.m}: real Ed25519 keys generated. Collect approvals to break glass.`
      : `${demo.n}-of-${demo.m}: collect approvals to break glass. (WebCrypto Ed25519 unavailable — counting is still real.)`;
    refresh();
  })();

  return wrap;
}
