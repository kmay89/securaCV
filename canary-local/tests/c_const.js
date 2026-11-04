// canary-local/tests/c_const.js — reads one integer constant out of a
// firmware header by the rule canary-local/tools/_tooling.py's read_const()
// reads it by (loaded by tests, never a test itself).
//
// vision.test.js holds devices/vision.json to the Vision's config.h and
// detect_config.h, and gen_vision.py wrote that file with read_const(). The
// two used to share one unanchored regex, `NAME\s*=\s*(\d+)`, so they agreed
// with each other on the wrong number: a commented-out or #if-ed-out
// definition above the live one, `kNAME = 99`, a string holding `NAME = 99`,
// the 3 of `3 * 500`, or `070` read as 70 when it compiles as 56 (sweep F369).
//
// The rule, spelled the same in both languages:
//   • comments are blanked and string, character and raw string literals
//     kept (a literal can hold `//` or `/*`), and a definition spelled inside
//     a literal is not one. Where each ends follows the compiler's lexing for
//     the cases a header meets: a backslash ending a line continues a line
//     comment or a literal, a raw string runs to its own delimiter, and a
//     digit separator (`1'000`) does not open a character literal. The
//     preprocessor is not run, and a splice outside a comment or a literal
//     is not undone. A #define's line ends at the first newline outside a
//     comment, as the compiler ends it: one that runs into a block comment
//     closed on a later line goes on there, so code after the comment's
//     close is refused by its own reason (sweep F429);
//   • NAME is a whole word defined exactly once, as `#define NAME value` or
//     `constexpr T NAME = value;` (two definitions are the preprocessor's
//     choice, which a regex cannot make);
//   • the value is a plain decimal literal (U/L suffixes allowed, no leading
//     zero, at most 2^53 - 1) that ends the #define's line or the
//     constexpr's statement.
// Anything else throws, naming the constant, the file and the reason, in the
// words read_const() dies with. c_const_cases.json, next to this file, is
// the table both are held to, verdict for verdict and message for message
// (here by vision.test.js, there by test_flash_read_const.py), so a change to
// one reader that the other does not make fails one of them.
//
// Whitespace and identifier characters are C's, spelled out ([ \t\n\r\f\v],
// [A-Za-z0-9_]): JavaScript's \s and Python's \s disagree on Unicode spaces,
// and neither is what the compiler reads.

"use strict";

// What the scan steps over whole, left to right, so whichever opens first
// wins (the places a definition is not code, and numbers): a raw string
// literal (`R"d(...)d"`, an optional u8/u/U/L prefix, its body verbatim to
// its own `)d"`), a string or character literal, a block comment, a line
// comment, and a number (a pp-number, so the digit separator in `1'000`
// opens no character literal).
// A backslash that ends a line (blanks after it allowed, as GCC allows them)
// splices the next line into the literal or the line comment it sits in.
// Python compiles its pattern with re.S, so its `.` crosses a newline too:
// `[\s\S]` here.
const SPLICE = String.raw`\\[ \t]*\r?\n`;
const C_TOKEN = new RegExp([
  String.raw`(?<![A-Za-z0-9_])(?:u8|[uUL])?R"([!-'*-\[\]-~]{0,16})\([\s\S]*?\)\1"`,
  String.raw`"(?:${SPLICE}|\\[\s\S]|[^"\\\n])*"`,
  String.raw`'(?:${SPLICE}|\\[\s\S]|[^'\\\n])*'`,
  String.raw`\/\*[\s\S]*?\*\/`,
  String.raw`\/\/(?:${SPLICE}|[^\n])*`,
  String.raw`(?<![A-Za-z0-9_.])\.?[0-9](?:[eEpP][+-]|'[0-9A-Za-z_]|[0-9A-Za-z_.])*`,
].join("|"), "g");
const SP = "[ \\t\\n\\r\\f\\v]";
const C_NAME = /^[A-Za-z_][A-Za-z0-9_]*$/;
const C_DECIMAL = /^(0|[1-9][0-9]*)[uUlL]*$/;
const C_OCTAL = /^0[0-9]+[uUlL]*$/;
const EDGE_SPACE = /^[ \t\n\r\f\v]+|[ \t\n\r\f\v]+$/g;

// `text` with every comment blanked (its newlines kept, so a line number
// still names its line), every number kept, and every literal kept or,
// without `keepLiterals`, masked. Given `runsOn` (a Map), each block comment
// that holds a newline is entered in it: the offset its blank starts at in
// the result, mapped to the offset that blank ends at. Those newlines are
// the comment's, not the line's: a #define goes on past them, where the
// compiler takes the line the comment closes on as more of it.
function blankC(text, keepLiterals, runsOn) {
  let shrunk = 0; // how much shorter the result is than `text`, so far
  // C_TOKEN has one capture group (a raw string's delimiter), so the
  // match's offset is the third argument.
  return text.replace(C_TOKEN, (token, _delimiter, offset) => {
    if (token[0] === "/") {
      const out = "\n".repeat(token.split("\n").length - 1) || " ";
      if (runsOn && token[1] === "*" && out[0] === "\n") {
        const at = offset - shrunk;
        runsOn.set(at, at + out.length);
      }
      shrunk += token.length - out.length;
      return out;
    }
    if ("0123456789.".includes(token[0])) return token;
    // Masked to quote marks (newlines kept), a literal keeps its length, so
    // a span found in the masked text is the same span of the code, and it
    // is not whitespace, so a #define's value still starts where the
    // literal does.
    return keepLiterals ? token : token.replace(/[^\n]/g, '"');
  });
}

// What a #define's line goes on with after the block comments it runs into,
// from `end`, where its value stops in `shape` (the first newline there, or
// the end of the text): each block comment `runsOn` holds that starts there
// is stepped over to the next newline, again and again, and what lies
// between is cut from `code` at the same span, its edge whitespace stripped;
// the pieces are joined by a space. Empty when the value's line ends for
// real, or only blanks follow each comment's close.
function pastComments(code, shape, end, runsOn) {
  const pieces = [];
  while (runsOn.has(end)) {
    const start = runsOn.get(end);
    const found = shape.indexOf("\n", start);
    end = found < 0 ? shape.length : found;
    const piece = code.slice(start, end).replace(EDGE_SPACE, "");
    if (piece) pieces.push(piece);
  }
  return pieces.join(" ");
}

// The integer NAME is defined as in `text` (the file `where` names), or a
// thrown Error saying why it is not read.
function readConst(text, name, where) {
  if (!C_NAME.test(name))
    throw new Error(`constant name \`${name}\` is not a C identifier; refusing to look for it in ${where}`);
  const code = blankC(text, true);
  const runsOn = new Map();
  const shape = blankC(text, false, runsOn);
  const definition = new RegExp(
    `#define[ \\t]+${name}[ \\t]+(?<define>[^\\n]*)` +
    `|(?<![A-Za-z0-9_])constexpr${SP}+[A-Za-z0-9_]+${SP}+${name}${SP}*=${SP}*` +
    `(?<constexpr>[^;]*);`, "gd");
  const found = [...shape.matchAll(definition)];
  if (!found.length) throw new Error(`constant ${name} not found in ${where}`);
  if (found.length > 1) {
    const lines = found.map((m) => shape.slice(0, m.index).split("\n").length).join(", ");
    throw new Error(`constant ${name} is defined ${found.length} times in ${where} ` +
      `(lines ${lines}); which one compiles is the preprocessor's call, so refusing to pick one`);
  }
  const m = found[0];
  const isDefine = m.groups.define !== undefined;
  const [from, to] = isDefine ? m.indices.groups.define : m.indices.groups.constexpr;
  const value = code.slice(from, to).replace(EDGE_SPACE, "");
  // The #define's value stops at a newline in `shape`; one a block comment
  // left there is not the end of its line (a constexpr's runs to its `;`).
  const after = isDefine ? pastComments(code, shape, to, runsOn) : "";
  if (after)
    throw new Error(`constant ${name} in ${where} runs into a block comment closed on a later line, ` +
      `and the #define goes on past the comment (\`${after}\`); refusing to read a ` +
      "number out of it");
  const literal = C_DECIMAL.exec(value);
  if (!literal) {
    const octal = C_OCTAL.test(value) ? " (a leading 0 is octal in C and C++)" : "";
    throw new Error(`constant ${name} in ${where} is \`${value}\`, ` +
      `not a plain integer literal${octal}; refusing to read a number out of it`);
  }
  // More than 16 digits is past 2^53 - 1 (16 of them) whatever they are,
  // checked first as read_const checks it.
  const digits = literal[1];
  if (digits.length > 16 || Number(digits) > Number.MAX_SAFE_INTEGER)
    throw new Error(`constant ${name} in ${where} is \`${value}\`, past 2^53 - 1, ` +
      "which a browser cannot hold exactly; refusing to read it");
  return Number(digits);
}

module.exports = { readConst };
