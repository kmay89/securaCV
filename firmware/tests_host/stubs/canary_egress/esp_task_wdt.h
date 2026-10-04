/* esp_task_wdt.h for the canary events-egress host build: the event log's
 * retention rewrite feeds the watchdog; here a feed does nothing. */
#ifndef STUB_CANARY_EGRESS_ESP_TASK_WDT_H
#define STUB_CANARY_EGRESS_ESP_TASK_WDT_H
inline int esp_task_wdt_reset() { return 0; }
#endif
