/**
 * @file secrets.h
 * @brief Secret credentials for SecuraCV Canary WAP
 *
 * SECURITY NOTICE:
 * ================
 * 1. Copy this file to secrets.h
 * 2. Edit secrets.h with your actual credentials
 * 3. NEVER commit secrets.h to version control
 *
 * The secrets.h file is gitignored by default.
 */

#pragma once

// ============================================================================
// WiFi Station Credentials (for connecting to your home network)
// ============================================================================

#define WIFI_SSID           "your-wifi-ssid"
#define WIFI_PASSWORD       "your-wifi-password"

// ============================================================================
// WiFi Access Point Settings
// ============================================================================

// IMPORTANT: Change this password before deployment!
// The default password is for development only.
#define AP_PASSWORD_CUSTOM  "change-me-now"

// ============================================================================
// MQTT Credentials (optional, for cloud connectivity)
// ============================================================================

#define MQTT_BROKER         "192.168.1.100"
#define MQTT_PORT           1883
#define MQTT_USER           ""
#define MQTT_PASSWORD       ""

// ============================================================================
// API Tokens (optional)
// ============================================================================

#define API_TOKEN           ""
