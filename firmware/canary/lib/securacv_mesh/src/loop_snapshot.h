/**
 * @file loop_snapshot.h
 * @brief State the loop task owns, published for other tasks to read whole
 *        (sweep F110).
 *
 * The canary-wap's mesh status routes (GET /api/mesh, /api/mesh/peers and
 * /api/mesh/alerts) run on esp_http_server's task, and what they show (the
 * peer table, the pairing session, the alert history) is
 * mesh_network::update()'s, written on the loop task every pass. Read in
 * place, one response could mix two passes: a member's name read mid-shift
 * after a removal, a pairing code read while cancel_pairing() wipes it, an
 * alert half overwritten. loop_command_ring.h moved the routes that change
 * that state to the loop task (sweep F96); this moves what the reading
 * routes see to copies the loop task publishes.
 *
 * Value<T>: the loop task builds a whole T and publishes it; any task reads
 * a whole copy of the last one published. A publish of the same bytes takes
 * no lock (the writer compares against its own last copy, which only it
 * writes), so publishing on every pass costs a compare.
 *
 * AttachedValue<T>: the same, with the published bytes in storage the loop
 * task attaches (a PSRAM block) instead of the object, for a T too large to
 * keep in internal SRAM; the lock and the flag stay in the object (sweep
 * F138: the Chirp routes' tables).
 *
 * Log<E, N>: a bounded log the loop task appends records to (the oldest
 * overwritten once N are held) and clears; any task reads the records whole
 * and all from one moment. A read copies one record per critical section,
 * and starts over when the loop task changed the log between two of them
 * (a generation count); after kReadAttempts such starts it copies them all
 * under one hold, so a read always ends and is never torn.
 *
 * The PlatformIO canary's mesh status routes read their copy the same way
 * (sweep F161): mesh_session publishes its StatusView through Value<T>.
 * This header is shared: firmware/canary/lib/securacv_mesh/src/
 * loop_snapshot.h is the canonical copy and this sketch's is held
 * byte-identical to it by firmware/scripts/check_mesh_sync.sh, so an edit
 * goes there first and is copied here. Each tree brings its own Lock.
 *
 * Locking as in loop_command_ring.h: everything another task reads is
 * written under Lock (PortMuxLock on the device, a FreeRTOS spinlock), and
 * each critical section copies one value or one record; the writer's own
 * reads take none, since it is the only writer. Nothing here needs Arduino
 * or FreeRTOS, so the host tests compile it as is, with a std::mutex for
 * their threads.
 */

#ifndef SECURACV_LOOP_SNAPSHOT_H
#define SECURACV_LOOP_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <type_traits>

namespace loop_snapshot {

template <typename L>
struct Guard {
  explicit Guard(L& l) : l_(l) { l_.lock(); }
  ~Guard() { l_.unlock(); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
  L& l_;
};

template <typename T, typename Lock>
class Value {
  static_assert(std::is_trivially_copyable<T>::value, "a value is copied whole under the lock");

 public:
  /* The loop task, the one writer. Copies `v` in under the lock unless its
   * bytes are the ones published already; true when it was copied. A
   * caller zeroes a T before it fills one, so padding compares equal. */
  bool publish(const T& v) {
    if (published_ && memcmp(&v, &value_, sizeof(T)) == 0) return false;
    Guard<Lock> g(lock_);
    memcpy(&value_, &v, sizeof(T));
    published_ = true;
    return true;
  }

  /* Any task. A whole copy of the last value published; false, with *out
   * untouched, before the first publish. */
  bool read(T* out) {
    Guard<Lock> g(lock_);
    if (!published_) return false;
    memcpy(out, &value_, sizeof(T));
    return true;
  }

 private:
  Lock lock_;
  T value_{};
  bool published_ = false;
};

/* Value<T> for a T too large for internal SRAM (sweep F138: the Chirp
 * routes' nearby and recent tables): the published bytes live where the
 * loop task's attach() says (a PSRAM block, csi_mem.h), as Log<>'s records
 * do; the lock and the published flag stay in the object, which the caller
 * keeps on-die (a spinlock never lives in PSRAM). Until attach() gives it
 * storage (or after attach(nullptr)) a publish keeps nothing and a read
 * answers false, as before a first publish. */
template <typename T, typename Lock>
class AttachedValue {
  static_assert(std::is_trivially_copyable<T>::value, "a value is copied whole under the lock");

 public:
  /* The loop task. Where the published T lives; nothing is published until
   * the next publish(). */
  void attach(T* storage) {
    Guard<Lock> g(lock_);
    storage_ = storage;
    published_ = false;
  }

  /* The loop task, the one writer: Value<T>::publish(), into the storage. */
  bool publish(const T& v) {
    if (storage_ == nullptr) return false;
    if (published_ && memcmp(&v, storage_, sizeof(T)) == 0) return false;
    Guard<Lock> g(lock_);
    memcpy(storage_, &v, sizeof(T));
    published_ = true;
    return true;
  }

  /* Any task: Value<T>::read(). */
  bool read(T* out) {
    Guard<Lock> g(lock_);
    if (!published_) return false;
    memcpy(out, storage_, sizeof(T));
    return true;
  }

 private:
  Lock lock_;
  T* storage_ = nullptr;
  bool published_ = false;
};

template <typename E, size_t N, typename Lock>
class Log {
  static_assert(N > 0, "a log holds at least one record");
  static_assert(std::is_trivially_copyable<E>::value, "a record is copied whole under the lock");

 public:
  static constexpr unsigned kReadAttempts = 4;

  /* The loop task. Where the N records live (null: appends are dropped and
   * every read is empty); the log starts empty. */
  void attach(E* storage) {
    Guard<Lock> g(lock_);
    storage_ = storage;
    head_ = 0;
    count_ = 0;
    ++gen_;
  }

  /* The loop task (the one writer): its own view, no lock. */
  E* storage() const { return storage_; }
  size_t count() const { return count_; }

  /* The loop task. False when there is nowhere to keep it. */
  bool append(const E& e) {
    if (storage_ == nullptr) return false;
    Guard<Lock> g(lock_);
    memcpy(&storage_[head_], &e, sizeof(E));
    head_ = (head_ + 1) % N;
    if (count_ < N) ++count_;
    ++gen_;
    return true;
  }

  /* The loop task. Empties the log, then zeroes the records outside the
   * lock: no reader copies a record from the generation change on (each of
   * read()'s critical sections checks it first), and every one that copied
   * before it held the lock before this did. */
  void clear() {
    {
      Guard<Lock> g(lock_);
      head_ = 0;
      count_ = 0;
      ++gen_;
    }
    if (storage_ != nullptr) memset(storage_, 0, N * sizeof(E));
  }

  /* Any task. Copies the records in storage order (slot 0 first), at most
   * `cap` of them, and returns how many: every one whole, and all from one
   * moment. */
  size_t read(E* out, size_t cap) {
    for (unsigned attempt = 0; attempt < kReadAttempts; ++attempt) {
      uint32_t gen;
      size_t n;
      {
        Guard<Lock> g(lock_);
        gen = gen_;
        n = held(cap);
      }
      size_t i = 0;
      for (; i < n; ++i) {
        Guard<Lock> g(lock_);
        if (gen_ != gen) break;   // the loop task changed it: start over
        memcpy(&out[i], &storage_[i], sizeof(E));
      }
      if (i == n) return n;
    }
    Guard<Lock> g(lock_);
    const size_t n = held(cap);
    if (n > 0) memcpy(out, storage_, n * sizeof(E));
    return n;
  }

 private:
  size_t held(size_t cap) const {
    if (storage_ == nullptr) return 0;
    return count_ < cap ? count_ : cap;
  }

  Lock lock_;
  E* storage_ = nullptr;
  size_t head_ = 0;
  size_t count_ = 0;
  uint32_t gen_ = 0;
};

}  // namespace loop_snapshot

#endif  // SECURACV_LOOP_SNAPSHOT_H
