/* Stand-in for the canary's securacv_witness.h: what the event egress and
 * the SD event log adapter take from it, the device identity (its key and
 * fingerprint sign the events body) and log_health(). The real header pulls
 * in the witness chain. Same declarations as the product's; the test defines
 * them. */
#ifndef STUB_CANARY_EGRESS_SECURACV_WITNESS_H
#define STUB_CANARY_EGRESS_SECURACV_WITNESS_H
#include <stdint.h>
#include "log_level.h"
struct DeviceIdentity {
  uint8_t privkey[32];
  uint8_t pubkey[32];
  uint8_t pubkey_fp[8];
  char    device_id[32];
};
DeviceIdentity& witness_get_device();
void log_health(LogLevel level, LogCategory category, const char* message,
                const char* detail = nullptr);
#endif
