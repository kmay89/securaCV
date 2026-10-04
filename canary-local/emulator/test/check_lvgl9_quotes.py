#!/usr/bin/env python3
"""check_lvgl9_quotes.py — hold fake_lvgl9/lvgl.h's quotes to LVGL itself (F225).

firmware/projects/canary-display/tests_host/test_lvgl_port_turn compiles the
real ui/lvgl_port.cpp against fake_lvgl9/lvgl.h, which QUOTES the LVGL 9.5
code the port's turned flush relies on, "as written there", so the host test
is held to LVGL rather than to itself. Nothing re-read those quotes against
the library after they were copied. This script does, against a checkout of
the release the display's Arduino profiles pin (glass_turn_lvgl9.sh fetches
it and runs this first):

  * the release the fake says it quotes (LVGL_VERSION_MAJOR/MINOR/PATCH) is
    the checkout's (lv_version.h), so a pin bump makes the quotes be re-read;
  * lv_display_rotate_area() and lv_display_rotate_point()
    (src/display/lv_display.c) and the portable rotate90/180/270_rgb565 loops
    (src/draw/sw/lv_draw_sw_utils.c) are, token for token after comments and
    whitespace, what the fake quotes — parameters and body. The library's
    loops open with an assembly hook (`if(LV_RESULT_OK ==
    LV_DRAW_SW_ROTATE90_RGB565(...)) return;`) the fake leaves out; that hook
    must still default to LV_RESULT_INVALID (no assembly backend), and it is
    the only thing dropped before comparing;
  * lv_draw_sw_rotate() still hands an RGB565 area at 90/180/270 to those
    loops, the dispatch the fake's lv_draw_sw_rotate() stands in for;
  * LV_DRAW_BUF_STRIDE_ALIGN still defaults to 1 outside Kconfig, and the
    display's lv_conf.h does not set it (the fake's width_to_stride is w * 2).

Exit 0 when every quote holds, 1 naming each one that drifted (with the
first differing tokens), 2 when a file or a function is missing.

  python3 check_lvgl9_quotes.py <lvgl-9-checkout>
  python3 check_lvgl9_quotes.py --self-test     # proves it fails on drift

--self-test builds a stand-in checkout from the fake's own quotes in a
temporary directory and requires that it passes, and that each of a set of
one-token drifts (an operand, an operator, a parameter, the hook's default,
the dispatch, the stride default, the version) fails, naming the quote.
"""

from __future__ import annotations

import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
PROJ = os.path.join(REPO, "firmware", "projects", "canary-display")
FAKE = os.path.join(PROJ, "tests_host", "fake_lvgl9", "lvgl.h")
LV_CONF = os.path.join(PROJ, "include", "lv_conf.h")

# (function, file under the checkout) — quoted in the fake "as written there".
QUOTES = [
    ("lv_display_rotate_area", "src/display/lv_display.c"),
    ("lv_display_rotate_point", "src/display/lv_display.c"),
    ("rotate90_rgb565", "src/draw/sw/lv_draw_sw_utils.c"),
    ("rotate180_rgb565", "src/draw/sw/lv_draw_sw_utils.c"),
    ("rotate270_rgb565", "src/draw/sw/lv_draw_sw_utils.c"),
]
TURNS = ("90", "180", "270")

TOKEN = re.compile(
    r"[A-Za-z_]\w*|0[xX][0-9a-fA-F]+|\d+|->|\+\+|--|<<=|>>=|[-+*/%&|^!=<>]=|&&|\|\||<<|>>|\.\.\.|\S")


class Missing(Exception):
    pass


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def tokens(src: str) -> list[str]:
    return TOKEN.findall(src)


def _balanced(src: str, i: int, open_ch: str, close_ch: str) -> int:
    """Index just past the bracket matching the one at src[i]."""
    depth = 0
    for j in range(i, len(src)):
        if src[j] == open_ch:
            depth += 1
        elif src[j] == close_ch:
            depth -= 1
            if depth == 0:
                return j + 1
    raise Missing(f"unbalanced {open_ch}{close_ch}")


def definition(src: str, name: str) -> tuple[list[str], list[str]]:
    """(parameter tokens, body tokens) of `name`'s definition in `src`
    (comments already stripped). Prototypes and calls are skipped."""
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", src):
        # A call sits after an operator or inside an expression; a definition
        # follows its return type (an identifier or a `*`).
        before = src[: m.start()].rstrip()
        if not before or not re.search(r"(\w|\*)$", before) or re.search(r"\breturn$", before):
            continue
        open_paren = m.end() - 1
        close = _balanced(src, open_paren, "(", ")")
        rest = src[close:].lstrip()
        if not rest.startswith("{"):
            continue  # a prototype
        body_at = len(src) - len(rest)
        end = _balanced(src, body_at, "{", "}")
        return tokens(src[open_paren + 1: close - 1]), tokens(src[body_at + 1: end - 1])
    raise Missing(f"no definition of {name}()")


def first_diff(a: list[str], b: list[str]) -> str:
    n = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
    ctx = lambda t: " ".join(t[max(0, n - 6): n + 6]) or "(end)"  # noqa: E731
    return f"token {n}: the fake has `{ctx(a)}`, LVGL has `{ctx(b)}`"


def drop_asm_hook(name: str, body: list[str]) -> tuple[list[str], str | None]:
    """The library's loops open with `if ( LV_RESULT_OK == LV_DRAW_SW_ROTATEnn_RGB565
    ( ... ) ) { return ; }`; return the body without it and the hook's name."""
    m = re.fullmatch(r"rotate(\d+)_rgb565", name)
    if not m:
        return body, None
    hook = f"LV_DRAW_SW_ROTATE{m.group(1)}_RGB565"
    head = ["if", "(", "LV_RESULT_OK", "==", hook, "("]
    if body[: len(head)] != head:
        return body, None
    try:
        close = body.index(")", len(head))
        tail = body[close: close + 6]
        if tail == [")", ")", "{", "return", ";", "}"]:
            return body[close + 6:], hook
    except ValueError:
        pass
    return body, None


def read(path: str) -> str:
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except OSError as e:
        raise Missing(f"cannot read {path}: {e.strerror}") from e


def check(lvgl: str, fake_path: str = FAKE, lv_conf_path: str = LV_CONF) -> list[str]:
    """Every drift, as one line each (empty: the quotes hold)."""
    problems: list[str] = []
    fake = strip_comments(read(fake_path))

    # The release the fake quotes is the checkout's.
    def version(src: str, where: str) -> str:
        parts = []
        for part in ("MAJOR", "MINOR", "PATCH"):
            m = re.search(r"#define\s+LVGL_VERSION_" + part + r"\s+(\d+)", src)
            if not m:
                raise Missing(f"{where} defines no LVGL_VERSION_{part}")
            parts.append(m.group(1))
        return ".".join(parts)

    fake_ver = version(fake, "the fake")
    lib_ver = version(read(os.path.join(lvgl, "lv_version.h")), "lv_version.h")
    if fake_ver != lib_ver:
        problems.append(f"version: fake_lvgl9/lvgl.h quotes LVGL {fake_ver}, the checkout is "
                        f"{lib_ver} — re-read every quote against it, then bump the fake")

    sources: dict[str, str] = {}
    for name, rel in QUOTES:
        if rel not in sources:
            sources[rel] = strip_comments(read(os.path.join(lvgl, rel)))
        f_params, f_body = definition(fake, name)
        l_params, l_body = definition(sources[rel], name)
        l_body, hook = drop_asm_hook(name, l_body)
        if name.startswith("rotate"):
            if hook is None:
                problems.append(f"{name}: {rel} no longer opens with the assembly hook the fake "
                                f"leaves out (re-read the loop)")
            elif not re.search(r"#\s*ifndef\s+" + hook + r"\s*#\s*define\s+" + hook +
                               r"\s*\(\s*\.\.\.\s*\)\s+LV_RESULT_INVALID\b", sources[rel]):
                problems.append(f"{name}: {hook} no longer defaults to LV_RESULT_INVALID in {rel}, "
                                f"so the portable loop the fake quotes may not be the one that runs")
        if f_params != l_params:
            problems.append(f"{name}: parameters drifted from {rel} ({first_diff(f_params, l_params)})")
        if f_body != l_body:
            problems.append(f"{name}: body drifted from {rel} ({first_diff(f_body, l_body)})")

    # lv_draw_sw_rotate() still hands RGB565 at each quarter turn to its loop.
    utils_rel = "src/draw/sw/lv_draw_sw_utils.c"
    _, disp = definition(sources[utils_rel], "lv_draw_sw_rotate")
    text = " ".join(disp)
    for turn in TURNS:
        block = re.search(r"rotation == LV_DISPLAY_ROTATION_" + turn + r" \) \{(.*?)(?:rotation ==|$)", text)
        call = (f"case LV_COLOR_FORMAT_RGB565 : rotate{turn}_rgb565 ( src , dest , src_width , "
                f"src_height , src_stride , dest_stride ) ;")
        if not block or call not in block.group(1):
            problems.append(f"lv_draw_sw_rotate: {utils_rel} no longer hands an RGB565 area at "
                            f"{turn} to rotate{turn}_rgb565 (the fake's dispatch)")

    # The stride the fake's width_to_stride assumes.
    internal = strip_comments(read(os.path.join(lvgl, "src/lv_conf_internal.h")))
    m = re.search(r"#ifndef LV_DRAW_BUF_STRIDE_ALIGN(.*?)#endif\s*#endif", internal, flags=re.S)
    if not m or not re.search(r"#else\s*#define\s+LV_DRAW_BUF_STRIDE_ALIGN\s+1\s*$", m.group(1)):
        problems.append("LV_DRAW_BUF_STRIDE_ALIGN: lv_conf_internal.h no longer defaults it to 1 "
                        "outside Kconfig (the fake's lv_draw_buf_width_to_stride is w * 2)")
    conf = strip_comments(read(lv_conf_path))
    if re.search(r"#\s*define\s+LV_DRAW_BUF_STRIDE_ALIGN\b", conf):
        problems.append("LV_DRAW_BUF_STRIDE_ALIGN: the display's lv_conf.h sets it; the fake "
                        "assumes the default")
    if re.search(r"#\s*define\s+LV_USE_DRAW_SW_ASM\b", conf):
        problems.append("rotate*_rgb565: the display's lv_conf.h picks an assembly backend "
                        "(LV_USE_DRAW_SW_ASM), which may replace the portable loops the fake quotes")
    return problems


# ── --self-test ──────────────────────────────────────────────────────────

def _stand_in(root: str, fake_src: str) -> None:
    """A checkout built from the fake's own quotes, laid out like LVGL's."""
    clean = strip_comments(fake_src)

    def src_of(name: str) -> str:
        params, body = definition(clean, name)
        return f"void {name}({' '.join(params)})\n{{\n{' '.join(body)}\n}}\n"

    ver = {p: re.search(r"#define\s+LVGL_VERSION_" + p + r"\s+(\d+)", clean).group(1)
           for p in ("MAJOR", "MINOR", "PATCH")}
    files = {
        "lv_version.h": "".join(f"#define LVGL_VERSION_{p} {v}\n" for p, v in ver.items()),
        "src/display/lv_display.c": "/* stand-in */\n" + src_of("lv_display_rotate_area") +
        "void lv_display_rotate_area(lv_display_t * disp, lv_area_t * area);\n" +
        src_of("lv_display_rotate_point"),
        "src/lv_conf_internal.h": ("#ifndef LV_DRAW_BUF_STRIDE_ALIGN\n    #ifdef LV_KCONFIG_PRESENT\n"
                                   "        #define LV_DRAW_BUF_STRIDE_ALIGN 0\n    #else\n"
                                   "        #define LV_DRAW_BUF_STRIDE_ALIGN 1\n    #endif\n#endif\n"),
    }
    utils = []
    for turn in TURNS:
        hook = f"LV_DRAW_SW_ROTATE{turn}_RGB565"
        utils.append(f"#ifndef {hook}\n    #define {hook}(...) LV_RESULT_INVALID\n#endif\n")
    for turn in TURNS:
        name = f"rotate{turn}_rgb565"
        params, body = definition(clean, name)
        args = ", ".join(p for p in params if re.fullmatch(r"\w+", p) and p not in
                         ("const", "uint16_t", "int32_t"))
        utils.append(f"static void {name}({' '.join(params)});\n")
        utils.append(f"static void {name}({' '.join(params)})\n{{\n    if(LV_RESULT_OK == "
                     f"LV_DRAW_SW_ROTATE{turn}_RGB565({args})) {{\n        return ;\n    }}\n"
                     f"{' '.join(body)}\n}}\n")
    disp = "void lv_draw_sw_rotate(const void * src, void * dest, int32_t src_width, int32_t src_height, " \
           "int32_t src_stride, int32_t dest_stride, lv_display_rotation_t rotation, " \
           "lv_color_format_t color_format)\n{\n"
    for turn in TURNS:
        disp += (f"    if(rotation == LV_DISPLAY_ROTATION_{turn}) {{\n        switch(color_format) {{\n"
                 f"            case LV_COLOR_FORMAT_RGB565:\n"
                 f"                rotate{turn}_rgb565(src, dest, src_width, src_height, src_stride, dest_stride);\n"
                 f"                break;\n            default:\n                break;\n        }}\n"
                 f"        return;\n    }}\n")
    utils.append(disp + "}\n")
    files["src/draw/sw/lv_draw_sw_utils.c"] = "".join(utils)
    files["lv_conf.h"] = read(LV_CONF)  # the display's own, edited by the drifts
    for rel, text in files.items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)


def _edit(root: str, rel: str, old: str, new: str) -> None:
    """Replace every `old` in the stand-in's `rel` (a prototype and its
    definition alike)."""
    path = os.path.join(root, rel)
    text = read(path)
    if old not in text:
        raise AssertionError(f"self-test drift: `{old}` not in {rel}")
    with open(path, "w", encoding="utf-8") as f:
        f.write(text.replace(old, new))


def self_test() -> int:
    fake_src = read(FAKE)
    drifts = [
        # (what, file, old, new, the quote the failure must name)
        ("an operand in rotate_area", "src/display/lv_display.c",
         "area -> y2 = disp -> ver_res - area -> x1 - 1", "area -> y2 = disp -> ver_res - area -> x1 - 2",
         "lv_display_rotate_area"),
        ("the 90 turn's axis in rotate_point", "src/display/lv_display.c",
         "point -> x = disp -> ver_res - y - 1", "point -> x = disp -> hor_res - y - 1",
         "lv_display_rotate_point"),
        ("an index in the 90 loop", "src/draw/sw/lv_draw_sw_utils.c",
         "( src_width - x - 1 )", "( src_width - x )", "rotate90_rgb565"),
        ("an operator in the 180 loop", "src/draw/sw/lv_draw_sw_utils.c",
         "dst [ dstIndex + width - x - 1 ]", "dst [ dstIndex - width + x + 1 ]", "rotate180_rgb565"),
        ("a parameter of the 270 loop", "src/draw/sw/lv_draw_sw_utils.c",
         "static void rotate270_rgb565(const uint16_t * src", "static void rotate270_rgb565(uint16_t * src",
         "rotate270_rgb565"),
        ("the 90 hook's default", "src/draw/sw/lv_draw_sw_utils.c",
         "#define LV_DRAW_SW_ROTATE90_RGB565(...) LV_RESULT_INVALID",
         "#define LV_DRAW_SW_ROTATE90_RGB565(...) lv_rotate90_rgb565_asm(__VA_ARGS__)", "rotate90_rgb565"),
        ("the 180 dispatch", "src/draw/sw/lv_draw_sw_utils.c",
         "rotate180_rgb565(src, dest, src_width, src_height, src_stride, dest_stride);\n                break;",
         "rotate180_argb8888(src, dest, src_width, src_height, src_stride, dest_stride);\n                break;",
         "lv_draw_sw_rotate"),
        ("the stride default", "src/lv_conf_internal.h",
         "#define LV_DRAW_BUF_STRIDE_ALIGN 1", "#define LV_DRAW_BUF_STRIDE_ALIGN 4", "LV_DRAW_BUF_STRIDE_ALIGN"),
        ("the release", "lv_version.h", "#define LVGL_VERSION_MINOR ", "#define LVGL_VERSION_MINOR 1", "version"),
        ("lv_conf.h setting the stride", "lv_conf.h", "#endif /* LV_CONF_H */",
         "#define LV_DRAW_BUF_STRIDE_ALIGN 4\n#endif /* LV_CONF_H */", "LV_DRAW_BUF_STRIDE_ALIGN"),
        ("lv_conf.h picking an assembly backend", "lv_conf.h", "#endif /* LV_CONF_H */",
         "#define LV_USE_DRAW_SW_ASM 255\n#endif /* LV_CONF_H */", "rotate"),
    ]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        base = os.path.join(tmp, "base")
        _stand_in(base, fake_src)
        got = check(base, lv_conf_path=os.path.join(base, "lv_conf.h"))
        if got:
            print("  self-test FAIL: the fake's own quotes do not pass:\n    " + "\n    ".join(got))
            failures += 1
        for i, (what, rel, old, new, names) in enumerate(drifts):
            root = os.path.join(tmp, f"drift{i}")
            _stand_in(root, fake_src)
            _edit(root, rel, old, new)
            got = check(root, lv_conf_path=os.path.join(root, "lv_conf.h"))
            if not any(p.startswith(names) for p in got):
                print(f"  self-test FAIL: {what} passed (or was not named {names}): {got}")
                failures += 1
        # A function the library no longer defines is a hard stop, not a pass.
        root = os.path.join(tmp, "gone")
        _stand_in(root, fake_src)
        _edit(root, "src/display/lv_display.c", "void lv_display_rotate_point(", "void lv_display_turn_point(")
        try:
            check(root, lv_conf_path=os.path.join(root, "lv_conf.h"))
            print("  self-test FAIL: a missing lv_display_rotate_point() passed")
            failures += 1
        except Missing:
            pass
    if failures:
        print(f"check_lvgl9_quotes self-test: {failures} FAILED")
        return 1
    print(f"check_lvgl9_quotes self-test: the fake's quotes pass, {len(drifts)} drifts and a missing "
          f"function fail")
    return 0


def main(argv: list[str]) -> int:
    if argv[1:] == ["--self-test"]:
        return self_test()
    if len(argv) != 2:
        print(__doc__.split("\n\n")[0], file=sys.stderr)
        print("usage: check_lvgl9_quotes.py <lvgl-9-checkout> | --self-test", file=sys.stderr)
        return 2
    try:
        problems = check(argv[1])
    except Missing as e:
        print(f"check_lvgl9_quotes: {e}", file=sys.stderr)
        return 2
    if problems:
        print("check_lvgl9_quotes: fake_lvgl9/lvgl.h no longer quotes the LVGL it names:")
        for p in problems:
            print(f"  {p}")
        return 1
    print(f"check_lvgl9_quotes: fake_lvgl9/lvgl.h's {len(QUOTES)} quoted functions, the RGB565 "
          f"dispatch and the stride default match {argv[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
