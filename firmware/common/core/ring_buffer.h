/**
 * @file ring_buffer.h
 * @brief Generic ring buffer implementation
 *
 * Thread-safe ring buffer for byte streams or fixed-size elements.
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
// BYTE RING BUFFER
// ============================================================================

/**
 * @brief Byte ring buffer structure
 */
typedef struct {
    uint8_t* buffer;        // Buffer storage
    size_t capacity;        // Total capacity
    size_t head;            // Write position
    size_t tail;            // Read position
    size_t count;           // Current count
} ring_buffer_t;

/**
 * @brief Initialize ring buffer
 * @param rb Ring buffer structure
 * @param buffer Storage buffer
 * @param capacity Buffer size in bytes
 */
static inline void ring_buffer_init(ring_buffer_t* rb, uint8_t* buffer, size_t capacity) {
    rb->buffer = buffer;
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
}

/**
 * @brief Reset ring buffer to empty state
 */
static inline void ring_buffer_reset(ring_buffer_t* rb) {
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
}

/**
 * @brief Check if buffer is empty
 */
static inline bool ring_buffer_empty(const ring_buffer_t* rb) {
    return rb->count == 0;
}

/**
 * @brief Check if buffer is full
 */
static inline bool ring_buffer_full(const ring_buffer_t* rb) {
    return rb->count >= rb->capacity;
}

/**
 * @brief Get current byte count
 */
static inline size_t ring_buffer_count(const ring_buffer_t* rb) {
    return rb->count;
}

/**
 * @brief Get available space
 */
static inline size_t ring_buffer_space(const ring_buffer_t* rb) {
    return rb->capacity - rb->count;
}

/**
 * @brief Push a single byte
 * @return true on success, false if full
 */
static inline bool ring_buffer_push(ring_buffer_t* rb, uint8_t byte) {
    if (ring_buffer_full(rb)) {
        return false;
    }
    rb->buffer[rb->head] = byte;
    rb->head = (rb->head + 1) % rb->capacity;
    rb->count++;
    return true;
}

/**
 * @brief Pop a single byte
 * @return true on success, false if empty
 */
static inline bool ring_buffer_pop(ring_buffer_t* rb, uint8_t* byte) {
    if (ring_buffer_empty(rb)) {
        return false;
    }
    *byte = rb->buffer[rb->tail];
    rb->tail = (rb->tail + 1) % rb->capacity;
    rb->count--;
    return true;
}

/**
 * @brief Peek at next byte without removing
 * @return true on success, false if empty
 */
static inline bool ring_buffer_peek(const ring_buffer_t* rb, uint8_t* byte) {
    if (ring_buffer_empty(rb)) {
        return false;
    }
    *byte = rb->buffer[rb->tail];
    return true;
}

/**
 * @brief Write multiple bytes
 * @return Number of bytes written
 */
static inline size_t ring_buffer_write(ring_buffer_t* rb, const uint8_t* data, size_t len) {
    size_t written = 0;
    while (written < len && !ring_buffer_full(rb)) {
        ring_buffer_push(rb, data[written++]);
    }
    return written;
}

/**
 * @brief Read multiple bytes
 * @return Number of bytes read
 */
static inline size_t ring_buffer_read(ring_buffer_t* rb, uint8_t* data, size_t len) {
    size_t read = 0;
    while (read < len && !ring_buffer_empty(rb)) {
        ring_buffer_pop(rb, &data[read++]);
    }
    return read;
}

// ============================================================================
// C++ TEMPLATE VERSION
// ============================================================================

#ifdef __cplusplus
}

/**
 * @brief C++ template ring buffer
 */
template <typename T, size_t N>
class RingBufferT {
public:
    RingBufferT() : head_(0), tail_(0), count_(0) {}

    void reset() {
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

    bool empty() const { return count_ == 0; }
    bool full() const { return count_ >= N; }
    size_t count() const { return count_; }
    size_t capacity() const { return N; }
    size_t space() const { return N - count_; }

    bool push(const T& item) {
        if (full()) return false;
        buffer_[head_] = item;
        head_ = (head_ + 1) % N;
        count_++;
        return true;
    }

    bool pop(T& item) {
        if (empty()) return false;
        item = buffer_[tail_];
        tail_ = (tail_ + 1) % N;
        count_--;
        return true;
    }

    bool peek(T& item) const {
        if (empty()) return false;
        item = buffer_[tail_];
        return true;
    }

    // Peek at item at offset from tail
    bool peek_at(size_t offset, T& item) const {
        if (offset >= count_) return false;
        item = buffer_[(tail_ + offset) % N];
        return true;
    }

private:
    T buffer_[N];
    size_t head_;
    size_t tail_;
    size_t count_;
};

extern "C" {
#endif

#ifdef __cplusplus
}
#endif
