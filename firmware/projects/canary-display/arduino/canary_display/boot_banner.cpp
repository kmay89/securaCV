#include "boot_banner.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static boot_write_fn_t s_write = NULL;

void boot_set_output(boot_write_fn_t fn) { s_write = fn; }

static void out(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (s_write) { s_write(buf); } else { printf("%s", buf); }
}

// ============================================================================
// FORMATTING HELPERS
// ============================================================================

void boot_separator(void) {
    out("    ------------------------------------------------\n");
}

void boot_blank(void) { out("\n"); }

void boot_line(const char* text) { out("%s\n", text); }

void boot_linef(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    if (len < 0) len = 0;
    else if (len >= (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 2;
    buf[len] = '\n';
    buf[len + 1] = '\0';
    if (s_write) { s_write(buf); } else { printf("%s", buf); }
}

void boot_kv(const char* key, const char* value) {
    out("    %-12s%s\n", key, value ? value : "--");
}

void boot_kvf(const char* key, const char* fmt, ...) {
    char val[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(val, sizeof(val), fmt, args);
    va_end(args);
    out("    %-12s%s\n", key, val);
}

void boot_feature(bool enabled, const char* name, const char* desc) {
    if (enabled)
        out("    + %-14s%s\n", name, desc ? desc : "");
    else
        out("    - %s\n", name);
}

void boot_note(const char* text) {
    out("                %s\n", text);
}

// ============================================================================
// CORE SCENES
// ============================================================================

void boot_scene_banner(const boot_info_t* info) {
    if (!info) return;
    out("\n\n");
    out("              ,_,          Waking up...\n");
    out("             (o.o)\n");
    out("             /| |\\         %s\n", info->product_name ? info->product_name : "--");
    out("              d b          v%s\n", info->fw_version ? info->fw_version : "--");
    boot_blank();
    out("    This is your privacy witness device.\n");
    out("    It creates tamper-proof records of what it\n");
    out("    sees, so nobody can change the story later.\n");
    boot_blank();
    boot_kv("Type",  info->device_type);
    boot_kv("Model", info->model);
    boot_kvf("Built", "%s %s", info->build_date ? info->build_date : "--", info->build_time ? info->build_time : "--");
    if (info->mac_address)
        boot_kv("MAC", info->mac_address);
    boot_separator();
    boot_blank();
}

void boot_scene_hardware(const boot_info_t* info) {
    if (!info) return;
    out("              ,_,\n");
    out("             (o.o) ?       Checking the hardware...\n");
    out("             (  >)\n");
    out("              \" \"          What am I running on?\n");
    boot_separator();

    boot_kv("Board", info->board_name);
    boot_kvf("Chip",  "%s rev %u", info->chip_model ? info->chip_model : "--", (unsigned)info->chip_revision);
    boot_kvf("CPU",   "%u MHz, %u core(s)", (unsigned)info->cpu_freq_mhz, (unsigned)info->cpu_cores);
    boot_note("(the brain — higher MHz = faster thinking)");
    boot_kvf("Flash", "%u MB", (unsigned)info->flash_mb);
    boot_note("(permanent storage, like a hard drive)");

    if (info->psram_found) {
        boot_kvf("PSRAM", "%u KB total, %u KB free",
                 (unsigned)info->psram_total_kb, (unsigned)info->psram_free_kb);
        boot_note("(extra memory for big tasks like camera)");
    } else {
        boot_kv("PSRAM", "not found");
    }

    boot_kvf("Heap", "%u KB free at boot", (unsigned)info->heap_free_kb);
    boot_note("(working memory — like a desk to work on)");
    boot_kv("SDK", info->sdk_version);
    boot_note("(the software toolkit this firmware uses)");
    boot_blank();
}

void boot_scene_ready(const char* msg1, const char* msg2, const char* msg3) {
    boot_blank();
    out("    ================================================\n");
    boot_blank();
    out("                   ,_,\n");
    out("                  (o.o)  ~~\n");
    out("                 /(> <)\\ ~~~~\n");
    out("                  d | b  ~~~~~~\n");
    boot_blank();
    out("    The canary is singing. Everything is working.\n");
    boot_blank();
    if (msg1) out("    %s\n", msg1);
    if (msg2) out("    %s\n", msg2);
    if (msg3) out("    %s\n", msg3);
    boot_blank();
    out("    ================================================\n");
    boot_blank();
}
