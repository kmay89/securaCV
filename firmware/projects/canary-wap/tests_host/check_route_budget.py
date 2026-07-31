#!/usr/bin/env python3
"""Handler-table budget check for the canary-wap firmware.

esp_http_server drops every registration past `max_uri_handlers` with
ESP_ERR_HTTPD_HANDLERS_FULL, and the sketch ignores that return value —
so an under-sized budget silently 404s whole API families (this is why
the Presence, Household, Bluetooth-clear, Chirp and BLE routes vanished:
154 registrations against a 123-slot budget dropped the last 31).

This script emulates the C preprocessor for each shipped build config,
counts the registrations that actually execute on the main HTTP server,
and asserts the computed `total_handlers` budget covers them with margin.
It is the durable guard: add routes without raising the budget and CI
fails here instead of the device silently losing endpoints.

Run from the repo root (CI: firmware.yml host tests):
    python3 firmware/projects/canary-wap/tests_host/check_route_budget.py
"""

import re
import sys
from pathlib import Path

SKETCH = Path("firmware/projects/canary-wap/arduino/canary_wap")
INO = SKETCH / "canary_wap.ino"

# Minimum free slots required after every registration lands.
MARGIN = 8

# Hardware capability flags per target (from build_config.h).
HW = {
    "XIAO_ESP32S3": dict(HW_HAS_CAMERA=1, HW_HAS_PSRAM=1, HW_HAS_BT_CLASSIC=0,
                         HW_HAS_BLE=1, HW_HAS_PDM_MIC=1),
    "XIAO_ESP32C3": dict(HW_HAS_CAMERA=0, HW_HAS_PSRAM=0, HW_HAS_BT_CLASSIC=0,
                         HW_HAS_BLE=1, HW_HAS_PDM_MIC=0),
}

# FEATURE_* values per build profile (from build_config.h), before the
# HW_HAS_* substitutions that some of them reference.
PROFILE = {
    "FULL": dict(
        FEATURE_SD_STORAGE=1, FEATURE_WIFI_AP=1, FEATURE_HTTP_SERVER=1,
        FEATURE_CAMERA_PEEK="HW_HAS_CAMERA", FEATURE_TAMPER_GPIO=0,
        FEATURE_WATCHDOG=1, FEATURE_STATE_LOG=1, FEATURE_MESH_NETWORK=1,
        FEATURE_BLUETOOTH="HW_HAS_BLE", FEATURE_BLE=1,
        FEATURE_BLE_SCAN="HW_HAS_BLE", FEATURE_BLE_STATUS=1,
        FEATURE_SYS_MONITOR=1, FEATURE_WIFI_PRESENCE=1,
        FEATURE_AUDIBLE_CHIRP=1, FEATURE_DATA_MGMT=1, FEATURE_POWER_MONITOR=1,
        FEATURE_POWER_POLICY=1, FEATURE_QR_PROVISION="HW_HAS_CAMERA",
        FEATURE_ACOUSTIC_EVENTS="HW_HAS_PDM_MIC",
        FEATURE_ACOUSTIC_TRANSIENTS="HW_HAS_PDM_MIC",
        FEATURE_VAULT_SNAPSHOT="HW_HAS_CAMERA && HW_HAS_PDM_MIC"),
    "DEV": dict(
        FEATURE_SD_STORAGE=1, FEATURE_WIFI_AP=1, FEATURE_HTTP_SERVER=1,
        FEATURE_CAMERA_PEEK=0, FEATURE_TAMPER_GPIO=0, FEATURE_WATCHDOG=1,
        FEATURE_STATE_LOG=1, FEATURE_MESH_NETWORK=0,
        FEATURE_BLUETOOTH="HW_HAS_BLE", FEATURE_BLE=0, FEATURE_BLE_SCAN=0,
        FEATURE_BLE_STATUS="HW_HAS_BLE", FEATURE_SYS_MONITOR=1,
        FEATURE_WIFI_PRESENCE=1, FEATURE_AUDIBLE_CHIRP=1, FEATURE_DATA_MGMT=1,
        FEATURE_POWER_MONITOR=1, FEATURE_POWER_POLICY=1, FEATURE_QR_PROVISION=0,
        FEATURE_ACOUSTIC_EVENTS="HW_HAS_PDM_MIC", FEATURE_ACOUSTIC_TRANSIENTS=0,
        FEATURE_VAULT_SNAPSHOT=0),
}

# Flags defined outside build_config.h profiles.
STATIC_FLAGS = dict(
    FEATURE_BEACON_CHANNEL=0,     # beacon_channel.h default, no profile sets it
    SECURACV_HAS_HTTPS_SERVER=1,  # count worst case
    DEBUG_HTTP=0,
)

# Configs to check: (profile, target). Arduino IDE default = FULL/S3.
CONFIGS = [("FULL", "XIAO_ESP32S3"), ("DEV", "XIAO_ESP32S3"),
           ("FULL", "XIAO_ESP32C3")]


def resolve_flags(profile, target):
    flags = dict(STATIC_FLAGS)
    flags.update(HW[target])
    for k, v in PROFILE[profile].items():
        if isinstance(v, str):
            # Either a bare HW flag name or a small expression over HW flags
            # (e.g. "HW_HAS_CAMERA && HW_HAS_PDM_MIC"), mirroring build_config.h.
            flags[k] = flags[v] if v in flags else int(eval_cond(v, flags))
        else:
            flags[k] = v
    return flags


def eval_cond(cond, flags):
    c = cond.strip()
    c = re.sub(r"defined\s*\(\s*(\w+)\s*\)",
               lambda m: "1" if m.group(1) in flags else "0", c)
    c = re.sub(r"\bHARDWARE_(\w+)\b",
               lambda m: "1" if flags.get("_TARGET") == m.group(1) else "0", c)
    c = re.sub(r"\bBUILD_PROFILE_(\w+)\b",
               lambda m: "1" if flags.get("_PROFILE") == m.group(1) else "0", c)
    for k, v in flags.items():
        if k.startswith("_"):
            continue
        c = re.sub(r"\b" + re.escape(k) + r"\b", str(int(v)), c)
    c = re.sub(r"\b[A-Za-z_]\w*\b", "0", c)  # unknown identifiers -> 0
    # Bare-number juxtaposition after substitution (e.g. "1 (0)") is not
    # valid Python; insert explicit comparisons only where operators exist.
    c = c.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    try:
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            return bool(eval(c))
    except Exception:
        return True  # fail toward counting (worst case)


def count_active(text, needle, flags):
    """Count lines containing `needle` that survive the preprocessor."""
    stack = []  # [active, taken] per open conditional
    n = 0
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", s)
        if m:
            kind, rest = m.group(1), m.group(2)
            if kind == "if":
                v = eval_cond(rest, flags)
                stack.append([v, v])
            elif kind == "ifdef":
                v = rest.strip() in flags
                stack.append([v, v])
            elif kind == "ifndef":
                v = rest.strip() not in flags
                stack.append([v, v])
            elif kind == "elif" and stack:
                v = (not stack[-1][1]) and eval_cond(rest, flags)
                stack[-1] = [v, stack[-1][1] or v]
            elif kind == "else" and stack:
                stack[-1] = [not stack[-1][1], True]
            elif kind == "endif" and stack:
                stack.pop()
            continue
        if all(f[0] for f in stack) and needle in line and \
                not s.startswith("//") and not s.startswith("*"):
            n += 1
    return n


def strip_block_comments(text):
    """Remove /* */ comments, honoring string/char literals so a literal
    containing */ can't make the regex swallow real code (and drop route
    registrations from the count). Line // comments are left in place —
    count_active already skips lines that start with //."""
    out = []
    i, n = 0, len(text)
    state = "code"  # code | line | block | string | char
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "*":
                state = "block"
                i += 2
            elif c == "/" and nxt == "/":
                # Keep the // marker so count_active still sees a comment
                # line, but consume its body here so an apostrophe in it
                # ("user's") can't be mistaken for a char literal.
                out.append("//")
                state = "line"
                i += 2
            elif c == '"':
                out.append(c); state = "string"; i += 1
            elif c == "'":
                out.append(c); state = "char"; i += 1
            else:
                out.append(c); i += 1
        elif state == "line":
            if c == "\n":
                out.append("\n"); state = "code"; i += 1
            else:
                i += 1
        elif state == "block":
            if c == "*" and nxt == "/":
                state = "code"; i += 2
            else:
                out.append("\n" if c == "\n" else " "); i += 1
        elif state == "string":
            out.append(c)
            if c == "\\" and nxt:
                out.append(nxt); i += 2
            else:
                if c == '"':
                    state = "code"
                i += 1
        elif state == "char":
            out.append(c)
            if c == "\\" and nxt:
                out.append(nxt); i += 2
            else:
                if c == "'":
                    state = "code"
                i += 1
    return "".join(out)


def budget_formula(ino_text, flags):
    """Evaluate the .ino's total_handlers for this config.

    The budget constants are #if-gated (a category is 0 when its feature is
    off), so pick the active `const int NAME = N;` for each name via the
    same preprocessor emulation, then sum the total_handlers expression.
    """
    consts = {}
    stack = []
    for line in ino_text.splitlines():
        s = line.strip()
        m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", s)
        if m:
            kind, rest = m.group(1), m.group(2)
            if kind == "if":
                v = eval_cond(rest, flags); stack.append([v, v])
            elif kind == "ifdef":
                v = rest.strip() in flags; stack.append([v, v])
            elif kind == "ifndef":
                v = rest.strip() not in flags; stack.append([v, v])
            elif kind == "elif" and stack:
                v = (not stack[-1][1]) and eval_cond(rest, flags)
                stack[-1] = [v, stack[-1][1] or v]
            elif kind == "else" and stack:
                stack[-1] = [not stack[-1][1], True]
            elif kind == "endif" and stack:
                stack.pop()
            continue
        if not all(f[0] for f in stack):
            continue
        cm = re.match(r"const int (\w+)\s*=\s*(\d+)\s*;", s)
        if cm:
            consts[cm.group(1)] = int(cm.group(2))
    m = re.search(r"const int total_handlers\s*=\s*([^;]+);", ino_text, re.S)
    expr = m.group(1)
    for name, val in sorted(consts.items(), key=lambda kv: -len(kv[0])):
        expr = re.sub(r"\b" + name + r"\b", str(val), expr)
    expr = expr.replace("\n", " ")
    return eval(expr)


def main():
    ino = strip_block_comments(INO.read_text(errors="replace"))
    module_files = {p.name: strip_block_comments(p.read_text(errors="replace"))
                    for p in list(SKETCH.glob("*.h")) + list(SKETCH.glob("*.cpp"))}

    problems = []
    for profile, target in CONFIGS:
        flags = resolve_flags(profile, target)
        flags["_PROFILE"] = profile
        flags["_TARGET"] = target

        # Registrations executed directly on the main active server. The
        # redirect server (g_http_server) has its own 12-slot budget (6 captive
        # probes + GET/OPTIONS /api/fleet + 2 wildcard redirects + headroom), so
        # exclude it.
        direct = count_active(ino, "httpd_register_uri_handler(active_server,",
                              flags)
        direct += count_active(ino, "httpd_register_uri_handler(server,", flags)

        # Module register functions invoked (actively) from the sketch add
        # their own registrations. Map each call to the file that defines it.
        module_calls = {
            r"csi_integration::init\s*\(": ("csi_integration.cpp",
                                            "httpd_register_uri_handler(server,"),
            r"bluetooth_api::register_routes\s*\(": ("bluetooth_api.h",
                                                     "register_api_handler(server,"),
            r"wifi_presence_api::register_routes\s*\(": ("wifi_presence_api.h",
                                                         "httpd_register_uri_handler(server,"),
            r"household_api::register_routes\s*\(": ("household_api.h",
                                                     "httpd_register_uri_handler(server,"),
            r"audible_chirp_api::register_routes\s*\(": ("audible_chirp_api.h",
                                                         "httpd_register_uri_handler(server,"),
            r"chirp_api::register_routes\s*\(": ("chirp_api.h",
                                                 "register_api_handler(server,"),
            r"beacon_api::register_routes\s*\(": ("beacon_api.h",
                                                  "register_api_handler(server,"),
            r"rf_presence_api::register_routes\s*\(": ("rf_presence_api.h",
                                                       "register_api_handler(server,"),
        }
        module_total = 0
        for call_re, (fname, needle) in module_calls.items():
            if _call_is_active(ino, call_re, flags) and fname in module_files:
                module_total += count_active(module_files[fname], needle, flags)

        total_registrations = direct + module_total
        budget = budget_formula(ino, flags)
        free = budget - total_registrations
        status = "OK" if free >= MARGIN else "FAIL"
        print(f"[{status}] {profile}/{target}: {total_registrations} "
              f"registrations, budget {budget}, {free} free "
              f"(margin >= {MARGIN})")
        if free < MARGIN:
            problems.append((profile, target, total_registrations, budget))

    if problems:
        print("\nHANDLER BUDGET CHECK FAILED — raise total_handlers in "
              "start_http_server() (canary_wap.ino):")
        for profile, target, need, budget in problems:
            print(f"  - {profile}/{target}: need {need}+{MARGIN}, "
                  f"budget is {budget}")
        return 1
    return 0


def _call_is_active(ino_text, call_re, flags):
    """True if a line matching call_re survives the preprocessor."""
    stack = []
    pat = re.compile(call_re)
    for line in ino_text.splitlines():
        s = line.strip()
        m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", s)
        if m:
            kind, rest = m.group(1), m.group(2)
            if kind == "if":
                v = eval_cond(rest, flags)
                stack.append([v, v])
            elif kind == "ifdef":
                v = rest.strip() in flags
                stack.append([v, v])
            elif kind == "ifndef":
                v = rest.strip() not in flags
                stack.append([v, v])
            elif kind == "elif" and stack:
                v = (not stack[-1][1]) and eval_cond(rest, flags)
                stack[-1] = [v, stack[-1][1] or v]
            elif kind == "else" and stack:
                stack[-1] = [not stack[-1][1], True]
            elif kind == "endif" and stack:
                stack.pop()
            continue
        if all(f[0] for f in stack) and pat.search(line) and \
                not s.startswith("//") and not s.startswith("*"):
            return True
    return False


if __name__ == "__main__":
    sys.exit(main())
