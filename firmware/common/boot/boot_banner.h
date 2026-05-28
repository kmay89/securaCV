/**
 * @file boot_banner.h
 * @brief Shared boot banner for all SecuraCV canary devices
 *
 * Provides ASCII-art canary scenes and formatting helpers that each
 * firmware project can compose into a friendly serial boot sequence.
 * Output goes through printf by default; call boot_set_output() to
 * redirect (e.g. on ESP32-C3 where Serial0 is needed).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// DEVICE INFO (caller fills this before calling scenes)
// ============================================================================

typedef struct {
    const char* product_name;   // "SecuraCV Canary WAP"
    const char* fw_version;     // "2.0.1"
    const char* build_date;
    const char* build_time;
    const char* device_type;    // "canary-wap"
    const char* model;          // "XIAO ESP32S3 Sense"
    const char* mac_address;    // "AA:BB:CC:DD:EE:FF"

    const char* board_name;
    const char* chip_model;
    uint8_t  chip_revision;
    uint16_t cpu_freq_mhz;
    uint8_t  cpu_cores;
    uint32_t flash_mb;
    bool     psram_found;
    uint32_t psram_total_kb;
    uint32_t psram_free_kb;
    uint32_t heap_free_kb;
    const char* sdk_version;
} boot_info_t;

// ============================================================================
// CORE SCENES — ASCII canary art with device context
// ============================================================================

void boot_scene_banner(const boot_info_t* info);
void boot_scene_hardware(const boot_info_t* info);
void boot_scene_ready(const char* msg1, const char* msg2, const char* msg3);

// ============================================================================
// FORMATTING HELPERS — for building custom per-project scenes
// ============================================================================

void boot_separator(void);
void boot_blank(void);
void boot_line(const char* text);
void boot_linef(const char* fmt, ...);
void boot_kv(const char* key, const char* value);
void boot_kvf(const char* key, const char* fmt, ...);
void boot_feature(bool enabled, const char* name, const char* desc);
void boot_note(const char* text);

// ============================================================================
// OUTPUT CONFIGURATION
// ============================================================================

typedef void (*boot_write_fn_t)(const char* line);

/**
 * Redirect boot output. The function receives one fully-formatted line
 * at a time (with trailing newline). Pass NULL to reset to printf.
 */
void boot_set_output(boot_write_fn_t fn);

#ifdef __cplusplus
}
#endif
