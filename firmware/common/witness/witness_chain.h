/**
 * @file witness_chain.h
 * @brief Witness chain management for tamper-evident logging
 *
 * Implements the Privacy Witness Kernel (PWK) compatible hash chain
 * with Ed25519 signatures, domain-separated hashing, and monotonic
 * sequence numbers.
 *
 * SECURITY PROPERTIES:
 * - Unique device identity from hardware RNG
 * - Monotonic sequence numbers (persist across reboots)
 * - Hash chain with domain separation (tamper-evident)
 * - Ed25519 signatures on every record
 * - Time coarsening for privacy
 */

#pragma once

#include "../core/types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CONSTANTS
// ============================================================================

#define WITNESS_PUBKEY_SIZE     32
#define WITNESS_PRIVKEY_SIZE    32
#define WITNESS_HASH_SIZE       32
#define WITNESS_SIG_SIZE        64
#define WITNESS_FINGERPRINT_LEN 8

// Domain separation prefixes
#define DOMAIN_CHAIN_HASH       "securacv:chain:v1"
#define DOMAIN_PAYLOAD_HASH     "securacv:payload:v1"
#define DOMAIN_BOOT_ATTEST      "securacv:boot:v1"

// ============================================================================
// CHAIN STATE
// ============================================================================

/**
 * @brief Witness chain state
 */
typedef struct {
    uint8_t private_key[WITNESS_PRIVKEY_SIZE];
    uint8_t public_key[WITNESS_PUBKEY_SIZE];
    uint8_t chain_head[WITNESS_HASH_SIZE];
    uint8_t fingerprint[WITNESS_FINGERPRINT_LEN];
    uint32_t sequence;
    uint32_t boot_count;
    bool initialized;
    char device_id[32];
} witness_chain_t;

/**
 * @brief Chain configuration
 */
typedef struct {
    const char* device_id_prefix;   // Prefix for device ID
    uint32_t time_bucket_ms;        // Time coarsening bucket
    uint32_t persist_interval;      // Records between persists
    bool auto_persist;              // Auto-persist chain state
} witness_chain_config_t;

// Default configuration
#define WITNESS_CHAIN_CONFIG_DEFAULT { \
    .device_id_prefix = "canary-", \
    .time_bucket_ms = 5000, \
    .persist_interval = 10, \
    .auto_persist = true, \
}

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize witness chain
 *
 * Loads existing identity from NVS or generates a new one.
 *
 * @param chain Chain state structure
 * @param config Configuration
 * @return RESULT_OK on success
 */
result_t witness_chain_init(witness_chain_t* chain, const witness_chain_config_t* config);

/**
 * @brief Deinitialize witness chain
 *
 * Persists state and securely wipes keys from memory.
 *
 * @param chain Chain state
 * @return RESULT_OK on success
 */
result_t witness_chain_deinit(witness_chain_t* chain);

/**
 * @brief Check if chain is initialized
 */
bool witness_chain_is_initialized(const witness_chain_t* chain);

// ============================================================================
// RECORD CREATION
// ============================================================================

/**
 * @brief Create a new witness record
 *
 * Creates a new record with:
 * - Monotonic sequence number
 * - Coarsened timestamp
 * - Domain-separated payload hash
 * - Chain hash linking to previous
 * - Ed25519 signature
 *
 * @param chain Chain state
 * @param type Record type
 * @param payload Payload data
 * @param payload_len Payload length
 * @param record Output record structure
 * @return RESULT_OK on success
 */
result_t witness_chain_create_record(
    witness_chain_t* chain,
    record_type_t type,
    const uint8_t* payload,
    size_t payload_len,
    witness_record_t* record
);

/**
 * @brief Create boot attestation record
 *
 * Special record created at boot containing device identity proof.
 *
 * @param chain Chain state
 * @param record Output record
 * @return RESULT_OK on success
 */
result_t witness_chain_create_boot_attestation(
    witness_chain_t* chain,
    witness_record_t* record
);

// ============================================================================
// VERIFICATION
// ============================================================================

/**
 * @brief Verify a witness record
 *
 * Checks:
 * - Signature is valid
 * - Chain hash is correct
 * - Sequence number is valid
 *
 * @param chain Chain state
 * @param record Record to verify
 * @param prev_hash Previous chain hash (NULL for first record)
 * @return RESULT_OK if valid
 */
result_t witness_chain_verify_record(
    const witness_chain_t* chain,
    const witness_record_t* record,
    const uint8_t* prev_hash
);

/**
 * @brief Self-verify the last created record
 *
 * Verifies that the signature we just created is valid.
 * Should be called after every record creation.
 *
 * @param chain Chain state
 * @param record Record to verify
 * @return RESULT_OK if valid
 */
result_t witness_chain_self_verify(
    const witness_chain_t* chain,
    const witness_record_t* record
);

// ============================================================================
// PERSISTENCE
// ============================================================================

/**
 * @brief Persist chain state to NVS
 *
 * Saves:
 * - Chain head hash
 * - Sequence number
 * - Boot count
 *
 * @param chain Chain state
 * @return RESULT_OK on success
 */
result_t witness_chain_persist(witness_chain_t* chain);

/**
 * @brief Load chain state from NVS
 *
 * @param chain Chain state
 * @return RESULT_OK on success, RESULT_NOT_FOUND if no saved state
 */
result_t witness_chain_load(witness_chain_t* chain);

// ============================================================================
// UTILITIES
// ============================================================================

/**
 * @brief Get device ID string
 */
const char* witness_chain_device_id(const witness_chain_t* chain);

/**
 * @brief Get public key
 */
const uint8_t* witness_chain_public_key(const witness_chain_t* chain);

/**
 * @brief Get public key fingerprint (8 bytes)
 */
const uint8_t* witness_chain_fingerprint(const witness_chain_t* chain);

/**
 * @brief Get current sequence number
 */
uint32_t witness_chain_sequence(const witness_chain_t* chain);

/**
 * @brief Get boot count
 */
uint32_t witness_chain_boot_count(const witness_chain_t* chain);

/**
 * @brief Format fingerprint as hex string
 * @param chain Chain state
 * @param buf Output buffer (at least 17 chars)
 */
void witness_chain_fingerprint_str(const witness_chain_t* chain, char* buf);

/**
 * @brief Get coarsened timestamp
 *
 * Returns timestamp rounded to time bucket for privacy.
 *
 * @param time_bucket_ms Bucket size in milliseconds
 * @return Coarsened timestamp
 */
uint32_t witness_chain_coarse_time(uint32_t time_bucket_ms);

#ifdef __cplusplus
}
#endif
