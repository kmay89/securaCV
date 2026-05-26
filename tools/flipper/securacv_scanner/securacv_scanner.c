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
 *   Up/Down — scroll device list
 *   OK      — show device detail (if debug beacon available)
 *   Back    — return to list / exit app
 */

#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <bt/bt_service/bt.h>

#include "securacv_protocol.h"

// ============================================================================
// CONSTANTS
// ============================================================================

#define MAX_DEVICES       16
#define DEVICE_TIMEOUT_MS 15000
#define SCREEN_WIDTH      128
#define SCREEN_HEIGHT     64
#define LINE_HEIGHT       10
#define MAX_VISIBLE       5

// BLE advertising data type codes (Bluetooth Core Spec)
#define AD_TYPE_FLAGS              0x01
#define AD_TYPE_INCOMPLETE_16     0x02
#define AD_TYPE_COMPLETE_16       0x03
#define AD_TYPE_INCOMPLETE_128    0x06
#define AD_TYPE_COMPLETE_128      0x07
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
    VIEW_SCAN_LIST,
    VIEW_DEVICE_DETAIL,
} AppView;

typedef struct {
    FuriMutex* mutex;
    ViewPort* view_port;
    Gui* gui;
    Bt* bt;
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
            // Evict oldest device
            uint32_t oldest_ms = UINT32_MAX;
            uint8_t oldest_idx = 0;
            for(uint8_t i = 0; i < app->device_count; i++) {
                if(app->devices[i].last_seen_ms < oldest_ms) {
                    oldest_ms = app->devices[i].last_seen_ms;
                    oldest_idx = i;
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

    if(evt->ble.has_mfg_data) {
        scv_debug_beacon_t parsed;
        if(scv_parse_debug_beacon(evt->ble.mfg_data, evt->ble.mfg_data_len, &parsed)) {
            dev->debug = parsed;
            dev->has_debug_data = true;
        }
    }

    furi_mutex_release(app->mutex);
    return dev;
}

static void expire_stale_devices(SecuraCVApp* app) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    uint32_t now = furi_get_tick();
    uint8_t i = 0;
    while(i < app->device_count) {
        if(now - app->devices[i].last_seen_ms > DEVICE_TIMEOUT_MS) {
            // Shift remaining devices down
            for(uint8_t j = i; j < app->device_count - 1; j++) {
                app->devices[j] = app->devices[j + 1];
            }
            app->device_count--;

            // Fix selection index
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
// DRAW: SCAN LIST
// ============================================================================

static void draw_scan_list(Canvas* canvas, SecuraCVApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "SecuraCV Scanner");

    canvas_set_font(canvas, FontSecondary);
    char status[32];
    snprintf(status, sizeof(status), "%d device%s found",
             app->device_count, app->device_count == 1 ? "" : "s");
    canvas_draw_str_aligned(canvas, SCREEN_WIDTH, 10, AlignRight, AlignBottom, status);

    canvas_draw_line(canvas, 0, 13, SCREEN_WIDTH, 13);

    if(app->device_count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, 34,
                                AlignCenter, AlignCenter,
                                app->scanning ? "Scanning..." : "No devices");
        if(app->scanning) {
            canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, 46,
                                    AlignCenter, AlignCenter,
                                    "Looking for SCV-* BLE");
        }
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

        char name_buf[20];
        snprintf(name_buf, sizeof(name_buf), "%.19s", dev->name);
        canvas_draw_str(canvas, 2, y + 8, name_buf);

        if(dev->is_debug_mode) {
            canvas_draw_str(canvas, 80, y + 8, "DBG");
        }

        draw_rssi_bar(canvas, SCREEN_WIDTH - 14, y + 1, dev->rssi);

        char rssi_buf[8];
        snprintf(rssi_buf, sizeof(rssi_buf), "%d", dev->rssi);
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
    draw_rssi_bar(canvas, SCREEN_WIDTH - 14, 2, dev->rssi);
    canvas_draw_line(canvas, 0, 13, SCREEN_WIDTH, 13);

    canvas_set_font(canvas, FontSecondary);

    if(!dev->has_debug_data || !dev->debug.valid) {
        canvas_draw_str(canvas, 2, 26, "No debug beacon data");
        canvas_draw_str(canvas, 2, 38, "Activate debug mode on");
        canvas_draw_str(canvas, 2, 48, "the Canary (hold BOOT 3s)");

        char rssi_line[32];
        snprintf(rssi_line, sizeof(rssi_line), "RSSI: %d dBm", dev->rssi);
        canvas_draw_str(canvas, 2, SCREEN_HEIGHT - 2, rssi_line);
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
    }
}

// ============================================================================
// DRAW CALLBACK
// ============================================================================

static void render_callback(Canvas* canvas, void* ctx) {
    SecuraCVApp* app = (SecuraCVApp*)ctx;

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);

    switch(app->current_view) {
        case VIEW_SCAN_LIST:
            draw_scan_list(canvas, app);
            break;
        case VIEW_DEVICE_DETAIL:
            draw_device_detail(canvas, app);
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

    // Request exclusive BLE access — disconnects phone app
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

    app->event_queue = furi_message_queue_alloc(16, sizeof(AppEvent));
    if(!app->event_queue) {
        furi_mutex_free(app->mutex);
        free(app);
        return -1;
    }

    app->running = true;
    app->current_view = VIEW_SCAN_LIST;
    app->selected_index = 0;
    app->scroll_offset = 0;

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
    furi_timer_start(app->tick_timer, 3000);

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
                view_port_update(app->view_port);
                break;

            case AppEventTypeTick:
                expire_stale_devices(app);
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
    view_port_free(app->view_port);
    furi_message_queue_free(app->event_queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
