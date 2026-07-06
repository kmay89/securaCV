/* Minimal Arduino stub for host-compiling the CSI feature extractor.
 * csi_features.cpp only needs millis(); the test provides the fake clock. */
#ifndef STUB_CSI_ARDUINO_H
#define STUB_CSI_ARDUINO_H

#include <stdint.h>
#include <stddef.h>

unsigned long millis();

#endif
