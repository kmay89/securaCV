/* esp_task_wdt.h for the fake-SD host build: esp_task_wdt_reset() counts
 * feeds, so a test can see the boot reload keep the watchdog fed on a big
 * tail (test_csi_event_log_load.cpp). */
#ifndef STUB_SD_FAKE_ESP_TASK_WDT_H
#define STUB_SD_FAKE_ESP_TASK_WDT_H

inline unsigned& stub_wdt_feeds() {
  static unsigned n = 0;
  return n;
}

inline int esp_task_wdt_reset() {
  stub_wdt_feeds()++;
  return 0;
}

#endif
