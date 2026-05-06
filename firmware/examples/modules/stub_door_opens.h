/**
 * @file stub_door_opens.h
 * @brief Example third-party CSI module.
 *
 * Detects a "door opens" signature: a brief but high motion score in the
 * Doppler band immediately followed by a sustained drop into the empty
 * state. The exact thresholds are deliberately unrefined — this module is
 * a tutorial, not a production behavior.
 *
 * Build: drop the .cpp next to your sketch's other source files, then call
 *
 *     csi_module_register(stub_door_opens_module());
 *
 * once at boot.
 */

#ifndef SECURACV_EXAMPLE_STUB_DOOR_OPENS_H
#define SECURACV_EXAMPLE_STUB_DOOR_OPENS_H

#include <csi_module.h>

#ifdef __cplusplus
extern "C" {
#endif

const csi_module_t* stub_door_opens_module(void);

#ifdef __cplusplus
}
#endif

#endif
