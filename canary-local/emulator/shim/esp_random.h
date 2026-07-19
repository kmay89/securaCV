// canary-local/emulator/shim/esp_random.h — entropy from the page.
// Commissioning tokens still deserve real randomness: the JS side seeds
// from crypto.getRandomValues; a scenario can pin the seed for
// reproducible walkthrough screenshots.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
uint32_t esp_random(void);
void esp_fill_random(void* buf, size_t len);
#ifdef __cplusplus
}
#endif
