/**
 * @file csi_module.cpp
 * @brief Implementation of the bounded, allocation-free module registry.
 */

#include "csi_module.h"

#include <string.h>

/* ──────────────────────────────────────────────────────────────────────────
 * BOUNDED REGISTRY
 *
 * Heap allocation is forbidden in our hot-loop budget; the registry is a
 * fixed-size array. The bound is generous (16) so the four v1 modules plus
 * a handful of third-party modules fit comfortably; raise it via build flag
 * if a large fleet of modules is needed.
 * ────────────────────────────────────────────────────────────────────────── */

#ifndef CSI_MODULE_MAX
#define CSI_MODULE_MAX 16
#endif

namespace {

const csi_module_t* g_modules[CSI_MODULE_MAX] = {nullptr};
size_t              g_module_count = 0;

/* Cache the id of the module that last emitted each pending event_id so
 * dismiss callbacks can route correctly. The array is intentionally tiny
 * — the dashboard's Today sheet only shows today's events and dismissals
 * almost always come within minutes of emission. Old entries are
 * overwritten in a round-robin fashion. */
struct DismissRouting {
  uint32_t event_id;
  uint8_t  module_index;
};
constexpr size_t DISMISS_ROUTE_CAP = 32;
DismissRouting g_dismiss_routes[DISMISS_ROUTE_CAP] = {};
size_t         g_dismiss_route_head = 0;

bool manifest_well_formed(const csi_module_t* m) {
  if (!m) return false;
  if (!m->id || !m->id[0]) return false;
  if (!m->tick) return false;
  if (!m->events || m->event_count == 0) return false;

  /* Allow-list bits unknown to this runtime build are rejected to prevent
   * a future module from sneaking through fields we don't yet enforce. */
  constexpr uint32_t known_bits =
      CSI_FIELD_STATE_NAME      |
      CSI_FIELD_CONFIDENCE      |
      CSI_FIELD_DURATION_SEC    |
      CSI_FIELD_TIME_BUCKET     |
      CSI_FIELD_MOTION_SCORE    |
      CSI_FIELD_BREATHING_SCORE |
      CSI_FIELD_BREATHING_RATE  |
      CSI_FIELD_DOMINANT_SIGNAL |
      CSI_FIELD_BUNDLED_COUNT   |
      CSI_FIELD_DISMISSED       |
      CSI_FIELD_NOTE;

  for (size_t i = 0; i < m->event_count; ++i) {
    const csi_event_decl_t* d = &m->events[i];
    if (!d->type_name || !d->type_name[0]) return false;
    if ((d->allowed_fields & ~known_bits) != 0) return false;
  }
  return true;
}

}  /* namespace */

extern "C" {

bool csi_module_register(const csi_module_t* module) {
  if (!manifest_well_formed(module)) return false;
  if (g_module_count >= CSI_MODULE_MAX) return false;
  /* Reject duplicate ids — modules are singletons. */
  for (size_t i = 0; i < g_module_count; ++i) {
    if (strcmp(g_modules[i]->id, module->id) == 0) return false;
  }
  g_modules[g_module_count++] = module;
  return true;
}

size_t csi_module_count(void) { return g_module_count; }

const csi_module_t* csi_module_find(const char* id) {
  if (!id) return nullptr;
  for (size_t i = 0; i < g_module_count; ++i) {
    if (strcmp(g_modules[i]->id, id) == 0) return g_modules[i];
  }
  return nullptr;
}

void csi_module_tick_all(const csi_features_t* features) {
  if (!features) return;
  for (size_t i = 0; i < g_module_count; ++i) {
    const csi_module_t* m = g_modules[i];
    if (m && m->tick) m->tick(features);
  }
}

void csi_module_dispatch_dismiss(uint32_t event_id) {
  for (size_t k = 0; k < DISMISS_ROUTE_CAP; ++k) {
    const DismissRouting& r = g_dismiss_routes[k];
    if (r.event_id == event_id) {
      if (r.module_index < g_module_count) {
        const csi_module_t* m = g_modules[r.module_index];
        if (m && m->on_event_dismissed) m->on_event_dismissed(event_id);
      }
      return;
    }
  }
}

/* Internal hook called by csi_event::emit() so dismiss can route later. */
void csi_module_record_emission_(uint32_t event_id, const char* module_id) {
  if (!module_id) return;
  uint8_t idx = 0xff;
  for (size_t i = 0; i < g_module_count; ++i) {
    if (strcmp(g_modules[i]->id, module_id) == 0) { idx = (uint8_t)i; break; }
  }
  if (idx == 0xff) return;
  g_dismiss_routes[g_dismiss_route_head].event_id     = event_id;
  g_dismiss_routes[g_dismiss_route_head].module_index = idx;
  g_dismiss_route_head = (g_dismiss_route_head + 1) % DISMISS_ROUTE_CAP;
}

/* Default settings reader: returns the supplied default. The host
 * application provides the real implementation that reads NVS. The weak
 * symbol pattern keeps the library standalone-buildable. */
__attribute__((weak)) int32_t csi_module_settings_int(
    const csi_module_settings_t*, const char*, int32_t default_value) {
  return default_value;
}
__attribute__((weak)) float csi_module_settings_float(
    const csi_module_settings_t*, const char*, float default_value) {
  return default_value;
}
__attribute__((weak)) bool csi_module_settings_bool(
    const csi_module_settings_t*, const char*, bool default_value) {
  return default_value;
}

}  /* extern "C" */
