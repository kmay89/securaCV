// wire_types.mjs — the device type a tree with no firmware/configs/ directory
// puts on the fleet's wire, read from the places its sources publish it.
//
// A config's DEVICE_TYPE is the type that device puts on the fleet's wire,
// which is what both figure lookups are fed: the glass's figure_for() reads
// device_type off a peer's MQTT status and state rows, the phone's
// forDeviceType() reads the mDNS `dt` record. The WAP's sketch reads no config
// and its wire type is not its DEVICE_TYPE constant: canary_wap.ino declares
// DEVICE_TYPE "canary" for its REST /api/status, its signed records and its
// boot banner, but advertises `dt` "canary-wap" over mDNS and csi_mqtt.cpp
// publishes "device_type":"canary-wap" on its status topic (sweep F258).
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

import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';

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

/* The rows. Keyed `<family>/<flavor>` like a config, so gen_figures.mjs's
 * CONFIG_FIGURE, CONFIG_FIGURELESS and one-device-family backing treat a row
 * the same way; each carries its first source as `source`. */
export const TREE_TYPES = [
  {
    family: 'canary-wap', flavor: 'sketch',
    sources: [
      { file: 'firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino', read: txtRecord('dt') },
      { file: 'firmware/projects/canary-wap/arduino/canary_wap/csi_mqtt.cpp', read: jsonKey('device_type') },
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
