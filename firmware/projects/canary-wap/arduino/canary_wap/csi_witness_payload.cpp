/**
 * @file csi_witness_payload.cpp
 * @brief Implementation of csi_witness_build_payload — see header.
 */

#include "csi_witness_payload.h"

#include <stdio.h>

extern "C" int csi_witness_build_payload(char*       buf,
                                         size_t      buflen,
                                         const char* module_id,
                                         const char* type_name,
                                         uint8_t     category,
                                         const char* state_name,
                                         const char* confidence,
                                         uint8_t     motion_score,
                                         uint8_t     breathing_score,
                                         uint8_t     bpm,
                                         uint16_t    duration_sec,
                                         uint8_t     time_bucket,
                                         const char* kernel_version,
                                         const char* ruleset_id,
                                         const char* zone_id) {
  if (!buf || buflen == 0) return -1;

  const char* mod = (module_id  && module_id[0])  ? module_id  : "?";
  const char* typ = (type_name  && type_name[0])  ? type_name  : "?";
  const char* st  = (state_name && state_name[0]) ? state_name : "-";
  const char* cf  = (confidence && confidence[0]) ? confidence : "-";
  const char* kv  = (kernel_version && kernel_version[0]) ? kernel_version : "-";
  const char* rs  = (ruleset_id     && ruleset_id[0])     ? ruleset_id     : "-";
  const char* zn  = (zone_id        && zone_id[0])        ? zone_id        : "-";

  const int len = snprintf(buf, buflen,
    "csi %s %s %u %s %s m=%u b=%u bpm=%u d=%u bk=%u kv=%s rs=%s zn=%s",
    mod, typ,
    (unsigned)category,
    st, cf,
    (unsigned)motion_score,
    (unsigned)breathing_score,
    (unsigned)bpm,
    (unsigned)duration_sec,
    (unsigned)time_bucket,
    kv, rs, zn);

  if (len < 0 || (size_t)len >= buflen) return -1;
  return len;
}
