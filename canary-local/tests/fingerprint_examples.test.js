// canary-local/tests/fingerprint_examples.test.js — every fingerprint and
// public-key example in the Lab pages' generated JSON (canary-local/devices/
// *.json) is spelled the way its product spells it (sweep A25).
//
// The WAP page's signed MQTT examples carried "fp":"7f3a9c21", and its boot
// log "Public key fingerprint: 7f3a9c21". Both are 8 hex digits. The envelope
// fp is 16 (the 8 bytes of pubkey_fp) and lowercase since HA20; the boot line
// prints g_device.fingerprint_hex, 16 capitals. Nothing failed on either:
// gen_wap.py held each topic's suffix and the boot lines' prefixes to the
// firmware, not the fp values written into them. An example of the wrong
// length or case teaches a reader the wrong shape to expect, or to compare by
// eye against a TOFU pin.
//
// So this walks every generated JSON in devices/ and finds each fp,
// fingerprint, pubkey and public_key example in it: a property, a key inside
// an example payload, a console line's "fingerprint: X" / "fp=X" /
// "pinned: X", a CLI flag's value. Each one must match exactly one RULE, and
// the rule says how the product that prints it spells it: a whole value has
// the rule's length and case; one elided with "…" has the case and is
// shorter; a bare "…" (as every example sig is) is not checked. Each rule's
// spelling is pinned to the source that writes it. An example no rule covers
// fails: read how its product spells it, then add the rule.
//
// What it does not see. It reads the generated JSON and nothing else, so an
// example a page's hand-written script spells for itself is out of its
// reach: the Vision page's simulated MQTT pane (assets/vision-ui.js) writes
// its health row's public_key as "ed25519:…", and canary-vision sends 64
// bare lowercase hex digits (an open item of its own, A26).
//
// Since A27 it also checks the examples that are missing: every events,
// chain and counts example must carry the signature envelope, so the Home
// Assistant page's old WAP chain line (no v, alg or fp) fails below. Since
// A28 and A29 it holds the names a key or a salt derives (the WAP's device
// id, SSID and unnamed host; the Sense and Vision pseudonym, host and MQTT
// client id) to the derivation, not just to a shape.
//
// The WAP's examples are also the repo's Ed25519 test key's (seed 0x42 x 32):
// the key the WAP's tests_host/test_mqtt_identity.cpp builds its events body
// with, and that Home Assistant's tests/test_fingerprint_case.py fixtures
// (WAP_EVENT and friends) are signed by. Each of those derives the key from
// the seed in its own test, and so does this one, so the page, the firmware
// test and HA show one fingerprint. HA's fixture is not read here: it is
// outside canary-local.yml's path filter, and its own test holds it to the
// seed.

const { test } = require("node:test");
const assert = require("node:assert");
const crypto = require("node:crypto");
const { readFileSync, readdirSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const DEVICES = join(ROOT, "devices");
const WAP = "firmware/projects/canary-wap/arduino/canary_wap";

const read = (rel) => readFileSync(join(REPO, rel), "utf8");

// ── the repo's Ed25519 test key ────────────────────────────────────────────
// seed 0x42 x 32 -> Ed25519 public key -> SHA256("securacv:pubkey:fingerprint"
// || 0x00 || pubkey)[0..8], canary_wap.ino's compute_fingerprint.
function testKey() {
  const pkcs8 = Buffer.concat([Buffer.from("302e020100300506032b657004220420", "hex"), Buffer.alloc(32, 0x42)]);
  const priv = crypto.createPrivateKey({ key: pkcs8, format: "der", type: "pkcs8" });
  const pub = crypto.createPublicKey(priv).export({ format: "der", type: "spki" }).subarray(-32);
  const fp = crypto.createHash("sha256")
    .update("securacv:pubkey:fingerprint").update(Buffer.from([0])).update(pub)
    .digest().subarray(0, 8);
  return { pub, fp, fpHex: fp.toString("hex") };
}
const KEY = testKey();

// ── what a rule is ─────────────────────────────────────────────────────────
// page: the devices/*.json file. where: the JSON path the example sits at.
// labels: what names it (the property, the payload key, or the word before
// a console line's value). len / kase: the whole value's hex digits and case
// ("any" where the product parses either). pins: [file, literal] pairs that
// make the spelling the source's, not this file's belief.
const PAYLOAD = /^\.mqtt\.topics\[\d+\]\.payload$/;
// The Hub page's "Meet the fleet" wire lines are the WAP's payloads (A27).
const WAP_WIRE = /^(?:\.mqtt\.topics\[\d+\]\.payload|\.terminal\.chapters\[\d+\]\.steps\[\d+\]\.out\[\d+\])$/;
const WAP_PAGES = ["wap.json", "homeassistant.json"];
const RULES = [
  {
    page: WAP_PAGES, where: WAP_WIRE, labels: ["fp"], len: 16, kase: "lower",
    what: "the WAP's envelope fp (mqtt_identity::fingerprint_hex, sweep HA20)",
    pins: [
      [`${WAP}/mqtt_identity.h`, 'kLowerHex[] = "0123456789abcdef"'],
      [`${WAP}/mqtt_identity.h`, "constexpr size_t FP_BYTES    = 8;"],
      [`${WAP}/canary_wap.ino`, "mqtt_identity::fingerprint_hex(mqtt_fp_hex, g_device.pubkey_fp);"],
      [`${WAP}/canary_wap.ino`, "g_device.device_id,\n                         mqtt_fp_hex);"],
      [`${WAP}/csi_mqtt.cpp`, "device_signature::fingerprint_hex()"],
    ],
  },
  {
    page: WAP_PAGES, where: WAP_WIRE, labels: ["public_key"], len: 64, kase: "lower",
    what: "the WAP's health public_key (mqtt_identity::public_key_hex)",
    pins: [
      [`${WAP}/mqtt_identity.h`, "constexpr size_t KEY_BYTES   = 32;"],
      [`${WAP}/canary_wap.ino`,
        "mqtt_identity::public_key_hex(pubkey_hex, g_device.pubkey);\n" +
        "  csi_mqtt::init(g_device.device_id, FIRMWARE_VERSION, pubkey_hex);"],
    ],
  },
  {
    page: "wap.json", where: /^\.serial\.boot\[\d+\]\.text$/, labels: ["fingerprint"], len: 16, kase: "upper",
    what: "the WAP's [PROV] boot line (g_device.fingerprint_hex, hex_to_str)",
    pins: [
      [`${WAP}/canary_wap.ino`, 'static const char hex[] = "0123456789ABCDEF";'],
      [`${WAP}/canary_wap.ino`, "hex_to_str(g_device.fingerprint_hex, g_device.pubkey_fp, 8);"],
      [`${WAP}/canary_wap.ino`, 'Serial.printf("[PROV] Public key fingerprint: %s\\n", g_device.fingerprint_hex);'],
    ],
  },
  {
    page: "sense.json", where: /^(?:\.mqtt\.topics\[\d+\]\.payload|\.serial\.boot\[\d+\]\.text|\.device\.fp_example)$/,
    labels: ["fp", "fp_example"], len: 16, kase: "lower",
    what: "the Sense's fp (witness.cpp fp_hex)",
    pins: [
      ["firmware/projects/canary-sense/src/witness.cpp", 'static const char H[] = "0123456789abcdef";'],
      ["firmware/projects/canary-sense/src/witness.cpp", "fp_hex[16] = '\\0';"],
      ["firmware/projects/canary-sense/src/witness.cpp", '"Ed25519 ready  fp=%s  chain_len=%lu\\n"'],
    ],
  },
  {
    page: "sense.json", where: PAYLOAD, labels: ["public_key"], len: 64, kase: "lower",
    what: "the Sense's health public_key (device_signature::pubkey_hex)",
    pins: [
      ["firmware/projects/canary-sense/src/net/mqtt_mgr.cpp", "device_signature::pubkey_hex(),"],
      ["firmware/common/identity/device_signature.cpp", "hex_encode(pub, 32, s_pubkey_hex, sizeof(s_pubkey_hex));"],
      ["firmware/common/identity/device_signature.cpp", 'static const char H[] = "0123456789abcdef";'],
    ],
  },
  {
    page: "operator.json", where: /^\.ceremony\.steps\[\d+\]\.out\[\d+\]$/, labels: ["pinned", "seed"], len: 16, kase: "lower",
    what: "break_glass's device identity (short_fp)",
    pins: [
      ["src/break_glass/cli.rs", 'bytes[..8].iter().map(|b| format!("{:02x}", b)).collect()'],
      ["src/break_glass/cli.rs", '"  {} device identity pinned: {}",'],
      ["src/break_glass/cli.rs", '"  {} device public key pinned, matches the supplied seed: {}",'],
    ],
  },
  {
    page: "operator.json", where: /^\.ceremony\.steps\[\d+\]\.cmd$/, labels: ["public-key"], len: 64, kase: "any",
    what: "trustee enroll's --public-key (hex::decode, 32 bytes)",
    pins: [
      ["src/break_glass/cli.rs", "/// Import an existing 32-byte Ed25519 public key (hex)"],
      ["src/break_glass/cli.rs", '.map_err(|e| anyhow!("invalid public key hex: {}", e))?;'],
      ["src/break_glass/cli.rs", '"public key must be 32 bytes, got {}"'],
    ],
  },
  {
    page: "flash.json", where: /^\.release_pubkey$/, labels: ["release_pubkey"], len: 64, kase: "lower",
    what: "the OTA release key (gen_flash.py read_release_pubkey)",
    pins: [
      ["canary-local/tools/gen_flash.py", 'return "".join(b.lower() for b in bytes_)'],
      ["canary-local/tools/gen_flash.py", "if len(bytes_) != 32:"],
    ],
  },
];

// ── finding the examples ───────────────────────────────────────────────────
const NAME = "(?:[a-z]+_)*(?:fp|fingerprint|pubkey|public_key)(?:_[a-z]+)*";
const PROP = new RegExp(`^${NAME}$`, "i");
const EMBEDDED = new RegExp(`"(${NAME})"\\s*:\\s*"([^"]*)"`, "gi");
// "fingerprint: X", "fp=X", "pinned: X", "--public-key X": a value that is
// hex (4+ digits, maybe elided) or a bare ellipsis, standing alone.
const LINE = /\b(fingerprint|fp|pubkey|public[-_ ]key|pinned|seed)(?:\s*[:=]\s*|\s+)([0-9A-Fa-f]{4,}…?|…)(?![0-9A-Za-z_…])/gi;

function* strings(node, path) {
  if (typeof node === "string") { yield [path, node, null]; return; }
  if (Array.isArray(node)) { for (let i = 0; i < node.length; i++) yield* strings(node[i], `${path}[${i}]`); return; }
  if (node && typeof node === "object")
    for (const [k, v] of Object.entries(node)) {
      if (typeof v === "string" && PROP.test(k)) { yield [`${path}.${k}`, v, k]; continue; }
      yield* strings(v, `${path}.${k}`);
    }
}

function examplesIn(page, data) {
  const out = [];
  for (const [path, s, prop] of strings(data, "")) {
    if (prop) { out.push({ page, path, label: prop.toLowerCase(), value: s }); continue; }
    for (const m of s.matchAll(EMBEDDED)) out.push({ page, path, label: m[1].toLowerCase(), value: m[2] });
    for (const m of s.matchAll(LINE)) out.push({ page, path, label: m[1].toLowerCase(), value: m[2] });
  }
  return out;
}

const PAGES = readdirSync(DEVICES).filter((f) => f.endsWith(".json")).sort();
const DATA = Object.fromEntries(PAGES.map((page) => [page, JSON.parse(readFileSync(join(DEVICES, page), "utf8"))]));
const EXAMPLES = PAGES.flatMap((page) => examplesIn(page, DATA[page]));

const rulesFor = (ex) => RULES.filter((r) => [].concat(r.page).includes(ex.page) && r.where.test(ex.path) && r.labels.includes(ex.label));

// null when the value is spelled by the rule, else what is wrong with it.
function shapeProblem(value, rule) {
  if (value === "…") return null; // elided whole, as every example sig is
  const elided = value.endsWith("…");
  const body = elided ? value.slice(0, -1) : value;
  const alphabet = { lower: /^[0-9a-f]+$/, upper: /^[0-9A-F]+$/, any: /^[0-9A-Fa-f]+$/ }[rule.kase];
  if (!/^[0-9A-Fa-f]+$/.test(body)) return `"${value}" is not hex`;
  if (!alphabet.test(body)) return `"${value}" is not ${rule.kase}case; ${rule.what} is`;
  if (elided && body.length >= rule.len) return `"${value}" elides nothing; ${rule.what} is ${rule.len} hex digits`;
  if (!elided && body.length !== rule.len) return `"${value}" is ${body.length} hex digits; ${rule.what} is ${rule.len}`;
  return null;
}

// ── the checker itself (so a pass below means something) ─────────────────
test("the shape check rejects the wrong length and case, and accepts elision", () => {
  const [envelope, health, serial] = RULES;
  assert.match(shapeProblem("7f3a9c21", envelope), /8 hex digits/, "the old 8-digit example");
  assert.match(shapeProblem("7916CA487912FA1B", envelope), /not lowercase/);
  assert.match(shapeProblem("7916ca487912fa1b", serial), /not uppercase/);
  assert.match(shapeProblem("7916ca487912fa1b0", envelope), /17 hex digits/);
  assert.match(shapeProblem("7916ca48…", serial), /not uppercase/, "an elided value keeps its case");
  assert.match(shapeProblem(`${"ab".repeat(32)}…`, health), /elides nothing/);
  assert.match(shapeProblem("canary-4dC2", envelope), /not hex/);
  assert.strictEqual(shapeProblem("7916ca487912fa1b", envelope), null);
  assert.strictEqual(shapeProblem("7916CA487912FA1B", serial), null);
  assert.strictEqual(shapeProblem("2152f8d1…", health), null);
  assert.strictEqual(shapeProblem("…", health), null);
  assert.strictEqual(shapeProblem("0123…", RULES.find((r) => r.kase === "any")), null);
});

test("the sweep finds the examples on every page it should (a broken match fails here)", () => {
  const count = (page) => EXAMPLES.filter((e) => e.page === page).length;
  // wap: three envelope fps, the health key, the boot line; sense: three fps,
  // the fp_example, the health key; operator: two identity lines, two flags.
  assert.ok(count("wap.json") >= 5, `wap.json: ${count("wap.json")} examples found`);
  assert.ok(count("sense.json") >= 5, `sense.json: ${count("sense.json")} examples found`);
  assert.ok(count("operator.json") >= 4, `operator.json: ${count("operator.json")} examples found`);
  for (const r of RULES)
    assert.ok(EXAMPLES.some((e) => rulesFor(e).includes(r)), `no example matches the rule for ${r.what}: a dead rule`);
});

test("every rule's spelling is the source's own", () => {
  for (const r of RULES)
    for (const [file, literal] of r.pins)
      assert.ok(read(file).includes(literal), `${r.what}: ${file} no longer has ${JSON.stringify(literal)}`);
});

// ── the gate ───────────────────────────────────────────────────────────────
test("every fp / pubkey example in the generated JSON has the length and case its product prints", () => {
  const problems = [];
  for (const ex of EXAMPLES) {
    const rules = rulesFor(ex);
    const at = `${ex.page} ${ex.path} (${ex.label})`;
    if (rules.length !== 1) {
      problems.push(`${at}: ${rules.length} rules match "${ex.value}" — read how that product spells it and add exactly one`);
      continue;
    }
    const p = shapeProblem(ex.value, rules[0]);
    if (p) problems.push(`${at}: ${p}`);
  }
  assert.deepStrictEqual(problems, []);
});

test("the WAP page's fingerprint is the repo test key's, as the firmware test and HA spell it", () => {
  const { pub, fpHex: fp } = KEY;
  assert.ok(read(`${WAP}/canary_wap.ino`).includes('sha256_domain("securacv:pubkey:fingerprint", pub, 32, hash);'),
    "compute_fingerprint's domain moved; re-derive the test key's fp");
  assert.ok(read(`${WAP}/canary_wap.ino`).includes("uint8_t sep = 0x00;"), "sha256_domain's separator moved");

  const cpp = read("firmware/projects/canary-wap/tests_host/test_mqtt_identity.cpp");
  assert.ok(cpp.includes(`const char kTestFp[] = "${fp}";`), "test_mqtt_identity.cpp's kTestFp is not the seed's fp");
  assert.ok(cpp.includes(`"${pub.toString("hex")}";`), "test_mqtt_identity.cpp's kTestPub is not the seed's key");

  const wap = EXAMPLES.filter((e) => e.page === "wap.json" && !e.value.includes("…"));
  const fps = wap.filter((e) => e.label === "fp");
  assert.strictEqual(fps.length, 3, "events, chain and counts each carry an fp");
  for (const e of fps) assert.strictEqual(e.value, fp, `${e.path}: not the test key's fp`);
  const boot = wap.filter((e) => e.label === "fingerprint");
  assert.strictEqual(boot.length, 1, "one [PROV] fingerprint line");
  assert.strictEqual(boot[0].value, fp.toUpperCase(), "the boot line is the same 8 bytes, in hex_to_str's capitals");
  for (const e of wap.filter((x) => x.label === "public_key"))
    assert.strictEqual(e.value, pub.toString("hex"), `${e.path}: not the test key`);
});

// ── the WAP's names (sweep A29) ────────────────────────────────────────────
// canary_wap.ino derives three names from pubkey_fp[0..1], and none of them
// from the MAC:
//   device id  generate_device_id: DEVICE_ID_PREFIX + unambiguous_suffix16
//   SSID       generate_ap_ssid:   "SecuraCV-" + the same suffix, same case
//   mDNS host  generate_mdns_hostname with no friendly name set:
//              "canary-%02x%02x", four lowercase hex digits, no "-s3-"
// The page used to show device id canary-s3-ab7k beside SSID SecuraCV-AB7K
// (one suffix in two cases, which one device cannot produce) and host
// canary-ab7k.local (k is not hex; only a friendly name could make it). Now
// every one is the test key's, derived here from the seed, so a page that
// shows fp 7916ca487912fa1b shows the names that key's device has. HA's
// tests/test_fingerprint_case.py calls the same device canary-s3-4dC2; it is
// outside canary-local.yml's path filter, so it is derived here, not read.
const INO = read(`${WAP}/canary_wap.ino`);
const WAP_NAME_PINS = [
  'snprintf(out, cap, "%s%s", DEVICE_ID_PREFIX, suffix)',
  'snprintf(out, cap, "SecuraCV-%s", suffix)',
  "out[i] = UNAMBIGUOUS_ALPHABET[v % UNAMBIGUOUS_LEN];",
  "v = (uint16_t)(v / UNAMBIGUOUS_LEN);",
  'snprintf(out, cap, "canary-%02x%02x",\n           g_device.pubkey_fp[0], g_device.pubkey_fp[1]);',
];
const FP_SUFFIX_CALL = "unambiguous_suffix16((uint16_t)((g_device.pubkey_fp[0] << 8) | g_device.pubkey_fp[1]),\n" +
  "                       suffix);";

function wapNames(fp) {
  const alphabet = INO.match(/UNAMBIGUOUS_ALPHABET\[\] =\s*"([^"]+)";/)[1];
  const prefix = INO.match(/#else\s*static const char\* DEVICE_ID_PREFIX = "([^"]+)";/)[1];
  let v = (fp[0] << 8) | fp[1];
  let suffix = "";
  for (let i = 0; i < 4; i++) {
    suffix += alphabet[v % alphabet.length];
    v = Math.floor(v / alphabet.length);
  }
  const hex2 = (b) => b.toString(16).padStart(2, "0");
  return { id: prefix + suffix, ssid: "SecuraCV-" + suffix, host: `canary-${hex2(fp[0])}${hex2(fp[1])}.local` };
}
const WAP_NAMES = wapNames(KEY.fp);

// Every string in the generated JSON, with its page and path.
function* allStrings() {
  for (const page of PAGES)
    for (const [path, s] of strings(DATA[page], "")) yield { page, path, s };
}
// Placeholders a page may show instead of a value: a SoftAP name nobody owns.
const SSID_PLACEHOLDER = "SecuraCV-XXXX";

test("the WAP's names derive from the test key the way canary_wap.ino derives them", () => {
  for (const pin of WAP_NAME_PINS) assert.ok(INO.includes(pin), `canary_wap.ino no longer has ${JSON.stringify(pin)}`);
  assert.strictEqual(INO.split(FP_SUFFIX_CALL).length - 1, 2,
    "generate_device_id and generate_ap_ssid no longer both encode pubkey_fp[0..1]");
  // the derivation itself, on values a reader can check by hand: 0x7916 is
  // 30998 = 2 + 34*54 + 10*54^2 -> digits 2, 34, 10, 0 -> "4dC2"
  assert.deepStrictEqual(wapNames(Buffer.from([0x79, 0x16])),
    { id: "canary-s3-4dC2", ssid: "SecuraCV-4dC2", host: "canary-7916.local" });
  assert.strictEqual(KEY.fpHex.slice(0, 4), "7916", "the seed's fingerprint moved");
});

test("every WAP device id, SSID and unnamed host a generated page shows is the test key's", () => {
  const found = { id: [], ssid: [], host: [] };
  const problems = [];
  for (const { page, path, s } of allStrings()) {
    for (const m of s.matchAll(/\bcanary-[cs]3-[A-Za-z0-9]+/g)) found.id.push({ page, path, v: m[0] });
    for (const m of s.matchAll(/\bSecuraCV-[A-Za-z0-9]+/g))
      if (m[0] !== SSID_PLACEHOLDER) found.ssid.push({ page, path, v: m[0] });
    for (const m of s.matchAll(/\bcanary-[0-9A-Fa-f]{4}\.local\b/g)) found.host.push({ page, path, v: m[0] });
  }
  for (const kind of ["id", "ssid", "host"])
    for (const f of found[kind])
      if (f.v !== WAP_NAMES[kind]) problems.push(`${f.page} ${f.path}: ${f.v} is not the test key's ${kind} (${WAP_NAMES[kind]})`);
  assert.deepStrictEqual(problems, []);

  const wap = DATA["wap.json"];
  assert.strictEqual(wap.device.id_example, WAP_NAMES.id);
  assert.strictEqual(wap.ap.ssid_example, WAP_NAMES.ssid);
  assert.strictEqual(wap.ap.mdns_example, WAP_NAMES.host);
  // the boot log's Device ID and AP lines, and the ready block's two rows
  const onWap = (kind) => found[kind].filter((f) => f.page === "wap.json").length;
  assert.ok(onWap("id") >= 3, `wap.json: ${onWap("id")} device ids found (the sweep's match broke?)`);
  assert.ok(onWap("ssid") >= 3, `wap.json: ${onWap("ssid")} SSIDs found (the sweep's match broke?)`);
});

// ── the salted device pseudonym (sweep A28) ────────────────────────────────
// canary-sense and canary-vision print a "Hardware ID", name their MQTT client
// and their mDNS host after device_pseudonym::device_id_hex: SHA-256 of
// "canary:device-id:v1:" || a per-device salt, rendered as 16 characters of
// the 54-character unambiguous alphabet (no 0/O/o, no 1/I/i/l/L). The Sense
// page showed 9f41c2d8a06be375 and the Vision page b3f2a9c41d5e (12): hex,
// which no unit prints. The Sense host, canary-sense-001-b7e2c4, borrowed
// the fingerprint's first six digits, where make_hostname appends the
// pseudonym's first six characters, case kept. The generators now derive
// each from an example salt (_pseudonym.py), the two salts the shared
// header's host test derives with, and this derives them again.
const PSEUDO_H = "firmware/common/identity/device_pseudonym.h";
const PSEUDO_HOST_TEST = "firmware/projects/canary-wap/tests_host/test_device_pseudonym_common.cpp";
const PSEUDO_PINS = [
  [PSEUDO_H, 'constexpr char   DOMAIN[]    = "canary:device-id:v1:";'],
  [PSEUDO_H, 'constexpr char     ALPHABET[]     = "23456789ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz";'],
  [PSEUDO_H, "constexpr unsigned ALPHABET_LIMIT = 216;"],
  [PSEUDO_H, "constexpr size_t TOKEN_BYTES = 8;"],
  [PSEUDO_H, "constexpr size_t HEX_LEN     = TOKEN_BYTES * 2;"],
  [PSEUDO_H, "memcpy(input + off, detail::DOMAIN, detail::DOMAIN_LEN); off += detail::DOMAIN_LEN;"],
  [PSEUDO_H, "memcpy(input + off, secret, secret_len);"],
  [PSEUDO_H, "if (hash[i] < detail::ALPHABET_LIMIT) {"],
  [PSEUDO_H, "out_hex[produced++] = detail::ALPHABET[hash[i] % detail::ALPHABET_LEN];"],
  [PSEUDO_H, "out_hex[produced] = detail::ALPHABET[produced];"],
  [PSEUDO_HOST_TEST, "memset(secret,  0x11, sizeof(secret));"],
  [PSEUDO_HOST_TEST, "memset(secret2, 0x22, sizeof(secret2));"],
];
const PSEUDO_ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz";
const PSEUDO_SHAPE = new RegExp(`^[${PSEUDO_ALPHABET}]{16}$`);

function pseudonym(salt) {
  const h = crypto.createHash("sha256").update("canary:device-id:v1:").update(salt).digest();
  let out = "";
  for (const b of h) {
    if (out.length >= 16) break;
    if (b < 216) out += PSEUDO_ALPHABET[b % 54];
  }
  while (out.length < 16) out += PSEUDO_ALPHABET[out.length];
  return out;
}
// mdns_mgr.cpp's make_hostname: the id cut to 23 bytes, '_', ' ' and '.'
// turned to '-', then '-' and the pseudonym's first six characters.
function makeHostname(deviceId, pseudo) {
  const base = (deviceId || "canary").slice(0, 23).replace(/[_ .]/g, "-");
  return `${base}-${pseudo.slice(0, 6)}`;
}

// The pages whose product prints the pseudonym, the salt each example uses,
// and the sources that spell the recipe.
const PSEUDO_PAGES = {
  "sense.json": { project: "firmware/projects/canary-sense", salt: 0x11 },
  "vision.json": { project: "firmware/projects/canary-vision", salt: 0x22 },
};
function projectPins(project) {
  return [
    [`${project}/src/main.cpp`, 'boot_kv("Hardware ID", devid_hex);'],
    [`${project}/src/net/mdns_mgr.cpp`, "char base[24];"],
    [`${project}/src/net/mdns_mgr.cpp`, 'copy_str(base, sizeof(base), device_id && device_id[0] ? device_id : "canary");'],
    [`${project}/src/net/mdns_mgr.cpp`, "if (*p == '_' || *p == ' ' || *p == '.') *p = '-';"],
    [`${project}/src/net/mdns_mgr.cpp`, 'snprintf(out, cap, "%s-%.6s", base, devid_hex);'],
    [`${project}/src/net/mqtt_mgr.cpp`, 'String clientId = String("securacv-") + cfg.device_id + "-" + devid_hex;'],
    [`${project}/src/net/mqtt_mgr.cpp`, '"Connecting %s:%u as %s ...\\n", cfg.mqtt_host, cfg.mqtt_port, clientId.c_str()'],
  ];
}
// What each page's product prints, derived.
function pseudoNames(page) {
  const { salt } = PSEUDO_PAGES[page];
  const id = DATA[page].device.id_example;
  const p = pseudonym(Buffer.alloc(32, salt));
  return { pseudonym: p, host: `${makeHostname(id, p)}.local`, client: `securacv-${id}-${p}` };
}

// Every pseudonym example, wherever it sits: a hwid-named property, a
// "Hardware ID <x>" console line, and the client id a "Connecting <broker>
// as <x> ..." line names.
const HWID_PROP = /^(?:[a-z]+_)*(?:hwid|hardware_id|pseudonym)(?:_[a-z]+)*$/i;
function pseudoExamples() {
  const out = [];
  for (const page of PAGES)
    for (const [path, s, prop] of strings(DATA[page], "")) {
      if (prop) continue; // fp-family properties: the RULES above
      const key = path.split(".").pop();
      if (HWID_PROP.test(key)) out.push({ page, path, kind: "pseudonym", value: s });
      for (const m of s.matchAll(/\bHardware ID\s+(\S+)/g)) out.push({ page, path, kind: "pseudonym", value: m[1] });
      for (const m of s.matchAll(/\bConnecting \S+ as (\S+) \.\.\./g)) out.push({ page, path, kind: "client", value: m[1] });
    }
  return out;
}

// Every .local host a generated page names: a fixed name, a template (it
// ends in a <placeholder>, so the match below skips it), or an example a
// product derives — the WAP's unnamed fallback (sweep A29) or make_hostname.
const FIXED_HOSTS = new Set(["canary.local", "homeassistant.local"]);
function hostExamples() {
  const out = [];
  for (const page of PAGES)
    for (const [path, s] of strings(DATA[page], "")) {
      const key = path.split(".").pop();
      if (/^host_example$/.test(key)) { out.push({ page, path, value: `${s}.local` }); continue; }
      for (const m of s.matchAll(/(?<![\w.<>-])[A-Za-z0-9][A-Za-z0-9_-]*\.local\b/g))
        if (!FIXED_HOSTS.has(m[0])) out.push({ page, path, value: m[0] });
    }
  return out;
}
const HOST_RULES = {
  "wap.json": () => WAP_NAMES.host,
  "homeassistant.json": () => WAP_NAMES.host,
  "sense.json": () => pseudoNames("sense.json").host,
  "vision.json": () => pseudoNames("vision.json").host,
};

test("the pseudonym recipe is the firmware's, and the derivation reproduces it", () => {
  for (const [file, literal] of [...PSEUDO_PINS, ...Object.values(PSEUDO_PAGES).flatMap((p) => projectPins(p.project))])
    assert.ok(read(file).includes(literal), `${file} no longer has ${JSON.stringify(literal)}`);
  for (const { project } of Object.values(PSEUDO_PAGES)) {
    const body = read(`${project}/src/net/mdns_mgr.cpp`).split("void make_hostname(")[1].split("\n}\n")[0];
    assert.ok(!/tolower|toupper/.test(body), `${project}'s make_hostname changes case now; the example host must follow`);
  }
  // the derivation on its own: 16 characters, the alphabet only, stable
  const a = pseudonym(Buffer.alloc(32, 0x11));
  assert.match(a, PSEUDO_SHAPE);
  assert.strictEqual(a, pseudonym(Buffer.alloc(32, 0x11)));
  assert.notStrictEqual(a, pseudonym(Buffer.alloc(32, 0x22)));
  assert.strictEqual(makeHostname("canary_sense_001", a), `canary-sense-001-${a.slice(0, 6)}`);
  assert.strictEqual(makeHostname("a.b c_d", "XYZabcdef"), "a-b-c-d-XYZabc", "case kept, separators hyphenated");
});

test("every Hardware ID and MQTT client id a generated page shows is the pseudonym its product prints", () => {
  const found = pseudoExamples();
  const problems = [];
  for (const ex of found) {
    const at = `${ex.page} ${ex.path}`;
    if (!PSEUDO_PAGES[ex.page]) { problems.push(`${at}: ${ex.kind} ${ex.value} on a page with no pseudonym rule`); continue; }
    const want = pseudoNames(ex.page);
    if (ex.kind === "pseudonym") {
      if (!PSEUDO_SHAPE.test(ex.value))
        problems.push(`${at}: "${ex.value}" is not 16 characters of the unambiguous alphabet (device_pseudonym::HEX_LEN)`);
      else if (ex.value !== want.pseudonym) problems.push(`${at}: "${ex.value}" is not the example salt's pseudonym (${want.pseudonym})`);
    } else if (ex.value !== want.client) {
      problems.push(`${at}: client id "${ex.value}" is not mqtt_mgr.cpp's securacv-<id>-<pseudonym> (${want.client})`);
    }
  }
  assert.deepStrictEqual(problems, []);
  const count = (page, kind) => found.filter((e) => e.page === page && e.kind === kind).length;
  assert.ok(count("sense.json", "pseudonym") >= 1, "sense.json: the hwid example went missing (the sweep's match broke?)");
  assert.ok(count("vision.json", "pseudonym") >= 1, "vision.json: the Hardware ID line went missing");
  assert.ok(count("sense.json", "client") >= 1 && count("vision.json", "client") >= 1, "a Connecting line went missing");
});

test("every .local host a generated page names is a fixed name, a template, or the host its product derives", () => {
  const found = hostExamples();
  const problems = [];
  for (const ex of found) {
    const rule = HOST_RULES[ex.page];
    if (!rule) { problems.push(`${ex.page} ${ex.path}: host ${ex.value} on a page with no host rule`); continue; }
    if (ex.value !== rule()) problems.push(`${ex.page} ${ex.path}: host ${ex.value} is not the one its product derives (${rule()})`);
  }
  assert.deepStrictEqual(problems, []);
  assert.ok(found.some((e) => e.page === "wap.json"), "wap.json: the unnamed host went missing");
  assert.ok(found.filter((e) => e.page === "sense.json").length >= 2, "sense.json: host_example and the [MDNS] line");
});

// ── a signed topic's example carries its envelope (sweep A27) ──────────────
// The checks above read the fps that are present; an example with no fp at
// all passed them. The Home Assistant page's WAP chain line was one:
// {"length":1284,"latest_hash":"9f2c…","sig":"ed25519:…"}, no v, alg or fp,
// which HA's signature.py reads as unsigned ("Payload missing sig/fp/alg
// fields") under a note saying the integration verifies it. events, chain
// and counts are the topics a Canary signs, so every example of one carries
// v, alg, fp and sig (a sig or hash elided with "…" is still an example of
// the field). Two kinds of string are examples: a topic-contract entry (an
// object with that suffix and a payload) and a wire line ("<prefix>/<id>/
// chain {…}"). Not covered: a sandbox scene's publishes (`.sandbox[…]`),
// which spell only the fields the scene changes ({"length":+1} is not even
// JSON) — an open item of their own.
const SIGNED_TOPIC = /^(?:events|chain|counts)$/;
const ENVELOPE_PINS = [
  ["firmware/common/identity/device_signature.h", "constexpr int         SCHEMA_V    = 1;"],
  ["firmware/common/identity/device_signature.h", 'constexpr const char* ALG_NAME    = "ed25519";'],
  [`${WAP}/device_signature.h`, "constexpr int         SCHEMA_V    = 1;"],
  [`${WAP}/device_signature.h`, 'constexpr const char* ALG_NAME    = "ed25519";'],
  [`${WAP}/csi_mqtt.cpp`, '"\\"alg\\":\\"%s\\",\\"fp\\":\\"%s\\",\\"sig\\":\\"%s\\"}",'],
];

function* objects(node, path) {
  if (Array.isArray(node)) { for (let i = 0; i < node.length; i++) yield* objects(node[i], `${path}[${i}]`); return; }
  if (node && typeof node === "object") {
    yield [path, node];
    for (const [k, v] of Object.entries(node)) yield* objects(v, `${path}.${k}`);
  }
}
function signedExamples() {
  const out = [];
  for (const page of PAGES) {
    for (const [path, o] of objects(DATA[page], ""))
      if (typeof o.suffix === "string" && SIGNED_TOPIC.test(o.suffix) && typeof o.payload === "string" &&
          !path.startsWith(".sandbox"))
        out.push({ page, path, suffix: o.suffix, payload: o.payload });
    for (const [path, s] of strings(DATA[page], "")) {
      const m = s.match(/^[a-z]+\/[^/\s]+\/(events|chain|counts) (\{.*\})$/);
      if (m) out.push({ page, path, suffix: m[1], payload: m[2] });
    }
  }
  return out;
}

test("every events, chain and counts example carries the v / alg / fp / sig envelope HA reads", () => {
  for (const [file, literal] of ENVELOPE_PINS)
    assert.ok(read(file).includes(literal), `${file} no longer has ${JSON.stringify(literal)}`);
  const found = signedExamples();
  const problems = [];
  for (const ex of found) {
    const at = `${ex.page} ${ex.path} (${ex.suffix})`;
    let p;
    try { p = JSON.parse(ex.payload); } catch { problems.push(`${at}: not JSON: ${ex.payload}`); continue; }
    const missing = ["v", "alg", "fp", "sig"].filter((k) => !(k in p));
    if (missing.length) { problems.push(`${at}: no ${missing.join(", ")} — HA reads it as unsigned`); continue; }
    if (p.v !== 1) problems.push(`${at}: v ${p.v}, not device_signature::SCHEMA_V (1)`);
    if (p.alg !== "ed25519") problems.push(`${at}: alg ${p.alg}, not device_signature::ALG_NAME`);
    if (typeof p.sig !== "string" || !/^(?:[A-Za-z0-9_-]+|[A-Za-z0-9_-]*…)$/.test(p.sig))
      problems.push(`${at}: sig ${JSON.stringify(p.sig)} is not base64url (elided or whole)`);
  }
  assert.deepStrictEqual(problems, []);
  const on = (page) => found.filter((e) => e.page === page).length;
  assert.ok(on("wap.json") >= 3 && on("sense.json") >= 2, "the topic contracts' signed examples went missing");
  assert.ok(on("homeassistant.json") >= 2, "the Hub page's chain and counts wire lines went missing");
});

test("the Hub page's fleet wire lines are the WAP page's retained topics, verbatim", () => {
  const wap = DATA["wap.json"];
  const fleet = DATA["homeassistant.json"].terminal.chapters.find((c) => c.id === "fleet");
  const lines = fleet.steps.flatMap((s) => s.out);
  assert.ok(lines.length >= 4, "the fleet step prints its wire");
  for (const line of lines) {
    const m = line.match(/^([a-z]+)\/([^/\s]+)\/(\S+) (.*)$/);
    assert.ok(m, `not a "<topic> <payload>" line: ${line}`);
    const [, prefix, id, suffix, payload] = m;
    assert.strictEqual(prefix, wap.mqtt.prefix);
    assert.strictEqual(id, WAP_NAMES.id, "the device is the test key's WAP");
    const t = wap.mqtt.topics.find((x) => x.suffix === suffix);
    assert.ok(t && t.retained, `${suffix}: not one of the WAP's retained topics`);
    assert.strictEqual(payload, t.payload, `${suffix}: not gen_wap.py's payload`);
  }
  const suffixes = lines.map((l) => l.split(" ")[0].split("/").pop());
  for (const want of ["health", "chain", "counts"]) assert.ok(suffixes.includes(want), `no ${want} line`);
  assert.ok(DATA["homeassistant.json"].ha_demo.device_id === WAP_NAMES.id, "the demo below is the same device");
});
