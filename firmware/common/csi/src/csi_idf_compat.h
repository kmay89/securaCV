/*
 * SecuraCV Canary — CSI ESP-IDF compatibility shim
 *
 * ESP-IDF exposes two incompatible CSI acquisition configs under the SAME
 * type name, selected by the target's Wi-Fi MAC generation:
 *
 *   • legacy (ESP32 / S2 / S3 / C3 — IDF 4.4 and 5.x):
 *       wifi_csi_config_t { lltf_en, htltf_en, stbc_htltf2_en, ltf_merge_en,
 *                           channel_filter_en, manu_scale, shift
 *                           (+ dump_ack_en on IDF 5.x) }
 *   • HE targets (C6 / C5 / C61 — IDF 5.1+, CONFIG_SOC_WIFI_HE_SUPPORT):
 *       typedef wifi_csi_acquire_config_t wifi_csi_config_t;
 *       bitfields enable / acquire_csi_legacy / acquire_csi_ht20 /
 *       acquire_csi_ht40 / acquire_csi_su / acquire_csi_mu / acquire_csi_dcm /
 *       acquire_csi_beamformed / val_scale_cfg / dump_ack_en (+ target- and
 *       version-specific extras: acquire_csi_force_lltf, acquire_csi_vht,
 *       and an STBC field that was RENAMED between IDF 5.1 and 5.5).
 *
 * Before this shim both HALs filled the legacy fields unconditionally, so
 * they did not compile on an ESP32-C6 even though the README said "C6 ✅".
 * This header is the ONE place the field names live. Only members present
 * in every known revision of each shape are touched; everything else stays
 * at its memset-zero default (which is why the STBC field is not named).
 *
 * Header-only, no includes of its own. Include it AFTER <esp_wifi.h> on the
 * device, or after a stub `wifi_csi_config_t` in a host test. The host test
 * (tests_host/test_csi_idf_compat.cpp) compiles this file against the three
 * struct shapes above so a field rename upstream is caught by a g++ run,
 * not by a customer's C6.
 *
 * License: MIT (matches the library).
 */
#ifndef SECURACV_CSI_IDF_COMPAT_H
#define SECURACV_CSI_IDF_COMPAT_H

#include <string.h>

#if defined(CONFIG_SOC_WIFI_HE_SUPPORT) && CONFIG_SOC_WIFI_HE_SUPPORT
  #define SECURACV_CSI_IDF_HE_CONFIG 1
#else
  #define SECURACV_CSI_IDF_HE_CONFIG 0
#endif

/*
 * Fill the driver config for whichever struct shape this target exposes.
 *
 * Both shapes request the same physical thing: the legacy L-LTF on EVERY
 * frame (the 64-tone section csi_subcarriers.h canonicalizes on — it is the
 * one training field every 802.11a/g/n/ax frame carries) plus the HT-LTF on
 * HT frames. HE-LTF acquisition stays OFF on Wi-Fi 6 parts: its tone count
 * differs per PPDU type, which is exactly the mixed-count problem the L-LTF
 * selection exists to avoid. Turning it on is a deliberate future step that
 * needs its own tone map, not a flag flip.
 */
static inline void csi_idf_fill_config(wifi_csi_config_t* cfg) {
  memset(cfg, 0, sizeof(*cfg));
#if SECURACV_CSI_IDF_HE_CONFIG
  cfg->enable                 = 1;
  cfg->acquire_csi_legacy     = 1;   /* L-LTF on 11g/11n/11ax PPDUs */
  cfg->acquire_csi_ht20       = 1;   /* HT-LTF, 20 MHz HT PPDUs */
  cfg->acquire_csi_ht40       = 1;   /* HT-LTF, 40 MHz HT PPDUs */
  cfg->acquire_csi_su         = 0;   /* HE-LTF paths off (see above) */
  cfg->acquire_csi_mu         = 0;
  cfg->acquire_csi_dcm        = 0;
  cfg->acquire_csi_beamformed = 0;
  cfg->val_scale_cfg          = 0;   /* driver default scaling */
  cfg->dump_ack_en            = 0;   /* ACKs are 14 bytes of nothing */
#else
  cfg->lltf_en           = true;     /* L-LTF: present on every frame */
  cfg->htltf_en          = true;     /* HT-LTF on HT frames */
  cfg->stbc_htltf2_en    = false;
  cfg->ltf_merge_en      = true;     /* merge HT-LTF1/2 on STBC frames */
  cfg->channel_filter_en = true;     /* driver-side adjacent-tone smoothing */
  cfg->manu_scale        = false;    /* automatic per-frame scaling */
  cfg->shift             = 0;
#endif
}

#endif /* SECURACV_CSI_IDF_COMPAT_H */
