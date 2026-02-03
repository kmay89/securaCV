/**
 * @file hal_storage.h
 * @brief HAL Storage Interface
 *
 * Provides hardware-independent persistent storage (NVS/Flash/SD).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// NVS (NON-VOLATILE STORAGE) INTERFACE
// ============================================================================

typedef void* nvs_handle_t;

/**
 * @brief Open NVS namespace
 * @param namespace Namespace name (max 15 chars)
 * @param handle Output handle
 * @return 0 on success, negative on error
 */
int hal_nvs_open(const char* namespace, nvs_handle_t* handle);

/**
 * @brief Close NVS namespace
 * @param handle NVS handle
 */
void hal_nvs_close(nvs_handle_t handle);

/**
 * @brief Write uint32 to NVS
 * @param handle NVS handle
 * @param key Key name (max 15 chars)
 * @param value Value to store
 * @return 0 on success, negative on error
 */
int hal_nvs_set_u32(nvs_handle_t handle, const char* key, uint32_t value);

/**
 * @brief Read uint32 from NVS
 * @param handle NVS handle
 * @param key Key name
 * @param value Output value
 * @return 0 on success, negative on error
 */
int hal_nvs_get_u32(nvs_handle_t handle, const char* key, uint32_t* value);

/**
 * @brief Write blob to NVS
 * @param handle NVS handle
 * @param key Key name
 * @param data Data buffer
 * @param len Data length
 * @return 0 on success, negative on error
 */
int hal_nvs_set_blob(nvs_handle_t handle, const char* key, const void* data, size_t len);

/**
 * @brief Read blob from NVS
 * @param handle NVS handle
 * @param key Key name
 * @param data Output buffer
 * @param len Input: buffer size, Output: bytes read
 * @return 0 on success, negative on error
 */
int hal_nvs_get_blob(nvs_handle_t handle, const char* key, void* data, size_t* len);

/**
 * @brief Write string to NVS
 * @param handle NVS handle
 * @param key Key name
 * @param str Null-terminated string
 * @return 0 on success, negative on error
 */
int hal_nvs_set_str(nvs_handle_t handle, const char* key, const char* str);

/**
 * @brief Read string from NVS
 * @param handle NVS handle
 * @param key Key name
 * @param str Output buffer
 * @param len Buffer size
 * @return 0 on success, negative on error
 */
int hal_nvs_get_str(nvs_handle_t handle, const char* key, char* str, size_t len);

/**
 * @brief Erase key from NVS
 * @param handle NVS handle
 * @param key Key name
 * @return 0 on success, negative on error
 */
int hal_nvs_erase_key(nvs_handle_t handle, const char* key);

/**
 * @brief Commit pending writes to flash
 * @param handle NVS handle
 * @return 0 on success, negative on error
 */
int hal_nvs_commit(nvs_handle_t handle);

// ============================================================================
// SD CARD INTERFACE
// ============================================================================

typedef struct {
    int cs_pin;
    int sck_pin;
    int miso_pin;
    int mosi_pin;
    uint32_t freq_hz;
} sd_config_t;

typedef struct {
    bool mounted;
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    char fs_type[16];
} sd_info_t;

/**
 * @brief Mount SD card
 * @param config SD card configuration
 * @return 0 on success, negative on error
 */
int hal_sd_mount(const sd_config_t* config);

/**
 * @brief Unmount SD card
 * @return 0 on success, negative on error
 */
int hal_sd_unmount(void);

/**
 * @brief Check if SD card is mounted
 * @return true if mounted
 */
bool hal_sd_is_mounted(void);

/**
 * @brief Get SD card information
 * @param info Output info structure
 * @return 0 on success, negative on error
 */
int hal_sd_info(sd_info_t* info);

/**
 * @brief Open file on SD card
 * @param path File path
 * @param mode Mode string ("r", "w", "a", "r+", "w+", "a+")
 * @return File handle or NULL on error
 */
void* hal_sd_fopen(const char* path, const char* mode);

/**
 * @brief Close file
 * @param file File handle
 * @return 0 on success, negative on error
 */
int hal_sd_fclose(void* file);

/**
 * @brief Read from file
 * @param file File handle
 * @param buf Buffer to read into
 * @param size Number of bytes to read
 * @return Number of bytes read, negative on error
 */
int hal_sd_fread(void* file, void* buf, size_t size);

/**
 * @brief Write to file
 * @param file File handle
 * @param buf Data to write
 * @param size Number of bytes to write
 * @return Number of bytes written, negative on error
 */
int hal_sd_fwrite(void* file, const void* buf, size_t size);

/**
 * @brief Flush file buffers
 * @param file File handle
 * @return 0 on success, negative on error
 */
int hal_sd_fflush(void* file);

/**
 * @brief Seek to position in file
 * @param file File handle
 * @param offset Offset from origin
 * @param origin SEEK_SET, SEEK_CUR, or SEEK_END
 * @return 0 on success, negative on error
 */
int hal_sd_fseek(void* file, long offset, int origin);

/**
 * @brief Get current file position
 * @param file File handle
 * @return Current position or negative on error
 */
long hal_sd_ftell(void* file);

/**
 * @brief Check if file exists
 * @param path File path
 * @return true if exists
 */
bool hal_sd_exists(const char* path);

/**
 * @brief Delete file
 * @param path File path
 * @return 0 on success, negative on error
 */
int hal_sd_remove(const char* path);

/**
 * @brief Create directory
 * @param path Directory path
 * @return 0 on success, negative on error
 */
int hal_sd_mkdir(const char* path);

#ifdef __cplusplus
}
#endif
