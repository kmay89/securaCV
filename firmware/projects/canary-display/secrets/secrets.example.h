#pragma once
/*
  secrets.example.h
  -----------------
  Copy to: secrets.h

  DO NOT COMMIT secrets.h
  This repo ignores it by default.
*/

// WiFi
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASS "your_wifi_password"

// MQTT broker
#define MQTT_HOST "192.168.1.10"
#define MQTT_PORT 1883

// Optional MQTT auth (use nullptr if your broker allows anonymous)
#define MQTT_USER "securacv"
#define MQTT_PASS "your_mqtt_password"

// Local timezone for the clock and quiet-hours night mode (POSIX TZ
// string). SET THIS unless you are on US Eastern. The compiled default is
// America/New_York, and the auto-learner below is compiled OUT unless you
// opt in, so a display that is never told its zone keeps that zone forever
// — outside it the clock reads hours wrong and, because quiet hours are
// wall-clock hours, the face can sit in night mode through the morning. You can also set it at runtime
// from the display's own web page (Timezone), which persists to NVS and
// wins over this value; this is the seed a freshly flashed unit boots on.
// The explicit flag stops the auto-learner from ever overriding your
// choice. Examples:
//   US Eastern:  "EST5EDT,M3.2.0,M11.1.0"
//   US Pacific:  "PST8PDT,M3.2.0,M11.1.0"
//   Central EU:  "CET-1CEST,M3.5.0,M10.5.0/3"
// Left commented deliberately — a guessed zone is a clock that lies
// convincingly, which is worse than one that is obviously unset. Pick
// yours and uncomment both lines.
// #define CD_TZ "EST5EDT,M3.2.0,M11.1.0"
// #define CD_TZ_EXPLICIT 1
//
// Optional: let the display learn its timezone from the internet instead
// of setting it above. PRIVACY NOTE — this sends ONE request to a
// geolocation service, which necessarily reveals your public IP (that's
// how it knows your zone). The project's default is that nothing leaves
// the home unasked, so this is OFF unless you opt in here:
// #define CD_TZ_WEB_LOOKUP 1


// Optional: where this home is (nightstand wave). Enables on-device
// sunrise/sunset lines — computed on the glass, never sent anywhere.
// Leave unset to skip sun lines entirely.
// #define CD_LAT 40.71
// #define CD_LON -74.01

// Optional: emergency contact shown during an unacknowledged alert
// (dash footer). Personal data, so it lives here with the credentials.
// #define CD_EMERGENCY_CONTACT "call Sam 555-0100"
