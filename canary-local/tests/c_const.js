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
//   • comments are blanked and string and character literals kept (a literal
//     can hold `//` or `/*`), and a definition spelled inside a literal is not
//     one;
//   • NAME is a whole word defined exactly once, as `#define NAME value` or
//     `constexpr T NAME = value;` (two definitions are the preprocessor's
//     choice, which a regex cannot make);
//   • the value is a plain decimal literal (U/L suffixes allowed, no leading
//     zero, at most 2^53 - 1) that ends the #define's line or the
//     constexpr's statement.
// Anything else throws, naming the constant, the file and the reason, in the
// words read_const() dies with. canary-local/tools/tests/c_const_cases.json
// is the table both are held to, verdict for verdict and message for message
// (here by vision.test.js, there by test_flash_read_const.py), so a change to
// one reader that the other does not make fails one of them.
//
// Whitespace and identifier characters are C's, spelled out ([ \t\n\r\f\v],
// [A-Za-z0-9_]): JavaScript's \s and Python's \s disagree on Unicode spaces,
// and neither is what the compiler reads.

"use strict";

// Python compiles its pattern with re.S, so its `\\.` crosses a newline too:
// `\\[\s\S]` here.
const C_COMMENT_OR_LITERAL =
  /"(?:\\[\s\S]|[^"\\\n])*"|'(?:\\[\s\S]|[^'\\\n])*'|\/\*[\s\S]*?\*\/|\/\/[^\n]*/g;
const SP = "[ \\t\\n\\r\\f\\v]";
const C_NAME = /^[A-Za-z_][A-Za-z0-9_]*$/;
const C_DECIMAL = /^(0|[1-9][0-9]*)[uUlL]*$/;
const C_OCTAL = /^0[0-9]+[uUlL]*$/;
const EDGE_SPACE = /^[ \t\n\r\f\v]+|[ \t\n\r\f\v]+$/g;

function blankC(text, keepLiterals) {
  return text.replace(C_COMMENT_OR_LITERAL, (token) => {
    if (token[0] === '"' || token[0] === "'")
      // Masked to quote marks (newlines kept), a literal keeps its length,
      // so a span found in the masked text is the same span of the code, and
      // it is not whitespace, so a #define's value still starts where the
      // literal does.
      return keepLiterals ? token : token.replace(/[^\n]/g, '"');
    return "\n".repeat(token.split("\n").length - 1) || " ";
  });
}

// `text` with every comment blanked and every literal kept; a block comment
// keeps its newlines, so a line number still names its line.
const stripCComments = (text) => blankC(text, true);

// The integer NAME is defined as in `text` (the file `where` names), or a
// thrown Error saying why it is not read.
function readConst(text, name, where) {
  if (!C_NAME.test(name)) throw new Error(`constant name \`${name}\` is not a C identifier`);
  const code = blankC(text, true);
  const shape = blankC(text, false);
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
  const [from, to] = m.groups.define !== undefined ? m.indices.groups.define
                                                    : m.indices.groups.constexpr;
  const value = code.slice(from, to).replace(EDGE_SPACE, "");
  const literal = C_DECIMAL.exec(value);
  if (!literal) {
    const octal = C_OCTAL.test(value) ? " (a leading 0 is octal in C and C++)" : "";
    throw new Error(`constant ${name} in ${where} is \`${value}\`, ` +
      `not a plain integer literal${octal}; refusing to read a number out of it`);
  }
  const number = Number(literal[1]);
  if (number > Number.MAX_SAFE_INTEGER)
    throw new Error(`constant ${name} in ${where} is \`${value}\`, past 2^53 - 1, ` +
      "which a browser cannot hold exactly; refusing to read it");
  return number;
}

module.exports = { readConst, stripCComments };
