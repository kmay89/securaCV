/**
 * @file securacv_scanner.c
 * @brief SecuraCV BLE Scanner — Flipper Zero Application
 *
 * Scans for SecuraCV Canary BLE advertisements and decodes debug beacons.
 * Displays device list with RSSI, and detailed health view for debug-mode devices.
 *
 * Target: Flipper Zero official firmware 1.x
 * BLE API: Bt service + furi_hal_bt GAP observer
 *
 * Controls:
 *   Up/Down    — scroll device list
 *   OK         — show device detail
 *   Left/Right — cycle sort (list) / toggle detail/graph
 *   Right(long)— open settings (scan list)
 *   Back       — return to list / exit app
 *   Back (long)— toggle proximity alerts (scan list)
 */

#include <furi.h>
#include <furi_hal_bt.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <bt/bt_service/bt.h>
#include <notification/notification_messages.h>

#include "securacv_protocol.h"

// ============================================================================
// BRANDING & VERSION
// ============================================================================

#define APP_NAME          "SecuraCV Canary"
#define APP_VERSION       "2.1.0"
#define APP_AUTHOR        "SecuraCV"

// ============================================================================
// CONSTANTS
// ============================================================================

#define MAX_DEVICES       16
#define SCREEN_WIDTH      128
#define SCREEN_HEIGHT     64
#define LINE_HEIGHT       10
#define MAX_VISIBLE       5
#define SPLASH_DURATION_MS 1500

#define AD_TYPE_SHORT_NAME        0x08
#define AD_TYPE_COMPLETE_NAME     0x09
#define AD_TYPE_MANUFACTURER_DATA 0xFF

// Custom event types for the message queue
typedef enum {
    AppEventTypeInput = 0,
    AppEventTypeBleDevice,
    AppEventTypeTick,
} AppEventType;

typedef struct {
    AppEventType type;
    union {
        InputEvent input;
        struct {
            char name[32];
            int8_t rssi;
            uint8_t mfg_data[SCV_PAYLOAD_LEN];
            uint8_t mfg_data_len;
            bool has_mfg_data;
        } ble;
    };
} AppEvent;

// ============================================================================
// APPLICATION STATE
// ============================================================================

typedef enum {
    VIEW_SPLASH,
    VIEW_SCAN_LIST,
    VIEW_DEVICE_DETAIL,
    VIEW_SIGNAL_GRAPH,
    VIEW_SETTINGS,
    VIEW_ABOUT,
} AppView;

typedef enum {
    SCAN_CONTINUOUS,
    SCAN_BALANCED,
    SCAN_LOW_POWER,
    SCAN_MODE_COUNT,
} ScanMode;

static const char* scan_mode_labels[] = {"Continuous", "Balanced", "Low Power"};
static const uint16_t scan_on_ms[]  = {0, 3000, 2000};
static const uint16_t scan_off_ms[] = {0, 3000, 8000};

typedef enum {
    SETTING_SCAN_MODE,
    SETTING_TIMEOUT,
    SETTING_SORT,
    SETTING_COUNT,
} SettingIndex;

static const uint16_t timeout_options[] = {10000, 15000, 30000, 60000};
static const char* timeout_labels[] = {"10s", "15s", "30s", "60s"};
#define TIMEOUT_OPTION_COUNT 4

typedef struct {
    FuriMutex* mutex;
    ViewPort* view_port;
    Gui* gui;
    Bt* bt;
    NotificationApp* notifications;
    FuriMessageQueue* event_queue;
    FuriTimer* tick_timer;
    bool running;
    bool bt_locked;

    // Device tracking
    scv_device_t devices[MAX_DEVICES];
    uint8_t device_count;

    // UI state
    AppView current_view;
    int8_t selected_index;
    int8_t scroll_offset;
    bool scanning;
    SortMode sort_mode;
    bool alerts_enabled;

    // Scan settings
    ScanMode scan_mode;
    uint8_t timeout_idx;
    uint32_t duty_cycle_ms;
    bool duty_scan_active;
    SettingIndex settings_cursor;

    // Splash & animation
    uint32_t splash_start_ms;
    uint8_t tick_count;
    uint32_t total_beacons;
} SecuraCVApp;

// ============================================================================
// BLE ADVERTISING DATA PARSER
// ============================================================================

static bool parse_ad_field(const uint8_t* ad_data, uint8_t ad_len,
                           uint8_t target_type,
                           const uint8_t** out_data, uint8_t* out_len) {
    uint8_t offset = 0;
    while(offset < ad_len) {
        uint8_t field_len = ad_data[offset];
        if(field_len == 0 || offset + field_len >= ad_len) break;

        uint8_t field_type = ad_data[offset + 1];
        if(field_type == target_type) {
            *out_data = &ad_data[offset + 2];
            *out_len = field_len - 1;
            return true;
        }
        offset += field_len + 1;
    }
    return false;
}

static void extract_name_from_ad(const uint8_t* ad_data, uint8_t ad_len,
                                  char* name_buf, size_t name_buf_len) {
    const uint8_t* field_data;
    uint8_t field_len;

    // Try complete name first, then short name
    if(parse_ad_field(ad_data, ad_len, AD_TYPE_COMPLETE_NAME, &field_data, &field_len) ||
       parse_ad_field(ad_data, ad_len, AD_TYPE_SHORT_NAME, &field_data, &field_len)) {
        size_t copy_len = field_len < name_buf_len - 1 ? field_len : name_buf_len - 1;
        memcpy(name_buf, field_data, copy_len);
        name_buf[copy_len] = '\0';
    }
}

static bool extract_manufacturer_data(const uint8_t* ad_data, uint8_t ad_len,
                                       uint8_t* mfg_buf, uint8_t mfg_buf_len,
                                       uint8_t* out_len) {
    const uint8_t* field_data;
    uint8_t field_len;

    if(parse_ad_field(ad_data, ad_len, AD_TYPE_MANUFACTURER_DATA, &field_data, &field_len)) {
        uint8_t copy_len = field_len < mfg_buf_len ? field_len : mfg_buf_len;
        memcpy(mfg_buf, field_data, copy_len);
        *out_len = copy_len;
        return true;
    }
    return false;
}

// ============================================================================
// DEVICE LIST MANAGEMENT
// ============================================================================

static scv_device_t* find_device_by_name(SecuraCVApp* app, const char* name) {
    for(uint8_t i = 0; i < app->device_count; i++) {
        if(strcmp(app->devices[i].name, name) == 0) {
            return &app->devices[i];
        }
    }
    return NULL;
}

static scv_device_t* add_or_update_device(SecuraCVApp* app, const AppEvent* evt) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    scv_device_t* dev = find_device_by_name(app, evt->ble.name);

    if(!dev) {
        if(app->device_count >= MAX_DEVICES) {
            // Evict oldest unpinned device; fall back to oldest overall
            // Uses subtraction-based age for 32-bit rollover safety
            uint32_t now_evict = furi_get_tick();
            uint32_t max_age = 0;
            uint8_t oldest_idx = 0;
            bool found_unpinned = false;
            for(uint8_t i = 0; i < app->device_count; i++) {
                if(!app->devices[i].pinned) {
                    uint32_t age = now_evict - app->devices[i].last_seen_ms;
                    if(!found_unpinned || age > max_age) {
                        max_age = age;
                        oldest_idx = i;
                        found_unpinned = true;
                    }
                }
            }
            if(!found_unpinned) {
                for(uint8_t i = 0; i < app->device_count; i++) {
                    uint32_t age = now_evict - app->devices[i].last_seen_ms;
                    if(i == 0 || age > max_age) {
                        max_age = age;
                        oldest_idx = i;
                    }
                }
            }
            dev = &app->devices[oldest_idx];
        } else {
            dev = &app->devices[app->device_count++];
        }
        memset(dev, 0, sizeof(*dev));
    }

    strncpy(dev->name, evt->ble.name, sizeof(dev->name) - 1);
    dev->rssi = evt->ble.rssi;
    dev->is_debug_mode = scv_is_debug_mode(evt->ble.name);
    dev->last_seen_ms = furi_get_tick();
    scv_rssi_push(dev, evt->ble.rssi);

    // Zone transition detection
    int8_t avg = dev->rssi_sample_count > 0 ? (int8_t)(dev->rssi_avg / 10) : dev->rssi;
    scv_proximity_zone_t new_zone = scv_classify_zone(avg);
    scv_proximity_zone_t old_zone = dev->zone;
    dev->zone = new_zone;

    if(evt->ble.has_mfg_data) {
        scv_debug_beacon_t parsed;
        if(scv_parse_debug_beacon(evt->ble.mfg_data, evt->ble.mfg_data_len, &parsed)) {
            dev->debug = parsed;
            dev->has_debug_data = true;
        }
    }

    furi_mutex_release(app->mutex);

    if(app->alerts_enabled && old_zone != SCV_ZONE_UNKNOWN && new_zone != old_zone) {
        if(new_zone == SCV_ZONE_NEAR) {
            notification_message(app->notifications, &sequence_single_vibro);
            notification_message(app->notifications, &sequence_blink_green_100);
        } else if(new_zone == SCV_ZONE_LOST ||
                  (new_zone > old_zone)) {
            notification_message(app->notifications, &sequence_double_vibro);
            notification_message(app->notifications, &sequence_blink_red_100);
        }
    }

    return dev;
}

static int cmp_rssi(const void* a, const void* b) {
    const scv_device_t* da = (const scv_device_t*)a;
    const scv_device_t* db = (const scv_device_t*)b;
    if(da->pinned != db->pinned) return db->pinned - da->pinned;
    int8_t ra = da->rssi_sample_count > 0 ? (int8_t)(da->rssi_avg / 10) : da->rssi;
    int8_t rb = db->rssi_sample_count > 0 ? (int8_t)(db->rssi_avg / 10) : db->rssi;
    return rb - ra;
}

static int cmp_name(const void* a, const void* b) {
    const scv_device_t* da = (const scv_device_t*)a;
    const scv_device_t* db = (const scv_device_t*)b;
    if(da->pinned != db->pinned) return db->pinned - da->pinned;
    return strcmp(da->name, db->name);
}

static int cmp_last_seen(const void* a, const void* b) {
    const scv_device_t* da = (const scv_device_t*)a;
    const scv_device_t* db = (const scv_device_t*)b;
    if(da->pinned != db->pinned) return db->pinned - da->pinned;
    if(db->last_seen_ms > da->last_seen_ms) return 1;
    if(db->last_seen_ms < da->last_seen_ms) return -1;
    return 0;
}

static void sort_devices(SecuraCVApp* app) {
    if(app->device_count < 2) return;
    typedef int (*cmp_fn)(const void*, const void*);
    cmp_fn fns[] = {cmp_rssi, cmp_name, cmp_last_seen};
    qsort(app->devices, app->device_count, sizeof(scv_device_t), fns[app->sort_mode]);
}

static void expire_stale_devices(SecuraCVApp* app) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    uint32_t now = furi_get_tick();
    uint8_t i = 0;
    while(i < app->device_count) {
        uint32_t timeout = timeout_options[app->timeout_idx];
        if(!app->devices[i].pinned &&
           now - app->devices[i].last_seen_ms > timeout) {
            if(i < app->selected_index) {
                app->selected_index--;
            } else if(i == app->selected_index) {
                if(app->current_view != VIEW_SCAN_LIST) {
                    app->current_view = VIEW_SCAN_LIST;
                }
            }
            if(i < app->scroll_offset && app->scroll_offset > 0) {
                app->scroll_offset--;
            }

            for(uint8_t j = i; j < app->device_count - 1; j++) {
                app->devices[j] = app->devices[j + 1];
            }
            app->device_count--;

            if(app->selected_index >= app->device_count && app->device_count > 0) {
                app->selected_index = app->device_count - 1;
            }
            if(app->selected_index < 0) app->selected_index = 0;
        } else {
            i++;
        }
    }

    furi_mutex_release(app->mutex);
}

// ============================================================================
// BLE OBSERVER CALLBACK
// ============================================================================

static void ble_observer_callback(const uint8_t* ad_data, uint8_t ad_len,
                                   int8_t rssi, void* context) {
    SecuraCVApp* app = (SecuraCVApp*)context;
    if(!app || !ad_data || ad_len == 0) return;

    // Extract device name from advertising data
    char name[32] = {0};
    extract_name_from_ad(ad_data, ad_len, name, sizeof(name));

    // Filter: only process SecuraCV devices
    if(!scv_is_securacv_device(name)) return;

    // Build event to send to main loop
    AppEvent event = {.type = AppEventTypeBleDevice};
    strncpy(event.ble.name, name, sizeof(event.ble.name) - 1);
    event.ble.rssi = rssi;
    event.ble.has_mfg_data = extract_manufacturer_data(
        ad_data, ad_len,
        event.ble.mfg_data, sizeof(event.ble.mfg_data),
        &event.ble.mfg_data_len);

    // Non-blocking put — drop if queue is full (transient scan data)
    furi_message_queue_put(app->event_queue, &event, 0);
}

// ============================================================================
// TICK TIMER CALLBACK
// ============================================================================

static void tick_timer_callback(void* context) {
    SecuraCVApp* app = (SecuraCVApp*)context;
    AppEvent event = {.type = AppEventTypeTick};
    furi_message_queue_put(app->event_queue, &event, 0);
}

// ============================================================================
// SIGNAL STRENGTH BAR
// ============================================================================

static void draw_rssi_bar(Canvas* canvas, int x, int y, int8_t rssi) {
    int bars = 0;
    if(rssi > -50) bars = 4;
    else if(rssi > -65) bars = 3;
    else if(rssi > -80) bars = 2;
    else if(rssi > -95) bars = 1;

    for(int i = 0; i < 4; i++) {
        int bar_h = 2 + i * 2;
        int bar_x = x + i * 3;
        int bar_y = y + 8 - bar_h;
        if(i < bars) {
            canvas_draw_box(canvas, bar_x, bar_y, 2, bar_h);
        } else {
            canvas_draw_frame(canvas, bar_x, bar_y, 2, bar_h);
        }
    }
}

// ============================================================================
// DRAW: SPLASH SCREEN
// ============================================================================

static void draw_splash(Canvas* canvas, SecuraCVApp* app) {
    UNUSED(app);

    canvas_draw_frame(canvas, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    canvas_draw_frame(canvas, 1, 1, SCREEN_WIDTH - 2, SCREEN_HEIGHT - 2);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, 18, AlignCenter, AlignCenter, APP_NAME);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, 30, AlignCenter, AlignCenter,
                            "BLE Scanner");

    canvas_draw_line(canvas, 30, 36, 98, 36);

    canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, 44, AlignCenter, AlignCenter,
                            "v" APP_VERSION);

    canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, 58, AlignCenter, AlignCenter,
                            "Initializing BLE...");
}

// ============================================================================
// DRAW: ABOUT SCREEN
// ============================================================================

static void draw_about(Canvas* canvas, SecuraCVApp* app) {
    canvas_draw_frame(canvas, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, 10, AlignCenter, AlignBottom, APP_NAME);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, 20, AlignCenter, AlignBottom,
                            "v" APP_VERSION "  " APP_AUTHOR);

    canvas_draw_line(canvas, 8, 23, 120, 23);

    int y = 32;
    canvas_draw_str(canvas, 4, y, "U/D Scroll  L/R Sort");
    y += 9;
    canvas_draw_str(canvas, 4, y, "OK  Detail  >Hold Cfg");
    y += 9;
    canvas_draw_str(canvas, 4, y, "Bk  Exit    BkHold Alt");

    char stats[32];
    snprintf(stats, sizeof(stats), "%lu beacons  %d devs",
             (unsigned long)app->total_beacons, app->device_count);
    canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, SCREEN_HEIGHT - 2,
                            AlignCenter, AlignBottom, stats);
}

// ============================================================================
// DRAW: SCAN STATUS INDICATOR
// ============================================================================

static void draw_scan_indicator(Canvas* canvas, bool scanning, uint8_t tick) {
    int x = SCREEN_WIDTH - 4;
    int y = 13;
    if(scanning) {
        uint8_t phase = tick % 4;
        int r = 1 + phase;
        canvas_draw_disc(canvas, x, y, r > 3 ? 3 : r);
    } else {
        canvas_draw_frame(canvas, x - 1, y - 1, 3, 3);
    }
}

// ============================================================================
// DRAW: SCAN LIST
// ============================================================================

static void draw_scan_list(Canvas* canvas, SecuraCVApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, APP_NAME);

    canvas_set_font(canvas, FontSecondary);
    char status[32];
    snprintf(status, sizeof(status), "%d <%s>%s",
             app->device_count, sort_mode_labels[app->sort_mode],
             app->alerts_enabled ? " ALT" : "");
    canvas_draw_str_aligned(canvas, SCREEN_WIDTH, 10, AlignRight, AlignBottom, status);

    canvas_draw_line(canvas, 0, 13, SCREEN_WIDTH, 13);
    draw_scan_indicator(canvas, app->scanning, app->tick_count);

    if(app->device_count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, 30,
                                AlignCenter, AlignCenter,
                                app->scanning ? "Scanning..." : "Paused");
        if(app->scanning) {
            canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, 42,
                                    AlignCenter, AlignCenter,
                                    "Looking for SCV-* BLE");
        }
        canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, SCREEN_HEIGHT - 2,
                                AlignCenter, AlignBottom,
                                ">Hold:Cfg  BkHold:Alt");
        return;
    }

    int y_start = 16;
    for(int i = 0; i < MAX_VISIBLE && (i + app->scroll_offset) < app->device_count; i++) {
        int idx = i + app->scroll_offset;
        scv_device_t* dev = &app->devices[idx];
        int y = y_start + i * LINE_HEIGHT;

        if(idx == app->selected_index) {
            canvas_draw_box(canvas, 0, y, SCREEN_WIDTH, LINE_HEIGHT);
            canvas_set_color(canvas, ColorWhite);
        }

        canvas_set_font(canvas, FontSecondary);

        int name_x = 2;
        if(dev->pinned) {
            canvas_draw_str(canvas, 2, y + 8, "*");
            name_x = 8;
        }

        char name_buf[16];
        snprintf(name_buf, sizeof(name_buf), "%.14s", dev->name);
        canvas_draw_str(canvas, name_x, y + 8, name_buf);

        int label_x = 68;
        if(dev->is_debug_mode) {
            canvas_draw_str(canvas, label_x, y + 8, "DBG");
            label_x += 18;
        }
        if(app->alerts_enabled && dev->zone != SCV_ZONE_UNKNOWN) {
            canvas_draw_str(canvas, label_x, y + 8, scv_zone_label(dev->zone));
        }

        int8_t display_rssi = dev->rssi_sample_count > 0
            ? (int8_t)(dev->rssi_avg / 10)
            : dev->rssi;
        draw_rssi_bar(canvas, SCREEN_WIDTH - 14, y + 1, display_rssi);

        char rssi_buf[8];
        snprintf(rssi_buf, sizeof(rssi_buf), "%d", display_rssi);
        canvas_draw_str(canvas, SCREEN_WIDTH - 30, y + 8, rssi_buf);

        if(idx == app->selected_index) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    if(app->scroll_offset > 0) {
        canvas_draw_str_aligned(canvas, SCREEN_WIDTH - 2, 15, AlignRight, AlignTop, "^");
    }
    if(app->scroll_offset + MAX_VISIBLE < app->device_count) {
        canvas_draw_str_aligned(canvas, SCREEN_WIDTH - 2, SCREEN_HEIGHT - 1,
                                AlignRight, AlignBottom, "v");
    }
}

// ============================================================================
// DRAW: DEVICE DETAIL
// ============================================================================

static void draw_device_detail(Canvas* canvas, SecuraCVApp* app) {
    if(app->selected_index < 0 || app->selected_index >= app->device_count) return;
    scv_device_t* dev = &app->devices[app->selected_index];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, dev->name);
    int8_t hdr_rssi = dev->rssi_sample_count > 0
        ? (int8_t)(dev->rssi_avg / 10) : dev->rssi;
    draw_rssi_bar(canvas, SCREEN_WIDTH - 14, 2, hdr_rssi);
    canvas_draw_line(canvas, 0, 13, SCREEN_WIDTH, 13);

    canvas_set_font(canvas, FontSecondary);

    if(!dev->has_debug_data || !dev->debug.valid) {
        canvas_draw_str(canvas, 2, 26, "No debug beacon data");
        canvas_draw_str(canvas, 2, 38, "Hold BOOT 3s on Canary");

        char sig_line[40];
        int8_t avg = dev->rssi_sample_count > 0 ? (int8_t)(dev->rssi_avg / 10) : dev->rssi;
        int16_t quality_input = dev->rssi_sample_count > 0
            ? dev->rssi_avg : (int16_t)(dev->rssi * 10);
        snprintf(sig_line, sizeof(sig_line), "RSSI: %d dBm  %s",
                 avg, scv_signal_quality(quality_input));
        canvas_draw_str(canvas, 2, 50, sig_line);

        canvas_draw_str_aligned(canvas, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 2,
                                AlignRight, AlignBottom, "Graph>");
        if(dev->rssi_sample_count > 1) {
            char range_line[32];
            uint16_t dm = scv_estimate_distance_dm(dev->rssi_avg);
            char dist_buf[8];
            scv_format_distance(dm, dist_buf, sizeof(dist_buf));
            snprintf(range_line, sizeof(range_line), "%d/%d ~%s",
                     dev->rssi_min, dev->rssi_max, dist_buf);
            canvas_draw_str(canvas, 2, SCREEN_HEIGHT - 2, range_line);
        }
        return;
    }

    scv_debug_beacon_t* d = &dev->debug;
    char line[40];
    int y = 24;

    snprintf(line, sizeof(line), "W%c B%c M%c C%c G%c S%c K%c",
             (d->subsystem_flags & SCV_FLAG_WIFI)   ? '+' : '-',
             (d->subsystem_flags & SCV_FLAG_BLE)    ? '+' : '-',
             (d->subsystem_flags & SCV_FLAG_MESH)   ? '+' : '-',
             (d->subsystem_flags & SCV_FLAG_CHIRP)  ? '+' : '-',
             (d->subsystem_flags & SCV_FLAG_GPS)    ? '+' : '-',
             (d->subsystem_flags & SCV_FLAG_SD)     ? '+' : '-',
             (d->subsystem_flags & SCV_FLAG_CRYPTO) ? '+' : '-');
    canvas_draw_str(canvas, 2, y, line);
    y += LINE_HEIGHT;

    snprintf(line, sizeof(line), "Mesh:%d peers  RF:%d devs",
             d->mesh_peers, d->rf_device_count);
    canvas_draw_str(canvas, 2, y, line);
    y += LINE_HEIGHT;

    char uptime_buf[16];
    scv_format_uptime(d->uptime_sec, uptime_buf, sizeof(uptime_buf));
    snprintf(line, sizeof(line), "Heap:%dKB  Up:%s", d->free_heap_kb, uptime_buf);
    canvas_draw_str(canvas, 2, y, line);
    y += LINE_HEIGHT;

    snprintf(line, sizeof(line), "Chain:%d V:%s E:%s",
             d->chain_height,
             scv_verify_name(d->chain_verify),
             scv_error_name(d->error_code));
    canvas_draw_str(canvas, 2, y, line);

    if(d->subsystem_flags & SCV_FLAG_TAMPER) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, SCREEN_HEIGHT - 2,
                                AlignCenter, AlignBottom, "!! TAMPER !!");
    } else if(dev->rssi_sample_count > 0) {
        uint16_t dm = scv_estimate_distance_dm(dev->rssi_avg);
        char dist_buf[8];
        scv_format_distance(dm, dist_buf, sizeof(dist_buf));
        snprintf(line, sizeof(line), "%ddBm %s ~%s",
                 (int)(dev->rssi_avg / 10),
                 scv_signal_quality(dev->rssi_avg), dist_buf);
        canvas_draw_str(canvas, 2, SCREEN_HEIGHT - 2, line);
        canvas_draw_str_aligned(canvas, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 2,
                                AlignRight, AlignBottom, "Graph>");
    }
}

// ============================================================================
// DRAW: SIGNAL GRAPH
// ============================================================================

#define GRAPH_X       0
#define GRAPH_Y       15
#define GRAPH_W       128
#define GRAPH_H       36
#define RSSI_FLOOR    (-100)
#define RSSI_CEIL     (-30)

static void draw_signal_graph(Canvas* canvas, SecuraCVApp* app) {
    if(app->selected_index < 0 || app->selected_index >= app->device_count) return;
    scv_device_t* dev = &app->devices[app->selected_index];

    // Header
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, dev->name);
    int8_t hdr_rssi = dev->rssi_sample_count > 0
        ? (int8_t)(dev->rssi_avg / 10) : dev->rssi;
    draw_rssi_bar(canvas, SCREEN_WIDTH - 14, 2, hdr_rssi);
    canvas_draw_line(canvas, 0, 13, SCREEN_WIDTH, 13);

    // Graph frame
    canvas_draw_frame(canvas, GRAPH_X, GRAPH_Y, GRAPH_W, GRAPH_H);

    // Y-axis labels (left margin)
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 1, GRAPH_Y + 7, "-30");
    canvas_draw_str(canvas, 1, GRAPH_Y + GRAPH_H - 1, "-100");

    if(dev->rssi_graph_count == 0) {
        canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, GRAPH_Y + GRAPH_H / 2,
                                AlignCenter, AlignCenter, "No data");
        return;
    }

    // Draw bars after label margin, each 2px wide, right-aligned
    int graph_inner_x = GRAPH_X + 25;
    int graph_inner_w = GRAPH_W - 26;
    int graph_inner_h = GRAPH_H - 2;
    int max_bars = graph_inner_w / 2;
    int bars_to_draw = dev->rssi_graph_count < max_bars
        ? dev->rssi_graph_count : max_bars;

    int rssi_range = RSSI_CEIL - RSSI_FLOOR;

    for(int i = 0; i < bars_to_draw; i++) {
        int sample_idx = (int)dev->rssi_graph_idx - bars_to_draw + i;
        if(sample_idx < 0) sample_idx += SCV_GRAPH_LEN;

        int8_t val = dev->rssi_graph[sample_idx];
        if(val < RSSI_FLOOR) val = RSSI_FLOOR;
        if(val > RSSI_CEIL) val = RSSI_CEIL;

        int bar_h = ((val - RSSI_FLOOR) * graph_inner_h) / rssi_range;
        if(bar_h < 1) bar_h = 1;

        int bar_x = graph_inner_x + graph_inner_w - (bars_to_draw - i) * 2;
        int bar_y = GRAPH_Y + 1 + graph_inner_h - bar_h;

        canvas_draw_box(canvas, bar_x, bar_y, 2, bar_h);
    }

    // Footer: avg RSSI, quality, distance
    canvas_set_font(canvas, FontSecondary);
    char footer[40];
    uint16_t dm = scv_estimate_distance_dm(dev->rssi_avg);
    char dist_buf[8];
    scv_format_distance(dm, dist_buf, sizeof(dist_buf));
    snprintf(footer, sizeof(footer), "%ddBm %s ~%s",
             (int)(dev->rssi_avg / 10),
             scv_signal_quality(dev->rssi_avg),
             dist_buf);
    canvas_draw_str(canvas, 2, SCREEN_HEIGHT - 2, footer);

    // Navigation hint
    canvas_draw_str_aligned(canvas, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 2,
                            AlignRight, AlignBottom, "<Info");
}

// ============================================================================
// DRAW: SETTINGS
// ============================================================================

static void draw_settings(Canvas* canvas, SecuraCVApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Canary Settings");
    canvas_draw_line(canvas, 0, 13, SCREEN_WIDTH, 13);

    canvas_set_font(canvas, FontSecondary);
    int y = 26;

    // Scan Mode
    if(app->settings_cursor == SETTING_SCAN_MODE) {
        canvas_draw_box(canvas, 0, y - 8, SCREEN_WIDTH, LINE_HEIGHT);
        canvas_set_color(canvas, ColorWhite);
    }
    char line[40];
    snprintf(line, sizeof(line), "Scan: <%s>", scan_mode_labels[app->scan_mode]);
    canvas_draw_str(canvas, 2, y, line);
    if(app->settings_cursor == SETTING_SCAN_MODE) {
        canvas_set_color(canvas, ColorBlack);
    }
    y += LINE_HEIGHT + 2;

    // Device Timeout
    if(app->settings_cursor == SETTING_TIMEOUT) {
        canvas_draw_box(canvas, 0, y - 8, SCREEN_WIDTH, LINE_HEIGHT);
        canvas_set_color(canvas, ColorWhite);
    }
    snprintf(line, sizeof(line), "Timeout: <%s>", timeout_labels[app->timeout_idx]);
    canvas_draw_str(canvas, 2, y, line);
    if(app->settings_cursor == SETTING_TIMEOUT) {
        canvas_set_color(canvas, ColorBlack);
    }
    y += LINE_HEIGHT + 2;

    // Sort Mode
    if(app->settings_cursor == SETTING_SORT) {
        canvas_draw_box(canvas, 0, y - 8, SCREEN_WIDTH, LINE_HEIGHT);
        canvas_set_color(canvas, ColorWhite);
    }
    snprintf(line, sizeof(line), "Sort: <%s>", sort_mode_labels[app->sort_mode]);
    canvas_draw_str(canvas, 2, y, line);
    if(app->settings_cursor == SETTING_SORT) {
        canvas_set_color(canvas, ColorBlack);
    }

    y += LINE_HEIGHT + 4;
    canvas_draw_line(canvas, 8, y - 6, 120, y - 6);
    canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, y,
                            AlignCenter, AlignCenter, "OK: About  Bk: Back");
}

// ============================================================================
// DRAW CALLBACK
// ============================================================================

static void render_callback(Canvas* canvas, void* ctx) {
    SecuraCVApp* app = (SecuraCVApp*)ctx;

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);

    switch(app->current_view) {
        case VIEW_SPLASH:
            draw_splash(canvas, app);
            break;
        case VIEW_SCAN_LIST:
            draw_scan_list(canvas, app);
            break;
        case VIEW_DEVICE_DETAIL:
            draw_device_detail(canvas, app);
            break;
        case VIEW_SIGNAL_GRAPH:
            draw_signal_graph(canvas, app);
            break;
        case VIEW_SETTINGS:
            draw_settings(canvas, app);
            break;
        case VIEW_ABOUT:
            draw_about(canvas, app);
            break;
    }

    furi_mutex_release(app->mutex);
}

// ============================================================================
// INPUT CALLBACK
// ============================================================================

static void input_callback(InputEvent* input_event, void* ctx) {
    SecuraCVApp* app = (SecuraCVApp*)ctx;
    AppEvent event = {.type = AppEventTypeInput, .input = *input_event};
    furi_message_queue_put(app->event_queue, &event, 0);
}

// ============================================================================
// INPUT HANDLING
// ============================================================================

static void handle_input(SecuraCVApp* app, InputEvent* event) {
    if(event->type == InputTypeLong && event->key == InputKeyOk &&
       app->current_view == VIEW_SCAN_LIST) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(app->device_count > 0 && app->selected_index < app->device_count) {
            app->devices[app->selected_index].pinned =
                !app->devices[app->selected_index].pinned;
        }
        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
        return;
    }

    if(event->type == InputTypeLong && event->key == InputKeyBack &&
       app->current_view == VIEW_SCAN_LIST) {
        app->alerts_enabled = !app->alerts_enabled;
        if(app->alerts_enabled) {
            notification_message(app->notifications, &sequence_single_vibro);
        }
        view_port_update(app->view_port);
        return;
    }

    if(event->type == InputTypeLong && event->key == InputKeyRight &&
       app->current_view == VIEW_SCAN_LIST) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->current_view = VIEW_SETTINGS;
        app->settings_cursor = SETTING_SCAN_MODE;
        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
        return;
    }

    if(event->type != InputTypePress && event->type != InputTypeRepeat) return;

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    switch(app->current_view) {
        case VIEW_SCAN_LIST:
            switch(event->key) {
                case InputKeyUp:
                    if(app->selected_index > 0) {
                        app->selected_index--;
                        if(app->selected_index < app->scroll_offset) {
                            app->scroll_offset = app->selected_index;
                        }
                    }
                    break;
                case InputKeyDown:
                    if(app->device_count > 0 && app->selected_index < app->device_count - 1) {
                        app->selected_index++;
                        if(app->selected_index >= app->scroll_offset + MAX_VISIBLE) {
                            app->scroll_offset = app->selected_index - MAX_VISIBLE + 1;
                        }
                    }
                    break;
                case InputKeyOk:
                    if(app->device_count > 0) {
                        app->current_view = VIEW_DEVICE_DETAIL;
                    }
                    break;
                case InputKeyLeft:
                    app->sort_mode = (app->sort_mode + SortModeCount - 1) % SortModeCount;
                    sort_devices(app);
                    break;
                case InputKeyRight:
                    app->sort_mode = (app->sort_mode + 1) % SortModeCount;
                    sort_devices(app);
                    break;
                case InputKeyBack:
                    app->running = false;
                    break;
                default:
                    break;
            }
            break;

        case VIEW_DEVICE_DETAIL:
            if(event->key == InputKeyBack) {
                app->current_view = VIEW_SCAN_LIST;
            } else if(event->key == InputKeyLeft || event->key == InputKeyRight) {
                app->current_view = VIEW_SIGNAL_GRAPH;
            }
            break;

        case VIEW_SIGNAL_GRAPH:
            if(event->key == InputKeyBack) {
                app->current_view = VIEW_SCAN_LIST;
            } else if(event->key == InputKeyLeft || event->key == InputKeyRight) {
                app->current_view = VIEW_DEVICE_DETAIL;
            }
            break;

        case VIEW_SETTINGS:
            switch(event->key) {
                case InputKeyUp:
                    if(app->settings_cursor > 0) app->settings_cursor--;
                    break;
                case InputKeyDown:
                    if(app->settings_cursor < SETTING_COUNT - 1) app->settings_cursor++;
                    break;
                case InputKeyLeft:
                case InputKeyRight: {
                    int dir = (event->key == InputKeyRight) ? 1 : -1;
                    if(app->settings_cursor == SETTING_SCAN_MODE) {
                        app->scan_mode = (app->scan_mode + SCAN_MODE_COUNT + dir) % SCAN_MODE_COUNT;
                        app->duty_cycle_ms = 0;
                        app->duty_scan_active = true;
                    } else if(app->settings_cursor == SETTING_TIMEOUT) {
                        app->timeout_idx = (app->timeout_idx + TIMEOUT_OPTION_COUNT + dir) % TIMEOUT_OPTION_COUNT;
                    } else if(app->settings_cursor == SETTING_SORT) {
                        app->sort_mode = (app->sort_mode + SortModeCount + dir) % SortModeCount;
                    }
                    break;
                }
                case InputKeyOk:
                    app->current_view = VIEW_ABOUT;
                    break;
                case InputKeyBack:
                    app->current_view = VIEW_SCAN_LIST;
                    break;
                default:
                    break;
            }
            break;

        case VIEW_ABOUT:
            if(event->key == InputKeyBack || event->key == InputKeyOk) {
                app->current_view = VIEW_SCAN_LIST;
            }
            break;

        case VIEW_SPLASH:
            if(event->key == InputKeyOk || event->key == InputKeyBack) {
                app->current_view = VIEW_SCAN_LIST;
            }
            break;
    }

    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
}

// ============================================================================
// BLE LIFECYCLE
// ============================================================================

static bool ble_scanner_start(SecuraCVApp* app) {
    app->bt = furi_record_open(RECORD_BT);
    if(!app->bt) {
        app->scanning = false;
        return false;
    }

    bt_disconnect(app->bt);
    furi_delay_ms(200);

    // Start GAP observer to receive advertising reports
    furi_hal_bt_start_observer(ble_observer_callback, app);

    app->scanning = true;
    app->bt_locked = true;
    return true;
}

static void ble_scanner_stop(SecuraCVApp* app) {
    if(app->bt_locked) {
        furi_hal_bt_stop_observer();
        app->scanning = false;
        app->bt_locked = false;
    }

    if(app->bt) {
        // Reconnect phone app
        bt_start_advertising(app->bt);
        furi_record_close(RECORD_BT);
        app->bt = NULL;
    }
}

// ============================================================================
// DUTY CYCLE MANAGEMENT
// ============================================================================

static void duty_cycle_update(SecuraCVApp* app) {
    if(app->scan_mode == SCAN_CONTINUOUS) {
        if(!app->scanning && app->bt) {
            furi_hal_bt_start_observer(ble_observer_callback, app);
            app->scanning = true;
            app->bt_locked = true;
        }
        return;
    }

    uint16_t on_ms = scan_on_ms[app->scan_mode];
    uint16_t off_ms = scan_off_ms[app->scan_mode];
    uint32_t cycle_total = on_ms + off_ms;

    bool should_scan = app->duty_cycle_ms < on_ms;

    app->duty_cycle_ms += 1000;
    if(app->duty_cycle_ms >= cycle_total) {
        app->duty_cycle_ms = 0;
    }

    if(should_scan && !app->scanning && app->bt) {
        furi_hal_bt_start_observer(ble_observer_callback, app);
        app->scanning = true;
        app->bt_locked = true;
    } else if(!should_scan && app->scanning) {
        furi_hal_bt_stop_observer();
        app->scanning = false;
        app->bt_locked = false;
    }
}

// ============================================================================
// APP ENTRY POINT
// ============================================================================

int32_t securacv_scanner_app(void* p) {
    UNUSED(p);

    SecuraCVApp* app = malloc(sizeof(SecuraCVApp));
    if(!app) return -1;
    memset(app, 0, sizeof(*app));

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!app->mutex) {
        free(app);
        return -1;
    }

    app->event_queue = furi_message_queue_alloc(64, sizeof(AppEvent));
    if(!app->event_queue) {
        furi_mutex_free(app->mutex);
        free(app);
        return -1;
    }

    app->running = true;
    app->current_view = VIEW_SPLASH;
    app->splash_start_ms = furi_get_tick();
    app->selected_index = 0;
    app->scroll_offset = 0;
    app->timeout_idx = 1;
    app->duty_scan_active = true;
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    // Set up GUI
    app->view_port = view_port_alloc();
    if(!app->view_port) {
        furi_message_queue_free(app->event_queue);
        furi_mutex_free(app->mutex);
        free(app);
        return -1;
    }
    view_port_draw_callback_set(app->view_port, render_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    if(!app->gui) {
        view_port_free(app->view_port);
        furi_message_queue_free(app->event_queue);
        furi_mutex_free(app->mutex);
        free(app);
        return -1;
    }
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    // Periodic tick for device expiry and UI refresh
    app->tick_timer = furi_timer_alloc(tick_timer_callback, FuriTimerTypePeriodic, app);
    if(!app->tick_timer) {
        gui_remove_view_port(app->gui, app->view_port);
        furi_record_close(RECORD_GUI);
        view_port_free(app->view_port);
        furi_message_queue_free(app->event_queue);
        furi_mutex_free(app->mutex);
        free(app);
        return -1;
    }
    furi_timer_start(app->tick_timer, 1000);

    // Start BLE scanning
    ble_scanner_start(app);
    view_port_update(app->view_port);

    // Main event loop
    AppEvent event;
    while(app->running) {
        if(furi_message_queue_get(app->event_queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        switch(event.type) {
            case AppEventTypeInput:
                handle_input(app, &event.input);
                break;

            case AppEventTypeBleDevice:
                add_or_update_device(app, &event);
                app->total_beacons++;
                view_port_update(app->view_port);
                break;

            case AppEventTypeTick:
                app->tick_count++;
                if(app->current_view == VIEW_SPLASH &&
                   furi_get_tick() - app->splash_start_ms >= SPLASH_DURATION_MS) {
                    app->current_view = VIEW_SCAN_LIST;
                }
                expire_stale_devices(app);
                duty_cycle_update(app);
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                sort_devices(app);
                furi_mutex_release(app->mutex);
                view_port_update(app->view_port);
                break;
        }
    }

    // Cleanup
    furi_timer_stop(app->tick_timer);
    furi_timer_free(app->tick_timer);
    ble_scanner_stop(app);
    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    view_port_free(app->view_port);
    furi_message_queue_free(app->event_queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
