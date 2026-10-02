/* Host stand-in for <esp_flash_encrypt.h>: flash encryption reads as on
 * (a test can switch it off), so the FE-gated NVS paths run. */
#ifndef STUB_MESH_NET_ESP_FLASH_ENCRYPT_H
#define STUB_MESH_NET_ESP_FLASH_ENCRYPT_H
namespace host_sim { inline bool flash_encrypted = true; }
inline bool esp_flash_encryption_enabled() { return host_sim::flash_encrypted; }
#endif
