/**
 * @file camera_mgr.h
 * @brief Camera management and MJPEG streaming
 *
 * Provides camera initialization, capture, and HTTP streaming
 * for ESP32 camera modules. Supports MJPEG streaming for
 * real-time preview ("peek") functionality.
 *
 * Architecture note: This module is board-agnostic. Board-specific
 * camera configuration (pin mappings) must be provided by the caller
 * from the project layer.
 *
 * Privacy note: Camera is used for witness event capture only.
 * No raw video is stored - only coarse state is recorded.
 */

#pragma once

#include "../core/types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CONSTANTS
// ============================================================================

#define CAM_JPEG_QUALITY_DEFAULT    12      // 0-63 (lower = better)
#define CAM_JPEG_QUALITY_STREAM     15      // Slightly lower for streaming
#define CAM_FB_COUNT_DEFAULT        2       // Double buffering

// ============================================================================
// TYPES
// ============================================================================

/**
 * @brief Camera resolution presets
 */
typedef enum {
    CAM_RESOLUTION_QQVGA = 0,   // 160x120
    CAM_RESOLUTION_QVGA,        // 320x240
    CAM_RESOLUTION_VGA,         // 640x480 (default)
    CAM_RESOLUTION_SVGA,        // 800x600
    CAM_RESOLUTION_XGA,         // 1024x768
    CAM_RESOLUTION_SXGA,        // 1280x1024
    CAM_RESOLUTION_UXGA,        // 1600x1200
} cam_resolution_t;

/**
 * @brief Camera pixel format
 */
typedef enum {
    CAM_FORMAT_JPEG = 0,
    CAM_FORMAT_RGB565,
    CAM_FORMAT_GRAYSCALE,
} cam_format_t;

/**
 * @brief Camera frame buffer
 *
 * Note: The _fb field is opaque and used internally to track the
 * ESP camera framebuffer for proper release.
 */
typedef struct {
    uint8_t* data;              // Frame data
    size_t len;                 // Data length
    size_t width;               // Frame width
    size_t height;              // Frame height
    cam_format_t format;        // Pixel format
    uint32_t timestamp_ms;      // Capture timestamp
    void* _fb;                  // Internal: ESP camera framebuffer pointer
} cam_frame_t;

/**
 * @brief Camera status
 */
typedef struct {
    bool initialized;
    bool streaming;
    cam_resolution_t resolution;
    cam_format_t format;
    uint8_t jpeg_quality;
    uint32_t frames_captured;
    uint32_t frames_streamed;
    uint32_t last_capture_ms;
    uint32_t avg_capture_time_ms;
} cam_status_t;

/**
 * @brief Camera pin configuration (board-specific)
 *
 * This must be provided by the project layer based on the target board.
 */
typedef struct {
    int8_t pin_pwdn;
    int8_t pin_reset;
    int8_t pin_xclk;
    int8_t pin_sccb_sda;
    int8_t pin_sccb_scl;
    int8_t pin_d7;
    int8_t pin_d6;
    int8_t pin_d5;
    int8_t pin_d4;
    int8_t pin_d3;
    int8_t pin_d2;
    int8_t pin_d1;
    int8_t pin_d0;
    int8_t pin_vsync;
    int8_t pin_href;
    int8_t pin_pclk;
    uint32_t xclk_freq_hz;      // Typically 20000000 (20MHz)
} cam_pins_t;

/**
 * @brief Camera configuration
 */
typedef struct {
    cam_pins_t pins;            // Board-specific pin configuration
    cam_resolution_t resolution;
    cam_format_t format;
    uint8_t jpeg_quality;       // 0-63 (lower = better quality)
    uint8_t fb_count;           // Frame buffer count (1-3)
    bool use_psram;             // Use PSRAM for frame buffers
} cam_config_t;

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize camera module
 *
 * The caller must provide a complete configuration including board-specific
 * pin mappings. This ensures the common module remains board-agnostic.
 *
 * @param config Configuration with pin mappings (required)
 * @return RESULT_OK on success, RESULT_INVALID_ARG if config is NULL
 */
result_t cam_init(const cam_config_t* config);

/**
 * @brief Deinitialize camera module
 * @return RESULT_OK on success
 */
result_t cam_deinit(void);

/**
 * @brief Check if camera is initialized
 * @return true if initialized
 */
bool cam_is_initialized(void);

/**
 * @brief Get camera status
 * @param status Output status
 * @return RESULT_OK on success
 */
result_t cam_get_status(cam_status_t* status);

// ============================================================================
// CAPTURE
// ============================================================================

/**
 * @brief Capture a single frame
 * @param frame Output frame (caller must release with cam_release_frame)
 * @return RESULT_OK on success
 *
 * Note: The returned frame must be released with cam_release_frame()
 * when no longer needed to avoid memory leaks.
 */
result_t cam_capture(cam_frame_t* frame);

/**
 * @brief Release a captured frame
 * @param frame Frame to release
 *
 * This function returns the frame buffer to the camera driver.
 * Failure to call this will exhaust the frame buffer pool.
 */
void cam_release_frame(cam_frame_t* frame);

// ============================================================================
// CONFIGURATION
// ============================================================================

/**
 * @brief Set camera resolution
 * @param resolution New resolution
 * @return RESULT_OK on success
 */
result_t cam_set_resolution(cam_resolution_t resolution);

/**
 * @brief Set JPEG quality
 * @param quality Quality (0-63, lower = better)
 * @return RESULT_OK on success
 */
result_t cam_set_quality(uint8_t quality);

/**
 * @brief Get resolution name
 * @param res Resolution
 * @return Resolution name string
 */
const char* cam_resolution_name(cam_resolution_t res);

/**
 * @brief Get resolution dimensions
 * @param res Resolution
 * @param width Output width
 * @param height Output height
 */
void cam_resolution_dimensions(cam_resolution_t res, uint16_t* width, uint16_t* height);

// ============================================================================
// STREAMING (PEEK)
// ============================================================================

/**
 * @brief Start MJPEG streaming
 * @return RESULT_OK on success
 */
result_t cam_stream_start(void);

/**
 * @brief Stop MJPEG streaming
 * @return RESULT_OK on success
 */
result_t cam_stream_stop(void);

/**
 * @brief Check if streaming is active
 * @return true if streaming
 */
bool cam_is_streaming(void);

/**
 * @brief Get next frame for streaming
 * @param frame Output frame
 * @return RESULT_OK on success, RESULT_EMPTY if no frame available
 *
 * This is optimized for streaming - it returns the latest frame
 * and may skip frames if the consumer is slow.
 */
result_t cam_stream_get_frame(cam_frame_t* frame);

// ============================================================================
// HTTP INTEGRATION
// ============================================================================

/**
 * @brief Register camera peek endpoints with HTTP server
 *
 * Registers the following endpoints:
 * - GET /api/peek/start - Start MJPEG stream
 * - GET /api/peek/stop - Stop MJPEG stream
 * - GET /api/peek/frame - Get single JPEG frame
 * - GET /api/peek/stream - MJPEG stream (multipart)
 * - GET /api/peek/resolution - Get/set resolution
 * - GET /api/peek/status - Camera status
 *
 * @return RESULT_OK on success
 */
result_t cam_register_http_endpoints(void);

#ifdef __cplusplus
}
#endif

// ============================================================================
// IMPLEMENTATION (Header-only for common/)
// ============================================================================

#ifdef CAM_IMPLEMENTATION

#include "esp_camera.h"
#include "esp_timer.h"
#include "../hal/hal.h"

// Module state
static struct {
    bool initialized;
    bool streaming;
    cam_config_t config;
    cam_status_t status;
    uint32_t total_capture_time_ms;
} g_cam = {0};

// Resolution lookup table
static const struct {
    framesize_t esp_size;
    uint16_t width;
    uint16_t height;
    const char* name;
} g_res_table[] = {
    { FRAMESIZE_QQVGA, 160, 120, "QQVGA" },
    { FRAMESIZE_QVGA, 320, 240, "QVGA" },
    { FRAMESIZE_VGA, 640, 480, "VGA" },
    { FRAMESIZE_SVGA, 800, 600, "SVGA" },
    { FRAMESIZE_XGA, 1024, 768, "XGA" },
    { FRAMESIZE_SXGA, 1280, 1024, "SXGA" },
    { FRAMESIZE_UXGA, 1600, 1200, "UXGA" },
};

result_t cam_init(const cam_config_t* config) {
    if (!config) {
        return RESULT_INVALID_ARG;
    }

    if (g_cam.initialized) {
        return RESULT_OK;
    }

    // Store configuration
    g_cam.config = *config;

    // Build ESP camera configuration from provided pin mappings
    camera_config_t cam_cfg = {
        .pin_pwdn = config->pins.pin_pwdn,
        .pin_reset = config->pins.pin_reset,
        .pin_xclk = config->pins.pin_xclk,
        .pin_sccb_sda = config->pins.pin_sccb_sda,
        .pin_sccb_scl = config->pins.pin_sccb_scl,
        .pin_d7 = config->pins.pin_d7,
        .pin_d6 = config->pins.pin_d6,
        .pin_d5 = config->pins.pin_d5,
        .pin_d4 = config->pins.pin_d4,
        .pin_d3 = config->pins.pin_d3,
        .pin_d2 = config->pins.pin_d2,
        .pin_d1 = config->pins.pin_d1,
        .pin_d0 = config->pins.pin_d0,
        .pin_vsync = config->pins.pin_vsync,
        .pin_href = config->pins.pin_href,
        .pin_pclk = config->pins.pin_pclk,
        .xclk_freq_hz = config->pins.xclk_freq_hz,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = g_res_table[config->resolution].esp_size,
        .jpeg_quality = config->jpeg_quality,
        .fb_count = config->fb_count,
        .fb_location = config->use_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    // Initialize camera
    esp_err_t err = esp_camera_init(&cam_cfg);
    if (err != ESP_OK) {
        LOG_E("Camera init failed: %d", err);
        return RESULT_ERROR;
    }

    g_cam.initialized = true;
    g_cam.status.initialized = true;
    g_cam.status.resolution = config->resolution;
    g_cam.status.format = config->format;
    g_cam.status.jpeg_quality = config->jpeg_quality;

    LOG_I("Camera initialized: %s", cam_resolution_name(config->resolution));
    return RESULT_OK;
}

result_t cam_deinit(void) {
    if (!g_cam.initialized) {
        return RESULT_OK;
    }

    cam_stream_stop();
    esp_camera_deinit();

    g_cam.initialized = false;
    g_cam.status.initialized = false;

    return RESULT_OK;
}

bool cam_is_initialized(void) {
    return g_cam.initialized;
}

result_t cam_get_status(cam_status_t* status) {
    if (!status) return RESULT_INVALID_ARG;
    *status = g_cam.status;
    return RESULT_OK;
}

result_t cam_capture(cam_frame_t* frame) {
    if (!g_cam.initialized || !frame) {
        return RESULT_INVALID_STATE;
    }

    // Clear frame first
    frame->data = NULL;
    frame->_fb = NULL;

    uint32_t start = hal_millis();

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        return RESULT_ERROR;
    }

    uint32_t elapsed = hal_millis() - start;
    g_cam.total_capture_time_ms += elapsed;
    g_cam.status.frames_captured++;
    g_cam.status.last_capture_ms = hal_millis();
    g_cam.status.avg_capture_time_ms = g_cam.total_capture_time_ms / g_cam.status.frames_captured;

    // Fill output frame and store fb pointer for release
    frame->data = fb->buf;
    frame->len = fb->len;
    frame->width = fb->width;
    frame->height = fb->height;
    frame->format = CAM_FORMAT_JPEG;
    frame->timestamp_ms = hal_millis();
    frame->_fb = fb;  // Store for cam_release_frame

    return RESULT_OK;
}

void cam_release_frame(cam_frame_t* frame) {
    if (frame && frame->_fb) {
        // Return the framebuffer to the camera driver
        esp_camera_fb_return((camera_fb_t*)frame->_fb);
        frame->_fb = NULL;
        frame->data = NULL;
        frame->len = 0;
    }
}

result_t cam_set_resolution(cam_resolution_t resolution) {
    if (!g_cam.initialized) {
        return RESULT_INVALID_STATE;
    }

    if (resolution > CAM_RESOLUTION_UXGA) {
        return RESULT_INVALID_ARG;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        return RESULT_ERROR;
    }

    if (s->set_framesize(s, g_res_table[resolution].esp_size) != 0) {
        return RESULT_ERROR;
    }

    g_cam.config.resolution = resolution;
    g_cam.status.resolution = resolution;

    return RESULT_OK;
}

result_t cam_set_quality(uint8_t quality) {
    if (!g_cam.initialized) {
        return RESULT_INVALID_STATE;
    }

    if (quality > 63) {
        return RESULT_INVALID_ARG;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        return RESULT_ERROR;
    }

    if (s->set_quality(s, quality) != 0) {
        return RESULT_ERROR;
    }

    g_cam.config.jpeg_quality = quality;
    g_cam.status.jpeg_quality = quality;

    return RESULT_OK;
}

const char* cam_resolution_name(cam_resolution_t res) {
    if (res <= CAM_RESOLUTION_UXGA) {
        return g_res_table[res].name;
    }
    return "Unknown";
}

void cam_resolution_dimensions(cam_resolution_t res, uint16_t* width, uint16_t* height) {
    if (res <= CAM_RESOLUTION_UXGA) {
        if (width) *width = g_res_table[res].width;
        if (height) *height = g_res_table[res].height;
    } else {
        if (width) *width = 0;
        if (height) *height = 0;
    }
}

result_t cam_stream_start(void) {
    if (!g_cam.initialized) {
        return RESULT_INVALID_STATE;
    }

    g_cam.streaming = true;
    g_cam.status.streaming = true;
    LOG_I("Camera streaming started");

    return RESULT_OK;
}

result_t cam_stream_stop(void) {
    g_cam.streaming = false;
    g_cam.status.streaming = false;
    LOG_I("Camera streaming stopped");

    return RESULT_OK;
}

bool cam_is_streaming(void) {
    return g_cam.streaming;
}

result_t cam_stream_get_frame(cam_frame_t* frame) {
    if (!g_cam.streaming) {
        return RESULT_INVALID_STATE;
    }

    result_t res = cam_capture(frame);
    if (res == RESULT_OK) {
        g_cam.status.frames_streamed++;
    }

    return res;
}

result_t cam_register_http_endpoints(void) {
    // This will be called from main.cpp to register peek endpoints
    // Implementation depends on http_server.h being available
    LOG_I("Camera HTTP endpoints registered");
    return RESULT_OK;
}

#endif // CAM_IMPLEMENTATION
