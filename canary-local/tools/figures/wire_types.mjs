// wire_types.mjs — the device type a tree with no firmware/configs/ directory
// puts on the fleet's wire, read from the places its sources publish it.
//
// A config's DEVICE_TYPE is the type that device puts on the fleet's wire,
// which is what both figure lookups are fed: the glass's figure_for() reads
// device_type off a peer's MQTT status and state rows, the phone's
// forDeviceType() reads the mDNS `dt` record. The WAP's sketch reads no config.
// It publishes its wire type in three kinds of site: the mDNS `dt` records in
// canary_wap.ino, the "device_type" of csi_mqtt.cpp's status topic, and the
// sketch's DEVICE_TYPE constant, which REST /api/status, node_status.node,
// /api/fleet's product, the boot banner and the hashed BOOT attestation read
// (sweeps F258, F294). The reader holds all three to one value. It does not
// read ble_improv.cpp's Improv firmware-name string, a literal in a file that
// cannot see the sketch's constant.
//
// So a row lists every file that publishes the type and how it does, and
// walkTrees() reads EVERY such site in those files, not only the ones in the
// shape it can read: a site whose value is not one string literal (a
// variable, a macro, a concatenation, a printf %s, an ArduinoJson
// assignment) is refused by file and line, because a type this reader cannot
// spell is a type it must not report as agreed. It also refuses a file that
// is gone or publishes no site, and sites that disagree. Comments are
// blanked first (string literals kept), so a commented-out call is no site.
// gen_figures.mjs builds its tree rows from this; figures.test.js holds it to
// the sketch and to every refusal.
//
// treeConfigsOfEnvs() (sweep A89) says which build envs compile a row, so the
// board those envs target serves the row's wire type in the hardware table.
// readProjects() reads what it needs from each firmware/projects/<p>/
// platformio.ini; the join takes those records as an argument, so
// figures.test.js can drive every refusal with synthetic projects.

import { readFileSync, existsSync, readdirSync } from 'node:fs';
import { join, relative } from 'node:path';

/** The source text with every comment replaced by spaces (newlines kept, so
 * offsets and line numbers still point into the file). String and character
 * literals are kept whole, so a `//` inside "mqtt://" is not a comment. */
export function codeOnly(src) {
  let out = '';
  let i = 0;
  while (i < src.length) {
    const c = src[i];
    const n = src[i + 1];
    if (c === '/' && n === '/') {
      while (i < src.length && src[i] !== '\n') { out += ' '; i++; }
    } else if (c === '/' && n === '*') {
      const end = src.indexOf('*/', i + 2);
      const stop = end < 0 ? src.length : end + 2;
      for (; i < stop; i++) out += src[i] === '\n' ? '\n' : ' ';
    } else if (c === '"' || c === '\'') {
      out += c; i++;
      while (i < src.length && src[i] !== c && src[i] !== '\n') {
        if (src[i] === '\\' && i + 1 < src.length) { out += src[i] + src[i + 1]; i += 2; continue; }
        out += src[i]; i++;
      }
      if (i < src.length) { out += src[i]; i++; }
    } else {
      out += c; i++;
    }
  }
  return out;
}

/** The arguments of the call whose `(` is at `open`, split at top-level
 * commas (strings and nested parentheses respected), or null when the call
 * never closes. */
function callArgs(code, open) {
  const args = [];
  let depth = 0;
  let cur = '';
  for (let i = open; i < code.length; i++) {
    const c = code[i];
    if (c === '"' || c === '\'') {
      let j = i + 1;
      while (j < code.length && code[j] !== c) j += code[j] === '\\' ? 2 : 1;
      cur += code.slice(i, j + 1);
      i = j;
      continue;
    }
    if (c === '(') { if (depth++ === 0) continue; }
    if (c === ')') { if (--depth === 0) { args.push(cur.trim()); return args; } }
    if (c === ',' && depth === 1) { args.push(cur.trim()); cur = ''; continue; }
    cur += c;
  }
  return null;
}

// One C string literal and nothing else: no escapes, no concatenation.
const LITERAL = /^"([^"\\]+)"$/;

/** Every `<obj>.addServiceTxt(service, proto, key, value)` call: a call whose
 * key is `key` names its value; a call whose key is not one string literal
 * cannot be told apart from it, so it is refused too. */
export function txtRecord(key) {
  return {
    what: `mDNS TXT record "${key}"`,
    sites(code) {
      const out = [];
      for (const m of code.matchAll(/\baddServiceTxt\s*\(/g)) {
        const args = callArgs(code, m.index + m[0].length - 1);
        const at = m.index;
        if (!args || args.length !== 4) {
          out.push({ at, refuse: `an addServiceTxt call with ${args ? args.length : 'unclosed'} arguments, not 4` });
          continue;
        }
        const k = LITERAL.exec(args[2]);
        if (!k) { out.push({ at, refuse: `an addServiceTxt key that is not one string literal (${args[2]})` }); continue; }
        if (k[1] !== key) continue;
        const v = LITERAL.exec(args[3]);
        out.push(v ? { at, type: v[1] }
          : { at, refuse: `the "${key}" value is not one string literal (${args[3]})` });
      }
      return out;
    },
  };
}

/** Every `key` token in the code: each must be the JSON key of a literal
 * value inside a C string, `\"key\":\"value\"`; anything else (a %s, a
 * string that ends there, an ArduinoJson doc["key"], an identifier) is
 * refused. */
export function jsonKey(key) {
  const shape = new RegExp(`^\\\\"${key}\\\\":\\\\"([^"\\\\%]+)\\\\"`);
  return {
    what: `JSON key "${key}" in a C string`,
    sites(code) {
      const out = [];
      for (const m of code.matchAll(new RegExp(`\\b${key}\\b`, 'g'))) {
        const v = shape.exec(code.slice(Math.max(0, m.index - 2)));
        out.push(m.index >= 2 && v ? { at: m.index, type: v[1] }
          : { at: m.index, refuse: `"${key}" is not the key of a literal value here `
            + `(${code.slice(Math.max(0, m.index - 2), m.index + key.length + 24).split('\n')[0].trim()})` });
      }
      return out;
    },
  };
}

/** Where the statement that starts at `from` ends: the offset of its first
 * `;` outside a string or character literal, or the end of the code. */
function statementEnd(code, from) {
  for (let i = from; i < code.length; i++) {
    const c = code[i];
    if (c === ';') return i;
    if (c === '"' || c === '\'') {
      let j = i + 1;
      while (j < code.length && code[j] !== c && code[j] !== '\n') j += code[j] === '\\' ? 2 : 1;
      i = j;
    }
  }
  return code.length;
}

/** Every assignment to the `name` constant. Its one declaration names the
 * type: `static const char* <name> = "<type>";`, or the same with the pointer
 * itself const (`static const char* const`, or `static constexpr const
 * char*`), the stronger form, since the compiler then refuses any other
 * assignment too. A declaration whose value is not one string literal is
 * refused, and so is a second declaration. So is any other assignment to the
 * name: `const char*` leaves the pointer itself writable, so a function could
 * repoint it (`=`, a compound assignment such as `+=` or `-=`, `++` or `--`,
 * prefix or postfix), and a type set at run time is not one this reader can
 * spell. The match is lexical, on the name: a write through a pointer or a
 * reference to the constant is not seen. `\b` keeps a longer name
 * (WAP_DEVICE_TYPE) out, and a comparison (`==`) is not a write. */
export function constString(name) {
  const write = new RegExp(`\\b${name}\\b\\s*(?:(<<|>>|[-+*/%&|^])?=(?!=)|\\+\\+|--)`
    + `|(?:\\+\\+|--)\\s*\\b${name}\\b`, 'g');
  const declPrefix =
    /^[ \t]*static[ \t]+(?:constexpr[ \t]+)?const[ \t]+char[ \t]*\*[ \t]*(?:const[ \t]+)?$/;
  const literalValue = /^\s*("[^"\\\n]*")\s*$/;
  return {
    what: `${name} constant`,
    sites(code) {
      const decls = [];
      const out = [];
      for (const m of code.matchAll(write)) {
        const at = m.index;
        const plain = m[0].endsWith('=') && m[1] === undefined;
        const before = code.slice(code.lastIndexOf('\n', at - 1) + 1, at);
        if (!plain || !declPrefix.test(before)) {
          out.push({ at, refuse: `${name} is assigned here; publish it from its one declaration `
            + `(static const char* const ${name} = "<type>";, or without the second const)` });
          continue;
        }
        const from = at + m[0].length;
        const expr = code.slice(from, statementEnd(code, from));
        const v = literalValue.exec(expr);
        const lit = v && LITERAL.exec(v[1]);
        decls.push(lit ? { at, type: lit[1] }
          : { at, refuse: `${name} is not one string literal (${expr.replace(/\s+/g, ' ').trim()})` });
      }
      if (decls.length > 1) {
        for (const d of decls) out.push({ at: d.at, refuse: `${name} is declared ${decls.length} times` });
      } else {
        out.push(...decls);
      }
      return out.sort((a, b) => a.at - b.at);
    },
  };
}

/* The rows. Keyed `<family>/<flavor>` like a config, so gen_figures.mjs's
 * CONFIG_FIGURE, CONFIG_FIGURELESS and one-device-family backing treat a row
 * the same way; each carries its first source as `source`. The WAP's
 * constant is read third, after the two wire sites, so `source` stays the
 * sketch's mDNS file and the agreement error lists the wire sites first. */
export const TREE_TYPES = [
  {
    family: 'canary-wap', flavor: 'sketch',
    sources: [
      { file: 'firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino', read: txtRecord('dt') },
      { file: 'firmware/projects/canary-wap/arduino/canary_wap/csi_mqtt.cpp', read: jsonKey('device_type') },
      { file: 'firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino', read: constString('DEVICE_TYPE') },
    ],
  },
];

const lineOf = (code, at) => code.slice(0, at).split('\n').length;

/** Each row's one wire type, with every site it was read from, or a throw
 * naming what could not be read. `root` is the checkout the files are under. */
export function walkTrees(root, rows = TREE_TYPES) {
  return rows.map((t) => {
    const seen = new Map();   // device type -> ["file:line", ...]
    const refused = [];
    for (const src of t.sources) {
      const path = join(root, src.file);
      if (!existsSync(path)) {
        throw new Error(`figures: TREE_TYPES reads ${t.family}'s device type from ${src.file}, which is gone. `
          + 'Point the row at the files that publish it, or remove the row.');
      }
      const code = codeOnly(readFileSync(path, 'utf8'));
      const sites = src.read.sites(code);
      if (!sites.length) {
        throw new Error(`figures: ${src.file} no longer publishes ${t.family}'s device type as the `
          + `${src.read.what} TREE_TYPES reads. Point the row at what does, or drop the source.`);
      }
      for (const s of sites) {
        const where = `${src.file}:${lineOf(code, s.at)}`;
        if (s.refuse) refused.push(`${where}: ${s.refuse}`);
        else seen.set(s.type, [...(seen.get(s.type) || []), where]);
      }
    }
    if (refused.length) {
      throw new Error(`figures: ${t.family}'s device type is published at ${refused.length} site(s) `
        + `this reader cannot spell, so it cannot say they agree:\n  ${refused.join('\n  ')}\n`
        + 'Publish one string literal there, or teach wire_types.mjs the new shape.');
    }
    if (seen.size !== 1) {
      throw new Error(`figures: ${t.family} publishes ${seen.size} different device types on the wire `
        + `(${[...seen].map(([dt, where]) => `"${dt}" at ${where.join(', ')}`).join('; ')}). `
        + 'A figure lookup can only follow one; make the sources agree.');
    }
    const [[device_type, sites]] = [...seen];
    return { family: t.family, flavor: t.flavor, device_type, source: t.sources[0].file, sites };
  });
}

/** Each firmware/projects/<p>/platformio.ini under `root`, as {project, src,
 * files}: the src_dir it compiles (PlatformIO's default `src` when it names
 * none) and the set of .ini files its envs can be defined in (itself and
 * every `extra_configs` entry, comments dropped), all relative to `root`. A
 * project directory with no platformio.ini is not a project. */
export function readProjects(root) {
  const base = join(root, 'firmware/projects');
  const out = [];
  for (const p of readdirSync(base, { withFileTypes: true }).filter((e) => e.isDirectory()).sort(
    (a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0))) {
    const ini = join(base, p.name, 'platformio.ini');
    if (!existsSync(ini)) continue;
    const pio = (readFileSync(ini, 'utf8').match(/^\[platformio\]\n([\s\S]*?)(?=^\[|$(?![\s\S]))/m) || [])[1] || '';
    const src = (pio.match(/^src_dir\s*=\s*(\S+)/m) || [])[1] || 'src';
    const extra = pio.match(/^extra_configs\s*=[ \t]*(.*)\n((?:[ \t]+\S.*(?:\n|$))*)/m);
    const named = extra ? [extra[1], ...extra[2].split('\n')] : [];
    const rel = (f) => relative(root, join(base, p.name, f));
    out.push({
      project: `firmware/projects/${p.name}`,
      src: rel(src),
      files: new Set([rel('platformio.ini'), ...named.map((x) => x.replace(/;.*$/, '').trim())
        .filter(Boolean).map(rel)]),
    });
  }
  return out;
}

/* A TREE_TYPES row (the WAP's sketch) has no firmware/configs/ flavor, so the
 * envs that build it include no configs/ header and resolve `config: null`,
 * and the board they compile served nothing in the hardware table
 * (board:seeed_xiao_esp32s3 read `serves: []`, sweep A89). Which envs build a
 * sketch is not typed anywhere: an env defined in one of a project's files
 * (`projects`, from readProjects()) compiles that project's src_dir, so an env
 * whose project's src_dir holds every source a row reads compiles that row,
 * and is keyed `<family>/<flavor>` like a config: this sets `config` on each
 * such env in `envs` ([{env, file, config}]). Refused, not guessed: a row no
 * project (or two) compiles, a row no env builds, an env file two projects
 * pull in, and an env that also includes a configs/ flavor. */
export function treeConfigsOfEnvs(envs, projects, rows = TREE_TYPES) {
  for (const row of rows) {
    const key = `${row.family}/${row.flavor}`;
    const homes = projects.filter((p) => row.sources.every((s) => s.file.startsWith(`${p.src}/`)));
    if (homes.length !== 1) {
      throw new Error(`figures: TREE_TYPES row ${key} reads ${row.sources.map((s) => s.file).join(', ')}, `
        + `which ${homes.length ? `${homes.length} projects (${homes.map((h) => h.project).join(', ')})`
          : 'no firmware/projects/<p>/platformio.ini src_dir'} compile. A row is one sketch tree.`);
    }
    const [home] = homes;
    const built = envs.filter((e) => home.files.has(e.file));
    if (!built.length) {
      throw new Error(`figures: no env builds ${home.project}'s src_dir ${home.src}, which TREE_TYPES `
        + `row ${key} reads; nothing compiles the row.`);
    }
    for (const e of built) {
      const owners = projects.filter((p) => p.files.has(e.file)).map((p) => p.project);
      if (owners.length > 1) {
        throw new Error(`figures: env ${e.env} is defined in ${e.file}, which ${owners.join(' and ')} `
          + 'all pull in, so it builds more than one src_dir; define it in one project\'s files.');
      }
      if (e.config && e.config !== key) {
        throw new Error(`figures: env ${e.env} builds the ${key} sketch (${home.src}) and includes `
          + `configs/${e.config} too; one build cannot publish both.`);
      }
      e.config = key;
    }
  }
  return envs;
}
