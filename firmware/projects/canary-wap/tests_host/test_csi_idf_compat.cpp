// Host test for csi_idf_compat.h — compiles the ONE config-fill routine
// against every wifi_csi_config_t shape ESP-IDF has shipped, so a member
// rename upstream fails a g++ run here instead of a customer's board.
//
// The Makefile builds this file four times, one shape each:
//   -DCSI_TEST_SHAPE_LEGACY44   ESP32/S3/C3 on IDF 4.4 (arduino-esp32 2.0.x)
//   -DCSI_TEST_SHAPE_LEGACY5X   same parts on IDF 5.x (+ dump_ack_en)
//   -DCSI_TEST_SHAPE_HE51       C6 on IDF 5.1 (acquire_csi_he_stbc, 2-bit scale)
//   -DCSI_TEST_SHAPE_HE55       C6/C5/C61 on IDF 5.5 MAC v3 (force_lltf, vht,
//                               he_stbc_mode, 4-bit scale)
// Each shape is pre-filled with 0xFF so the test also proves the routine
// zeroes what it does not name.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(CSI_TEST_SHAPE_LEGACY44)
typedef struct {
  bool    lltf_en;
  bool    htltf_en;
  bool    stbc_htltf2_en;
  bool    ltf_merge_en;
  bool    channel_filter_en;
  bool    manu_scale;
  uint8_t shift;
} wifi_csi_config_t;
static const char* SHAPE = "legacy IDF 4.4";
#elif defined(CSI_TEST_SHAPE_LEGACY5X)
typedef struct {
  bool    lltf_en;
  bool    htltf_en;
  bool    stbc_htltf2_en;
  bool    ltf_merge_en;
  bool    channel_filter_en;
  bool    manu_scale;
  uint8_t shift;
  bool    dump_ack_en;
} wifi_csi_config_t;
static const char* SHAPE = "legacy IDF 5.x";
#elif defined(CSI_TEST_SHAPE_HE51)
#define CONFIG_SOC_WIFI_HE_SUPPORT 1
typedef struct {
  uint32_t enable : 1;
  uint32_t acquire_csi_legacy : 1;
  uint32_t acquire_csi_ht20 : 1;
  uint32_t acquire_csi_ht40 : 1;
  uint32_t acquire_csi_su : 1;
  uint32_t acquire_csi_mu : 1;
  uint32_t acquire_csi_dcm : 1;
  uint32_t acquire_csi_beamformed : 1;
  uint32_t acquire_csi_he_stbc : 2;
  uint32_t val_scale_cfg : 2;
  uint32_t dump_ack_en : 1;
  uint32_t reserved : 19;
} wifi_csi_acquire_config_t;
typedef wifi_csi_acquire_config_t wifi_csi_config_t;
static const char* SHAPE = "HE IDF 5.1 (C6)";
#elif defined(CSI_TEST_SHAPE_HE55)
#define CONFIG_SOC_WIFI_HE_SUPPORT 1
typedef struct {
  uint32_t enable                 : 1;
  uint32_t acquire_csi_legacy     : 1;
  uint32_t acquire_csi_force_lltf : 1;
  uint32_t acquire_csi_ht20       : 1;
  uint32_t acquire_csi_ht40       : 1;
  uint32_t acquire_csi_vht        : 1;
  uint32_t acquire_csi_su         : 1;
  uint32_t acquire_csi_mu         : 1;
  uint32_t acquire_csi_dcm        : 1;
  uint32_t acquire_csi_beamformed : 1;
  uint32_t acquire_csi_he_stbc_mode: 2;
  uint32_t val_scale_cfg           : 4;
  uint32_t dump_ack_en             : 1;
  uint32_t reserved                : 15;
} wifi_csi_acquire_config_t;
typedef wifi_csi_acquire_config_t wifi_csi_config_t;
static const char* SHAPE = "HE IDF 5.5 MAC v3 (C5/C6/C61)";
#else
#error "build with one -DCSI_TEST_SHAPE_* (see Makefile)"
#endif

#include "csi_idf_compat.h"

int main() {
  wifi_csi_config_t cfg;
  memset(&cfg, 0xFF, sizeof(cfg));
  csi_idf_fill_config(&cfg);

#if SECURACV_CSI_IDF_HE_CONFIG
  assert(cfg.enable == 1);
  assert(cfg.acquire_csi_legacy == 1);
  assert(cfg.acquire_csi_ht20 == 1);
  assert(cfg.acquire_csi_ht40 == 1);
  assert(cfg.acquire_csi_su == 0);
  assert(cfg.acquire_csi_mu == 0);
  assert(cfg.acquire_csi_dcm == 0);
  assert(cfg.acquire_csi_beamformed == 0);
  assert(cfg.val_scale_cfg == 0);
  assert(cfg.dump_ack_en == 0);
  assert(cfg.reserved == 0);
  #if defined(CSI_TEST_SHAPE_HE51)
  assert(cfg.acquire_csi_he_stbc == 0);          // untouched → zero
  #else
  assert(cfg.acquire_csi_force_lltf == 0);       // untouched → zero
  assert(cfg.acquire_csi_vht == 0);
  assert(cfg.acquire_csi_he_stbc_mode == 0);
  #endif
#else
  assert(cfg.lltf_en == true);
  assert(cfg.htltf_en == true);
  assert(cfg.stbc_htltf2_en == false);
  assert(cfg.ltf_merge_en == true);
  assert(cfg.channel_filter_en == true);
  assert(cfg.manu_scale == false);
  assert(cfg.shift == 0);
  #if defined(CSI_TEST_SHAPE_LEGACY5X)
  assert(cfg.dump_ack_en == false);              // untouched → zero
  #endif
#endif
  printf("test_csi_idf_compat [%s]: PASSED\n", SHAPE);
  return 0;
}
