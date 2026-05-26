/**
 * @file securacv_scanner.c
 * @brief SecuraCV BLE Scanner — Flipper Zero Application
 *
 * Scans for SecuraCV Canary BLE advertisements and decodes debug beacons.
 * Displays device list with RSSI, and detailed health view for debug-mode devices.
 *
 * Controls:
 *   Up/Down — scroll device list
 *   OK      — show device detail (if debug beacon available)
 *   Back    — return to list / exit app
 */

#include <furi.h>
#include <furi_hal_bt.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <extra_profiles/ble_app_extra_beacon.h>
#include <bt/bt_service/bt.h>
#include <gap.h>

#include "securacv_protocol.h"

// ============================================================================
// CONSTANTS
// ============================================================================

#define MAX_DEVICES       16
#define SCAN_INTERVAL_MS  3000
#define DEVICE_TIMEOUT_MS 15000
#define SCREEN_WIDTH      128
#define SCREEN_HEIGHT     64
#define LINE_HEIGHT       10
#define MAX_VISIBLE       5

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
    FuriMessageQueue* event_queue;
    bool running;

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
// SIGNAL STRENGTH BAR
// ============================================================================

static void draw_rssi_bar(Canvas* canvas, int x, int y, int8_t rssi) {
    // Map RSSI to 0-4 bars
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

    // Status line
    canvas_set_font(canvas, FontSecondary);
    char status[32];
    snprintf(status, sizeof(status), "%d device%s found",
             app->device_count, app->device_count == 1 ? "" : "s");
    canvas_draw_str_aligned(canvas, SCREEN_WIDTH, 10, AlignRight, AlignBottom, status);

    canvas_draw_line(canvas, 0, 13, SCREEN_WIDTH, 13);

    if(app->device_count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, SCREEN_WIDTH / 2, 38,
                                AlignCenter, AlignCenter,
                                app->scanning ? "Scanning..." : "No devices");
        return;
    }

    // Device list
    int y_start = 16;
    for(int i = 0; i < MAX_VISIBLE && (i + app->scroll_offset) < app->device_count; i++) {
        int idx = i + app->scroll_offset;
        scv_device_t* dev = &app->devices[idx];
        int y = y_start + i * LINE_HEIGHT;

        // Highlight selected
        if(idx == app->selected_index) {
            canvas_draw_box(canvas, 0, y, SCREEN_WIDTH, LINE_HEIGHT);
            canvas_set_color(canvas, ColorWhite);
        }

        canvas_set_font(canvas, FontSecondary);

        // Device name (truncated, precision limits read length)
        char name_buf[20];
        snprintf(name_buf, sizeof(name_buf), "%.19s", dev->name);
        canvas_draw_str(canvas, 2, y + 8, name_buf);

        // Debug indicator
        if(dev->is_debug_mode) {
            canvas_draw_str(canvas, 80, y + 8, "DBG");
        }

        // RSSI bar
        draw_rssi_bar(canvas, SCREEN_WIDTH - 14, y + 1, dev->rssi);

        // RSSI number
        char rssi_buf[8];
        snprintf(rssi_buf, sizeof(rssi_buf), "%d", dev->rssi);
        canvas_draw_str(canvas, SCREEN_WIDTH - 30, y + 8, rssi_buf);

        if(idx == app->selected_index) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    // Scroll indicators
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

    // Header
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, dev->name);
    draw_rssi_bar(canvas, SCREEN_WIDTH - 14, 2, dev->rssi);
    canvas_draw_line(canvas, 0, 13, SCREEN_WIDTH, 13);

    canvas_set_font(canvas, FontSecondary);

    if(!dev->has_debug_data || !dev->debug.valid) {
        canvas_draw_str(canvas, 2, 26, "No debug beacon data");
        canvas_draw_str(canvas, 2, 38, "Activate debug mode on");
        canvas_draw_str(canvas, 2, 48, "the Canary (hold BOOT 3s)");
        return;
    }

    scv_debug_beacon_t* d = &dev->debug;
    char line[40];
    int y = 24;

    // Subsystem flags as compact indicators
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

    // Mesh and RF
    snprintf(line, sizeof(line), "Mesh:%d peers  RF:%d devs",
             d->mesh_peers, d->rf_device_count);
    canvas_draw_str(canvas, 2, y, line);
    y += LINE_HEIGHT;

    // Heap and uptime
    char uptime_buf[16];
    scv_format_uptime(d->uptime_sec, uptime_buf, sizeof(uptime_buf));
    snprintf(line, sizeof(line), "Heap:%dKB  Up:%s", d->free_heap_kb, uptime_buf);
    canvas_draw_str(canvas, 2, y, line);
    y += LINE_HEIGHT;

    // Chain and error
    snprintf(line, sizeof(line), "Chain:%d V:%s E:%s",
             d->chain_height,
             scv_verify_name(d->chain_verify),
             scv_error_name(d->error_code));
    canvas_draw_str(canvas, 2, y, line);

    // Tamper warning
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
    furi_message_queue_put(app->event_queue, input_event, 0);
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
// BLE SCAN SIMULATION STUB
// ============================================================================

/*
 * NOTE: Full BLE scanning requires the Flipper Zero BLE GAP observer API.
 * On firmware versions that support it, replace this stub with:
 *
 *   furi_hal_bt_start_scan(scan_callback, app);
 *
 * The scan_callback receives GapScanResult with name, RSSI, and
 * manufacturer data. Filter for SecuraCV devices using
 * scv_is_securacv_device() and parse debug beacons with
 * scv_parse_debug_beacon().
 *
 * For firmware versions without the observer API, use the Extra Beacon
 * profile or BLE serial profile scanning capabilities.
 *
 * See: https://github.com/flipperdevices/flipperzero-firmware/blob/dev/
 *      applications/services/bt/bt_service/bt.h
 *
 * The app compiles and runs the UI regardless — it just won't discover
 * devices until connected to the actual BLE scan API for your firmware
 * version.
 */

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

    app->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    if(!app->event_queue) {
        furi_mutex_free(app->mutex);
        free(app);
        return -1;
    }

    app->running = true;
    app->current_view = VIEW_SCAN_LIST;
    app->selected_index = 0;
    app->scroll_offset = 0;
    app->scanning = true;

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

    // Main event loop — view_port_update is called by handle_input on
    // state changes; no need to redraw every iteration.
    InputEvent event;
    while(app->running) {
        if(furi_message_queue_get(app->event_queue, &event, 100) == FuriStatusOk) {
            handle_input(app, &event);
        }
    }

    // Cleanup
    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);
    furi_message_queue_free(app->event_queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
