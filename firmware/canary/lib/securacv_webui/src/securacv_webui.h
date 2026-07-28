/*
 * SecuraCV Canary — Web UI
 *
 * Dashboard HTML/CSS/JS as PROGMEM strings.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_WEBUI_H
#define SECURACV_WEBUI_H

#include <Arduino.h>

// Main dashboard HTML
extern const char CANARY_UI_HTML[] PROGMEM;

// First-boot setup wizard (captive-portal page) — small and CNA-safe, served
// on the OS connectivity-probe paths and /setup. See securacv_setup_page.cpp.
extern const char CANARY_SETUP_HTML[] PROGMEM;

#endif // SECURACV_WEBUI_H
