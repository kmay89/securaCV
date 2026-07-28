/**
 * @file tuning_console.h
 * @brief Line-based serial tuning console — knob table in, commands out.
 *
 * The bench problem this solves: right after flashing, a person at the USB
 * console (the browser flasher's tuning bench, the desktop Flasher's monitor,
 * or a bare `pio device monitor`) needs to SEE what the radar sees and TURN
 * every runtime knob — without WiFi, without a broker, without HA. This
 * module owns the inbound half of that console: it buffers received bytes
 * into lines, parses a tiny command vocabulary, and applies values through
 * the same clamping setters MQTT/HA use, so no transport can latch a value
 * another would refuse.
 *
 * Layering (firmware/ARCHITECTURE.md): lives in `common/`, so it knows
 * NOTHING about pins, configs, NVS, Arduino, or what the knobs mean. The
 * project hands it a table of named knobs (getter + clamping setter + bounds
 * + default) and a write callback; everything else is plain C++ — fully
 * host-testable (tests_host/test_tuning_console.cpp).
 *
 * Command vocabulary (one command per line; case-insensitive; CR ignored):
 *
 *   help | ?              command list + every knob with range/current value
 *   cfg  | get            one machine-readable line: `[cfg] name=value ...`
 *   set <knob> <value>    clamp + apply + persist (via the knob's setter),
 *                         then reply with the refreshed `[cfg]` line
 *   reset                 every knob back to its compiled default
 *   stream on|off|<ms>    periodic `[radar]` status line (on = 1000 ms;
 *                         period clamped to 200..10000 ms)
 *   raw on|off            bench detail in the stream line (raw cm / BPM);
 *                         session-only, never persisted — see the privacy
 *                         note in the project's main.cpp
 *
 * Replies are single lines so a UI can wire them without a state machine:
 *   `[tune] ok ...` / `[tune] err ...` for verdicts, `[cfg] ...` for the
 *   knob snapshot. The `[radar]` stream itself is emitted by the project
 *   loop (it owns the sensor state); this class only keeps the schedule.
 *
 * No dynamic allocation, no exceptions, wrap-safe timing — same posture as
 * the mr60_* modules.
 */

#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace securacv::console {

// Sink for every reply line (already newline-terminated by the console).
using WriteFn = void (*)(const char* line);

// One runtime knob. `set` must clamp internally (sense_config posture) and
// return true only when the stored value actually changed.
struct Knob {
    const char* name;   // command token, e.g. "debounce" (lowercase ASCII)
    const char* unit;   // "ms" / "cm" / "bpm" — help text only
    const char* help;   // one short clause for `help`
    uint32_t    lo;     // advertised bound (help text; setter re-clamps)
    uint32_t    hi;
    uint32_t    def;    // compiled default, restored by `reset`
    uint32_t  (*get)();
    bool      (*set)(uint32_t v);
};

class TuningConsole {
public:
    static constexpr size_t   MAX_LINE          = 96;
    static constexpr uint32_t STREAM_DEFAULT_MS = 1000;
    static constexpr uint32_t STREAM_MIN_MS     = 200;
    static constexpr uint32_t STREAM_MAX_MS     = 10000;

    // `knobs` must outlive the console (a static table in the project).
    void begin(const Knob* knobs, size_t count, WriteFn write) {
        knobs_ = knobs;
        count_ = count;
        write_ = write;
        len_ = 0;
        overflow_ = false;
        stream_ms_ = STREAM_DEFAULT_MS;
        raw_ = false;
        changed_ = false;
    }

    // Feed one received byte. Executes the buffered line on '\n'.
    void feed(char c) {
        if (c == '\r') return;                    // tolerate CRLF terminals
        if (c == '\n') {
            buf_[len_] = '\0';
            const bool was_overflow = overflow_;
            len_ = 0;
            overflow_ = false;
            if (was_overflow) {
                reply("[tune] err line too long (max %u chars)",
                      (unsigned)(MAX_LINE - 1));
                return;
            }
            execute_(buf_);
            return;
        }
        if (len_ + 1 >= MAX_LINE) { overflow_ = true; return; }
        buf_[len_++] = c;
    }

    // Convenience for tests and paste-style input.
    void feed(const char* s) { while (*s) feed(*s++); }

    // ---- state the project loop reads --------------------------------------

    // Current stream period in ms; 0 means the stream is off.
    uint32_t stream_period_ms() const { return stream_ms_; }
    // Bench detail (raw scalars) requested for the stream line.
    bool raw_enabled() const { return raw_; }
    // True once after any successful set/reset — the project drains this and
    // reconfigures its FSMs / republishes retained cfg.
    bool take_changed() { const bool c = changed_; changed_ = false; return c; }

    // The `[cfg]` snapshot line, also sent on connect-style demands ("cfg").
    void print_cfg() const {
        char line[192];
        size_t n = (size_t)snprintf(line, sizeof(line), "[cfg]");
        for (size_t i = 0; i < count_ && n < sizeof(line); ++i) {
            n += (size_t)snprintf(line + n, sizeof(line) - n, " %s=%lu",
                                  knobs_[i].name,
                                  (unsigned long)knobs_[i].get());
        }
        if (n < sizeof(line)) {
            snprintf(line + n, sizeof(line) - n, " stream=%lu raw=%u",
                     (unsigned long)stream_ms_, raw_ ? 1u : 0u);
        }
        writeln_(line);
    }

    void print_help() const {
        writeln_("[tune] commands:");
        writeln_("[tune]   help | ?              this list");
        writeln_("[tune]   cfg                   all knobs on one line");
        writeln_("[tune]   set <knob> <value>    change a knob (clamped, saved)");
        writeln_("[tune]   reset                 restore compiled defaults");
        writeln_("[tune]   stream on|off|<ms>    periodic [radar] line");
        writeln_("[tune]   raw on|off            bench detail in the stream");
        writeln_("[tune] knobs (name  range  current):");
        for (size_t i = 0; i < count_; ++i) {
            const Knob& k = knobs_[i];
            reply("[tune]   %-11s %lu..%lu %s  now %lu  (%s)",
                  k.name, (unsigned long)k.lo, (unsigned long)k.hi, k.unit,
                  (unsigned long)k.get(), k.help);
        }
    }

private:
    // ---- command execution --------------------------------------------------

    static bool ieq_(const char* a, const char* b) {
        while (*a && *b) {
            if (lower_(*a) != lower_(*b)) return false;
            ++a; ++b;
        }
        return *a == '\0' && *b == '\0';
    }
    static char lower_(char c) {
        return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }

    // Strict uint parse: digits only, fits in uint32. Rejects "", "12x", "-3".
    static bool parse_u32_(const char* s, uint32_t* out) {
        if (!s || !*s) return false;
        uint32_t v = 0;
        for (const char* p = s; *p; ++p) {
            if (*p < '0' || *p > '9') return false;
            const uint32_t d = (uint32_t)(*p - '0');
            if (v > (0xFFFFFFFFu - d) / 10u) return false;  // overflow
            v = v * 10u + d;
        }
        *out = v;
        return true;
    }

    const Knob* find_(const char* name) const {
        for (size_t i = 0; i < count_; ++i) {
            if (ieq_(knobs_[i].name, name)) return &knobs_[i];
        }
        return nullptr;
    }

    void execute_(char* line) {
        // Tokenize in place on spaces/tabs: cmd [arg1] [arg2].
        char* tok[3] = {nullptr, nullptr, nullptr};
        size_t ntok = 0;
        for (char* p = line; *p && ntok < 3;) {
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p) break;
            tok[ntok++] = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
            if (*p) *p++ = '\0';
        }
        if (ntok == 0) return;  // blank line — a terminal probing for life

        const char* cmd = tok[0];
        if (ieq_(cmd, "help") || ieq_(cmd, "?")) { print_help(); return; }
        if (ieq_(cmd, "cfg") || ieq_(cmd, "get")) { print_cfg(); return; }

        if (ieq_(cmd, "set")) {
            if (ntok < 3) {
                reply("[tune] err usage: set <knob> <value>");
                return;
            }
            const Knob* k = find_(tok[1]);
            if (!k) {
                reply("[tune] err unknown knob '%s' (try 'help')", tok[1]);
                return;
            }
            uint32_t v = 0;
            if (!parse_u32_(tok[2], &v)) {
                reply("[tune] err '%s' is not a whole number", tok[2]);
                return;
            }
            const uint32_t asked = v;
            if (v < k->lo) v = k->lo;
            if (v > k->hi) v = k->hi;
            const bool changed = k->set(v);
            if (changed) changed_ = true;
            if (asked != v) {
                reply("[tune] ok %s=%lu (clamped from %lu; range %lu..%lu)",
                      k->name, (unsigned long)v, (unsigned long)asked,
                      (unsigned long)k->lo, (unsigned long)k->hi);
            } else {
                reply("[tune] ok %s=%lu%s", k->name, (unsigned long)v,
                      changed ? "" : " (unchanged)");
            }
            print_cfg();
            return;
        }

        if (ieq_(cmd, "reset")) {
            bool any = false;
            for (size_t i = 0; i < count_; ++i) {
                if (knobs_[i].set(knobs_[i].def)) any = true;
            }
            if (any) changed_ = true;
            reply("[tune] ok defaults restored%s", any ? "" : " (already there)");
            print_cfg();
            return;
        }

        if (ieq_(cmd, "stream")) {
            if (ntok < 2) {
                reply("[tune] err usage: stream on|off|<ms>");
                return;
            }
            if (ieq_(tok[1], "off")) {
                stream_ms_ = 0;
                reply("[tune] ok stream off");
                return;
            }
            uint32_t ms = STREAM_DEFAULT_MS;
            if (!ieq_(tok[1], "on")) {
                if (!parse_u32_(tok[1], &ms)) {
                    reply("[tune] err usage: stream on|off|<ms>");
                    return;
                }
                if (ms < STREAM_MIN_MS) ms = STREAM_MIN_MS;
                if (ms > STREAM_MAX_MS) ms = STREAM_MAX_MS;
            }
            stream_ms_ = ms;
            reply("[tune] ok stream every %lu ms", (unsigned long)ms);
            return;
        }

        if (ieq_(cmd, "raw")) {
            if (ntok < 2 || (!ieq_(tok[1], "on") && !ieq_(tok[1], "off"))) {
                reply("[tune] err usage: raw on|off");
                return;
            }
            raw_ = ieq_(tok[1], "on");
            reply("[tune] ok raw %s", raw_ ? "on" : "off");
            return;
        }

        reply("[tune] err unknown command '%s' (try 'help')", cmd);
    }

    void writeln_(const char* line) const {
        if (!write_) return;
        write_(line);
        write_("\n");
    }

    void reply(const char* fmt, ...) const
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
    {
        char line[160];
        va_list args;
        va_start(args, fmt);
        vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        writeln_(line);
    }

    const Knob* knobs_ = nullptr;
    size_t      count_ = 0;
    WriteFn     write_ = nullptr;

    char   buf_[MAX_LINE];
    size_t len_ = 0;
    bool   overflow_ = false;

    uint32_t stream_ms_ = STREAM_DEFAULT_MS;
    bool     raw_ = false;
    bool     changed_ = false;
};

}  // namespace securacv::console
