# Common Firmware Core

Board-agnostic firmware logic shared across projects.

## Module Structure

```
common/
├── core/           # Core types, utilities
│   ├── types.h     # Common data structures
│   ├── ring_buffer.h  # Ring buffer implementation
│   └── feature_sanity.h  # FEATURE_* vs HAS_* compile-time cross-check
├── hal/            # Hardware Abstraction Layer
│   ├── hal.h       # Main HAL header
│   ├── hal_gpio.h  # GPIO interface
│   ├── hal_uart.h  # UART interface
│   ├── hal_spi.h   # SPI interface
│   ├── hal_i2c.h   # I2C interface
│   ├── hal_timer.h # Timer interface
│   ├── hal_storage.h # Storage interface
│   ├── hal_crypto.h  # Cryptography interface
│   ├── hal_wifi.h  # WiFi interface
│   └── hal_ble.h   # BLE interface
├── witness/        # Witness chain management
│   └── witness_chain.h
├── gnss/           # GPS time validation + privacy coarsening
│   ├── gnss_time.h
│   └── gps_privacy.h
├── storage/        # Unified storage
│   └── storage.h
├── network/        # Network modules
│   ├── provision_core.h
│   ├── wifi_join_policy.h
│   └── setup_portal*.{h,cpp}
├── bluetooth/      # BLE management
│   └── bluetooth_mgr.h
├── chirp/          # Community witness network
│   └── chirp_channel.h
├── health/         # Boot policy + serial test console
│   ├── boot_policy.h
│   └── test_console.h
├── encoding/       # Data encoding
│   └── cbor.h
└── web/            # HTTP server
    └── http_server.h
```

Not here on purpose: the health log, mesh network, RF presence and web UI
live next to the canary-wap sketch (`firmware/projects/canary-wap/arduino/
canary_wap/{health_log,mesh_network,rf_presence,web_ui}.h`) and the PIO
tree's logger is `include/canary/log.h` per project. Unbuilt `common/`
scaffolds that shared those names were deleted (roadmap item 29) so an
include path can never pick the wrong one.

## Design Principles

1. **No board-specific code**: Common modules must not include pin mappings
   or board-specific peripherals. Use HAL interfaces instead.

2. **No configuration data**: Common modules must not embed configuration
   values. Configurations come from `configs/`.

3. **Interface-first design**: Each module exposes a clean C interface
   with init/deinit lifecycle and config structs.

4. **Testable**: Modules can be unit tested independently with mock HAL.

## Usage

Include modules via their header files:

```cpp
#include "common/core/types.h"
#include "common/hal/hal.h"
#include "common/witness/witness_chain.h"
```

## Adding a New Module

1. Create a new directory: `common/<module>/`
2. Create the header file with interface definitions
3. Implement the module (implementation files go in project src/)
4. Document the module in this README
5. Ensure no board-specific dependencies
