/**
 * @file cbor.h
 * @brief Minimal CBOR (RFC 8949) encoder
 *
 * Provides a lightweight CBOR encoder for building PWK-compatible
 * witness record payloads. Only encoding is supported - decoding
 * is handled by the backend.
 *
 * Features:
 * - Zero-allocation design (writes to user-provided buffer)
 * - Supports: integers, strings, bytes, floats, bools, null, maps, arrays
 * - ~1KB code size
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// TYPES
// ============================================================================

/**
 * @brief CBOR writer context
 */
typedef struct {
    uint8_t* buf;           // Output buffer
    size_t cap;             // Buffer capacity
    size_t pos;             // Current write position
    bool error;             // Error flag (overflow)
} cbor_writer_t;

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize CBOR writer
 * @param w Writer context
 * @param buf Output buffer
 * @param cap Buffer capacity
 */
static inline void cbor_init(cbor_writer_t* w, uint8_t* buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->pos = 0;
    w->error = false;
}

/**
 * @brief Get current output size
 * @param w Writer context
 * @return Bytes written
 */
static inline size_t cbor_size(const cbor_writer_t* w) {
    return w->pos;
}

/**
 * @brief Check if writer has error
 * @param w Writer context
 * @return true if overflow occurred
 */
static inline bool cbor_has_error(const cbor_writer_t* w) {
    return w->error;
}

/**
 * @brief Reset writer to beginning
 * @param w Writer context
 */
static inline void cbor_reset(cbor_writer_t* w) {
    w->pos = 0;
    w->error = false;
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

static inline void cbor_write_byte(cbor_writer_t* w, uint8_t b) {
    if (w->pos < w->cap) {
        w->buf[w->pos++] = b;
    } else {
        w->error = true;
    }
}

static inline void cbor_write_bytes(cbor_writer_t* w, const uint8_t* data, size_t len) {
    if (w->pos + len > w->cap) {
        w->error = true;
        return;
    }
    memcpy(w->buf + w->pos, data, len);
    w->pos += len;
}

static inline void cbor_write_type_value(cbor_writer_t* w, uint8_t major, uint64_t val) {
    uint8_t mt = major << 5;

    if (val <= 23) {
        cbor_write_byte(w, mt | (uint8_t)val);
    } else if (val <= 0xFF) {
        cbor_write_byte(w, mt | 24);
        cbor_write_byte(w, (uint8_t)val);
    } else if (val <= 0xFFFF) {
        cbor_write_byte(w, mt | 25);
        cbor_write_byte(w, (val >> 8) & 0xFF);
        cbor_write_byte(w, val & 0xFF);
    } else if (val <= 0xFFFFFFFF) {
        cbor_write_byte(w, mt | 26);
        cbor_write_byte(w, (val >> 24) & 0xFF);
        cbor_write_byte(w, (val >> 16) & 0xFF);
        cbor_write_byte(w, (val >> 8) & 0xFF);
        cbor_write_byte(w, val & 0xFF);
    } else {
        cbor_write_byte(w, mt | 27);
        for (int i = 7; i >= 0; i--) {
            cbor_write_byte(w, (val >> (i * 8)) & 0xFF);
        }
    }
}

// ============================================================================
// ENCODING FUNCTIONS
// ============================================================================

/**
 * @brief Write unsigned integer
 * @param w Writer context
 * @param val Value
 */
static inline void cbor_write_uint(cbor_writer_t* w, uint64_t val) {
    cbor_write_type_value(w, 0, val);
}

/**
 * @brief Write signed integer
 * @param w Writer context
 * @param val Value
 */
static inline void cbor_write_int(cbor_writer_t* w, int64_t val) {
    if (val >= 0) {
        cbor_write_type_value(w, 0, (uint64_t)val);
    } else {
        cbor_write_type_value(w, 1, (uint64_t)(-(val + 1)));
    }
}

/**
 * @brief Write byte string
 * @param w Writer context
 * @param data Byte data
 * @param len Data length
 */
static inline void cbor_write_bstr(cbor_writer_t* w, const uint8_t* data, size_t len) {
    cbor_write_type_value(w, 2, len);
    cbor_write_bytes(w, data, len);
}

/**
 * @brief Write text string (UTF-8)
 * @param w Writer context
 * @param str String (null-terminated)
 */
static inline void cbor_write_tstr(cbor_writer_t* w, const char* str) {
    size_t len = strlen(str);
    cbor_write_type_value(w, 3, len);
    cbor_write_bytes(w, (const uint8_t*)str, len);
}

/**
 * @brief Write array header (fixed length)
 * @param w Writer context
 * @param count Number of elements
 *
 * Note: You must write exactly 'count' elements after this.
 */
static inline void cbor_write_array(cbor_writer_t* w, size_t count) {
    cbor_write_type_value(w, 4, count);
}

/**
 * @brief Write map header (fixed length)
 * @param w Writer context
 * @param count Number of key-value pairs
 *
 * Note: You must write exactly 'count' key-value pairs after this.
 */
static inline void cbor_write_map(cbor_writer_t* w, size_t count) {
    cbor_write_type_value(w, 5, count);
}

/**
 * @brief Write boolean
 * @param w Writer context
 * @param val Boolean value
 */
static inline void cbor_write_bool(cbor_writer_t* w, bool val) {
    cbor_write_byte(w, val ? 0xF5 : 0xF4);
}

/**
 * @brief Write null
 * @param w Writer context
 */
static inline void cbor_write_null(cbor_writer_t* w) {
    cbor_write_byte(w, 0xF6);
}

/**
 * @brief Write undefined
 * @param w Writer context
 */
static inline void cbor_write_undefined(cbor_writer_t* w) {
    cbor_write_byte(w, 0xF7);
}

/**
 * @brief Write IEEE 754 double-precision float
 * @param w Writer context
 * @param val Float value
 */
static inline void cbor_write_float64(cbor_writer_t* w, double val) {
    cbor_write_byte(w, 0xFB);  // Float64 marker
    union {
        double d;
        uint64_t u;
    } conv;
    conv.d = val;
    for (int i = 7; i >= 0; i--) {
        cbor_write_byte(w, (conv.u >> (i * 8)) & 0xFF);
    }
}

/**
 * @brief Write IEEE 754 single-precision float
 * @param w Writer context
 * @param val Float value
 */
static inline void cbor_write_float32(cbor_writer_t* w, float val) {
    cbor_write_byte(w, 0xFA);  // Float32 marker
    union {
        float f;
        uint32_t u;
    } conv;
    conv.f = val;
    cbor_write_byte(w, (conv.u >> 24) & 0xFF);
    cbor_write_byte(w, (conv.u >> 16) & 0xFF);
    cbor_write_byte(w, (conv.u >> 8) & 0xFF);
    cbor_write_byte(w, conv.u & 0xFF);
}

// ============================================================================
// CONVENIENCE MACROS
// ============================================================================

/**
 * @brief Write a string key-value pair
 */
#define CBOR_KV_STR(w, key, val) do { \
    cbor_write_tstr(w, key); \
    cbor_write_tstr(w, val); \
} while(0)

/**
 * @brief Write a string key with uint value
 */
#define CBOR_KV_UINT(w, key, val) do { \
    cbor_write_tstr(w, key); \
    cbor_write_uint(w, val); \
} while(0)

/**
 * @brief Write a string key with int value
 */
#define CBOR_KV_INT(w, key, val) do { \
    cbor_write_tstr(w, key); \
    cbor_write_int(w, val); \
} while(0)

/**
 * @brief Write a string key with float value
 */
#define CBOR_KV_FLOAT(w, key, val) do { \
    cbor_write_tstr(w, key); \
    cbor_write_float64(w, val); \
} while(0)

/**
 * @brief Write a string key with bool value
 */
#define CBOR_KV_BOOL(w, key, val) do { \
    cbor_write_tstr(w, key); \
    cbor_write_bool(w, val); \
} while(0)

/**
 * @brief Write a string key with bytes value
 */
#define CBOR_KV_BYTES(w, key, data, len) do { \
    cbor_write_tstr(w, key); \
    cbor_write_bstr(w, data, len); \
} while(0)

#ifdef __cplusplus
}
#endif

// ============================================================================
// C++ WRAPPER CLASS
// ============================================================================

#ifdef __cplusplus

/**
 * @brief C++ CBOR Writer class
 *
 * Provides a fluent interface for CBOR encoding.
 *
 * Example:
 *   uint8_t buf[256];
 *   CborWriter w(buf, sizeof(buf));
 *   w.map(3)
 *    .key("device_id").str("canary-s3-AB12")
 *    .key("state").str("MOVING")
 *    .key("speed").flt(1.5);
 *   if (w.ok()) {
 *       size_t len = w.size();
 *       // Use buf[0..len-1]
 *   }
 */
class CborWriter {
public:
    CborWriter(uint8_t* buf, size_t cap) {
        cbor_init(&w_, buf, cap);
    }

    // Size and status
    size_t size() const { return cbor_size(&w_); }
    bool ok() const { return !cbor_has_error(&w_); }
    void reset() { cbor_reset(&w_); }

    // Map/Array containers
    CborWriter& map(size_t count) { cbor_write_map(&w_, count); return *this; }
    CborWriter& array(size_t count) { cbor_write_array(&w_, count); return *this; }

    // Key (string for map keys)
    CborWriter& key(const char* k) { cbor_write_tstr(&w_, k); return *this; }

    // Value types
    CborWriter& str(const char* s) { cbor_write_tstr(&w_, s); return *this; }
    CborWriter& bytes(const uint8_t* data, size_t len) { cbor_write_bstr(&w_, data, len); return *this; }
    CborWriter& uint(uint64_t v) { cbor_write_uint(&w_, v); return *this; }
    CborWriter& int_(int64_t v) { cbor_write_int(&w_, v); return *this; }
    CborWriter& flt(double v) { cbor_write_float64(&w_, v); return *this; }
    CborWriter& flt32(float v) { cbor_write_float32(&w_, v); return *this; }
    CborWriter& boolean(bool v) { cbor_write_bool(&w_, v); return *this; }
    CborWriter& null() { cbor_write_null(&w_); return *this; }

    // Convenience key-value methods
    CborWriter& kv(const char* k, const char* v) { key(k); str(v); return *this; }
    CborWriter& kv(const char* k, uint64_t v) { key(k); uint(v); return *this; }
    CborWriter& kv(const char* k, int64_t v) { key(k); int_(v); return *this; }
    CborWriter& kv(const char* k, double v) { key(k); flt(v); return *this; }
    CborWriter& kv(const char* k, bool v) { key(k); boolean(v); return *this; }

private:
    cbor_writer_t w_;
};

#endif // __cplusplus
