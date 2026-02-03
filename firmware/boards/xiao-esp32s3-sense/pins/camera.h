/**
 * @file camera.h
 * @brief Camera-specific pin configuration for XIAO ESP32-S3 Sense
 *
 * Provides the camera_config_t structure initialization for the OV2640
 * camera module built into the XIAO ESP32S3 Sense board.
 */

#pragma once

#include "pins.h"

#ifdef ESP_CAMERA_SUPPORTED

#include "esp_camera.h"

/**
 * @brief Get camera configuration for this board
 * @return camera_config_t initialized with board-specific pins
 *
 * Usage:
 *   camera_config_t config = xiao_esp32s3_camera_config();
 *   esp_err_t err = esp_camera_init(&config);
 */
static inline camera_config_t xiao_esp32s3_camera_config() {
    camera_config_t config;

    // Pin assignments
    config.pin_pwdn = CAM_PIN_PWDN;
    config.pin_reset = CAM_PIN_RESET;
    config.pin_xclk = CAM_PIN_XCLK;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;

    config.pin_d7 = CAM_PIN_D7;
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d5 = CAM_PIN_D5;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d0 = CAM_PIN_D0;

    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href = CAM_PIN_HREF;
    config.pin_pclk = CAM_PIN_PCLK;

    // Clock configuration
    config.xclk_freq_hz = 20000000;  // 20MHz XCLK
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;

    // Frame configuration (defaults)
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_VGA;  // 640x480
    config.jpeg_quality = 12;           // 0-63 (lower = better quality)
    config.fb_count = 2;                // Double buffering
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    return config;
}

/**
 * @brief Camera resolution presets for this board
 */
typedef enum {
    CAM_RES_QQVGA = FRAMESIZE_QQVGA,   // 160x120
    CAM_RES_QVGA  = FRAMESIZE_QVGA,    // 320x240
    CAM_RES_VGA   = FRAMESIZE_VGA,     // 640x480
    CAM_RES_SVGA  = FRAMESIZE_SVGA,    // 800x600
    CAM_RES_XGA   = FRAMESIZE_XGA,     // 1024x768
    CAM_RES_SXGA  = FRAMESIZE_SXGA,    // 1280x1024
    CAM_RES_UXGA  = FRAMESIZE_UXGA,    // 1600x1200 (max)
} xiao_cam_resolution_t;

#endif // ESP_CAMERA_SUPPORTED
