/**
 * @file hal_crypto.h
 * @brief HAL Cryptography Interface
 *
 * Provides hardware-accelerated cryptographic operations where available,
 * with software fallbacks. Supports Ed25519 signatures, SHA-256 hashing,
 * and secure random number generation.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CONSTANTS
// ============================================================================

#define HAL_ED25519_PUBKEY_SIZE     32
#define HAL_ED25519_PRIVKEY_SIZE    32
#define HAL_ED25519_SIGNATURE_SIZE  64
#define HAL_SHA256_HASH_SIZE        32
#define HAL_SHA256_BLOCK_SIZE       64

// ============================================================================
// ED25519 SIGNATURES
// ============================================================================

/**
 * @brief Generate Ed25519 keypair
 * @param pubkey Output public key (32 bytes)
 * @param privkey Output private key (32 bytes)
 * @return 0 on success, negative on error
 */
int hal_ed25519_keygen(uint8_t pubkey[HAL_ED25519_PUBKEY_SIZE],
                       uint8_t privkey[HAL_ED25519_PRIVKEY_SIZE]);

/**
 * @brief Derive public key from private key
 * @param privkey Private key (32 bytes)
 * @param pubkey Output public key (32 bytes)
 * @return 0 on success, negative on error
 */
int hal_ed25519_pubkey(const uint8_t privkey[HAL_ED25519_PRIVKEY_SIZE],
                       uint8_t pubkey[HAL_ED25519_PUBKEY_SIZE]);

/**
 * @brief Sign message with Ed25519
 * @param privkey Private key (32 bytes)
 * @param pubkey Public key (32 bytes)
 * @param msg Message to sign
 * @param msg_len Message length
 * @param signature Output signature (64 bytes)
 * @return 0 on success, negative on error
 */
int hal_ed25519_sign(const uint8_t privkey[HAL_ED25519_PRIVKEY_SIZE],
                     const uint8_t pubkey[HAL_ED25519_PUBKEY_SIZE],
                     const uint8_t* msg, size_t msg_len,
                     uint8_t signature[HAL_ED25519_SIGNATURE_SIZE]);

/**
 * @brief Verify Ed25519 signature
 * @param pubkey Public key (32 bytes)
 * @param msg Original message
 * @param msg_len Message length
 * @param signature Signature to verify (64 bytes)
 * @return true if valid, false otherwise
 */
bool hal_ed25519_verify(const uint8_t pubkey[HAL_ED25519_PUBKEY_SIZE],
                        const uint8_t* msg, size_t msg_len,
                        const uint8_t signature[HAL_ED25519_SIGNATURE_SIZE]);

// ============================================================================
// SHA-256 HASHING
// ============================================================================

/**
 * @brief SHA-256 context for incremental hashing
 */
typedef struct {
    uint8_t internal[128];  // Opaque internal state
} hal_sha256_ctx_t;

/**
 * @brief Compute SHA-256 hash in one shot
 * @param data Input data
 * @param len Data length
 * @param hash Output hash (32 bytes)
 * @return 0 on success, negative on error
 */
int hal_sha256(const uint8_t* data, size_t len, uint8_t hash[HAL_SHA256_HASH_SIZE]);

/**
 * @brief Initialize SHA-256 context
 * @param ctx Context to initialize
 * @return 0 on success, negative on error
 */
int hal_sha256_init(hal_sha256_ctx_t* ctx);

/**
 * @brief Update SHA-256 hash with more data
 * @param ctx Context
 * @param data Data to add
 * @param len Data length
 * @return 0 on success, negative on error
 */
int hal_sha256_update(hal_sha256_ctx_t* ctx, const uint8_t* data, size_t len);

/**
 * @brief Finalize SHA-256 hash
 * @param ctx Context
 * @param hash Output hash (32 bytes)
 * @return 0 on success, negative on error
 */
int hal_sha256_final(hal_sha256_ctx_t* ctx, uint8_t hash[HAL_SHA256_HASH_SIZE]);

// ============================================================================
// DOMAIN-SEPARATED HASHING
// ============================================================================

/**
 * @brief Compute domain-separated SHA-256 hash
 *
 * Hash = SHA256(domain || 0x00 || data)
 *
 * @param domain Domain separator string
 * @param data Input data
 * @param len Data length
 * @param hash Output hash (32 bytes)
 * @return 0 on success, negative on error
 */
int hal_sha256_domain(const char* domain, const uint8_t* data, size_t len,
                      uint8_t hash[HAL_SHA256_HASH_SIZE]);

// ============================================================================
// SECURE MEMORY
// ============================================================================

/**
 * @brief Securely wipe memory
 *
 * Prevents compiler from optimizing away the clear operation.
 *
 * @param ptr Memory to clear
 * @param len Number of bytes to clear
 */
void hal_secure_wipe(void* ptr, size_t len);

/**
 * @brief Constant-time memory comparison
 *
 * Prevents timing attacks by always comparing all bytes.
 *
 * @param a First buffer
 * @param b Second buffer
 * @param len Number of bytes to compare
 * @return 0 if equal, non-zero otherwise
 */
int hal_secure_compare(const void* a, const void* b, size_t len);

// ============================================================================
// SELF-TEST
// ============================================================================

/**
 * @brief Run cryptographic self-test
 *
 * Verifies that all crypto operations are working correctly.
 * Should be called at boot and periodically.
 *
 * @return 0 if all tests pass, negative on failure
 */
int hal_crypto_self_test(void);

#ifdef __cplusplus
}
#endif
