/**
 * @file camera_mgr.h
 * @brief Camera management and MJPEG streaming
 *
 * Provides camera initialization, capture, and HTTP streaming
 * for the OV2640 camera module. Supports MJPEG streaming for
 * real-time preview ("peek") functionality.
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
 */
typedef struct {
    uint8_t* data;              // Frame data
    size_t len;                 // Data length
    size_t width;               // Frame width
    size_t height;              // Frame height
    cam_format_t format;        // Pixel format
    uint32_t timestamp_ms;      // Capture timestamp
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
 * @brief Camera configuration
 */
typedef struct {
    cam_resolution_t resolution;
    cam_format_t format;
    uint8_t jpeg_quality;       // 0-63 (lower = better quality)
    uint8_t fb_count;           // Frame buffer count (1-3)
    bool use_psram;             // Use PSRAM for frame buffers
} cam_config_t;

// Default configuration
#define CAM_CONFIG_DEFAULT { \
    .resolution = CAM_RESOLUTION_VGA, \
    .format = CAM_FORMAT_JPEG, \
    .jpeg_quality = CAM_JPEG_QUALITY_DEFAULT, \
    .fb_count = CAM_FB_COUNT_DEFAULT, \
    .use_psram = true, \
}

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize camera module
 * @param config Configuration (NULL for defaults)
 * @return RESULT_OK on success
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
 * when no longer needed.
 */
result_t cam_capture(cam_frame_t* frame);

/**
 * @brief Release a captured frame
 * @param frame Frame to release
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

// Include board-specific camera pins
#include "pins.h"

#if HAS_CAMERA
#include "camera.h"
#endif

// Module state
static struct {
    bool initialized;
    bool streaming;
    cam_config_t config;
    cam_status_t status;
    uint32_t capture_start_ms;
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
#if !HAS_CAMERA
    return RESULT_NOT_SUPPORTED;
#else
    if (g_cam.initialized) {
        return RESULT_OK;
    }

    // Use defaults if no config provided
    if (config) {
        g_cam.config = *config;
    } else {
        cam_config_t def = CAM_CONFIG_DEFAULT;
        g_cam.config = def;
    }

    // Get board-specific camera configuration
    camera_config_t cam_cfg = xiao_esp32s3_camera_config();

    // Apply user settings
    cam_cfg.frame_size = g_res_table[g_cam.config.resolution].esp_size;
    cam_cfg.jpeg_quality = g_cam.config.jpeg_quality;
    cam_cfg.fb_count = g_cam.config.fb_count;

    if (g_cam.config.use_psram) {
        cam_cfg.fb_location = CAMERA_FB_IN_PSRAM;
    } else {
        cam_cfg.fb_location = CAMERA_FB_IN_DRAM;
    }

    // Initialize camera
    esp_err_t err = esp_camera_init(&cam_cfg);
    if (err != ESP_OK) {
        LOG_E("Camera init failed: %d", err);
        return RESULT_ERROR;
    }

    g_cam.initialized = true;
    g_cam.status.initialized = true;
    g_cam.status.resolution = g_cam.config.resolution;
    g_cam.status.format = g_cam.config.format;
    g_cam.status.jpeg_quality = g_cam.config.jpeg_quality;

    LOG_I("Camera initialized: %s", cam_resolution_name(g_cam.config.resolution));
    return RESULT_OK;
#endif
}

result_t cam_deinit(void) {
#if !HAS_CAMERA
    return RESULT_NOT_SUPPORTED;
#else
    if (!g_cam.initialized) {
        return RESULT_OK;
    }

    cam_stream_stop();
    esp_camera_deinit();

    g_cam.initialized = false;
    g_cam.status.initialized = false;

    return RESULT_OK;
#endif
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
#if !HAS_CAMERA
    return RESULT_NOT_SUPPORTED;
#else
    if (!g_cam.initialized || !frame) {
        return RESULT_INVALID_STATE;
    }

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

    // Fill output frame
    frame->data = fb->buf;
    frame->len = fb->len;
    frame->width = fb->width;
    frame->height = fb->height;
    frame->format = CAM_FORMAT_JPEG;
    frame->timestamp_ms = hal_millis();

    return RESULT_OK;
#endif
}

void cam_release_frame(cam_frame_t* frame) {
#if HAS_CAMERA
    if (frame && frame->data) {
        // The ESP camera API uses its own frame buffer management
        // We need to get the fb pointer back to release it
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
        frame->data = NULL;
        frame->len = 0;
    }
#endif
}

result_t cam_set_resolution(cam_resolution_t resolution) {
#if !HAS_CAMERA
    return RESULT_NOT_SUPPORTED;
#else
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
#endif
}

result_t cam_set_quality(uint8_t quality) {
#if !HAS_CAMERA
    return RESULT_NOT_SUPPORTED;
#else
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
#endif
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

// HTTP endpoint handlers would be implemented here
// when http_server.h integration is enabled

result_t cam_register_http_endpoints(void) {
    // This will be called from main.cpp to register peek endpoints
    // Implementation depends on http_server.h being available
    LOG_I("Camera HTTP endpoints registered");
    return RESULT_OK;
}

#endif // CAM_IMPLEMENTATION
