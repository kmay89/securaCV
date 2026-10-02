/**
 * @file loop_event_queue.h
 * @brief What another task reports, handed to the loop task to apply
 *        (sweep F143).
 *
 * The canary-wap's Bluetooth channel (bluetooth_channel.cpp) is the loop
 * task's: update() and the owner's commands (loop_command_ring.h, sweep
 * F111) read and write the connection, the pairing session and its pending
 * Numeric-Comparison answer, the paired and scanned devices and the state.
 * The NimBLE host task's callbacks (a link up or down, a passkey to show or
 * confirm, a bond, a scan result, a scan's end) wrote the same state from
 * that task, with no lock, and saved the paired list to NVS there. Now a
 * callback only describes what happened and posts it here, and the loop
 * task applies it (consume) on its next pass.
 *
 * Queue<E, N, Lock>: a bounded FIFO of N events. post() copies an event in
 * from any task, unless the queue already holds `limit` events: then the
 * event is dropped and counted, and post() says so, so the producer can do
 * what that event needs instead (bluetooth_channel.cpp rejects a pairing
 * it cannot hand over). A producer of events that may be lost (a scan
 * result, a link's activity) posts with a lower limit, so those can never
 * take the room the ones that must not be lost (a link up or down, a
 * passkey) are kept. The drops are counted by kind: dropped_limited() the
 * posts a lower limit refused (routine in a burst), dropped_reserved() the
 * posts at the full limit that still found no room (the ones the reserve
 * was for: the consumer stalled for that long), dropped() both.
 *
 * consume() is the one consumer's, the loop task's: it applies the events
 * that wait when it starts, oldest first, each copied out under the lock
 * and applied with no lock held, so an apply may take other locks or post
 * again, and a producer that posts as fast as the loop task applies cannot
 * hold it there.
 *
 * Locking as in loop_command_ring.h: every critical section copies one
 * event or reads a count (PortMuxLock on the device, a FreeRTOS spinlock a
 * NimBLE callback may take on the other core). Nothing here needs Arduino
 * or FreeRTOS, so the host tests compile it as is, with a std::mutex for
 * their threads.
 */

#ifndef SECURACV_LOOP_EVENT_QUEUE_H
#define SECURACV_LOOP_EVENT_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <type_traits>

namespace loop_event_queue {

template <typename L>
struct Guard {
  explicit Guard(L& l) : l_(l) { l_.lock(); }
  ~Guard() { l_.unlock(); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
  L& l_;
};

template <typename E, size_t N, typename Lock>
class Queue {
  static_assert(N > 0, "a queue holds at least one event");
  static_assert(std::is_trivially_copyable<E>::value, "an event is copied whole under the lock");

 public:
  static constexpr size_t kCapacity = N;

  /* Any task. Copies `e` in behind the events waiting, unless `limit` of
   * them (at most N) wait already: then nothing is queued, the drop is
   * counted, and it returns false. */
  bool post(const E& e, size_t limit = N) {
    if (limit > N) limit = N;
    Guard<Lock> g(lock_);
    if (count_ >= limit) {
      if (limit < N) {
        ++dropped_limited_;
      } else {
        ++dropped_reserved_;
      }
      return false;
    }
    memcpy(&slots_[(head_ + count_) % N], &e, sizeof(E));
    ++count_;
    return true;
  }

  /* The loop task, the one consumer. Applies each event that waits when it
   * starts, oldest first: copied out under the lock, `apply(event)` called
   * with none held. Events posted meanwhile wait for the next consume().
   * Returns how many it applied. */
  template <typename F>
  size_t consume(F&& apply) {
    size_t n;
    {
      Guard<Lock> g(lock_);
      n = count_;
    }
    for (size_t i = 0; i < n; ++i) {
      E e;
      {
        Guard<Lock> g(lock_);
        memcpy(&e, &slots_[head_], sizeof(E));
        head_ = (head_ + 1) % N;
        --count_;
      }
      apply(e);
    }
    return n;
  }

  /* Any task. Events refused so far, of both kinds (the counts wrap). */
  uint32_t dropped() {
    Guard<Lock> g(lock_);
    return dropped_limited_ + dropped_reserved_;
  }

  /* Any task. Posts under a lower limit that found it reached. */
  uint32_t dropped_limited() {
    Guard<Lock> g(lock_);
    return dropped_limited_;
  }

  /* Any task. Posts at the full limit (N) that found every slot taken. */
  uint32_t dropped_reserved() {
    Guard<Lock> g(lock_);
    return dropped_reserved_;
  }

  /* Any task. How many wait. */
  size_t waiting() {
    Guard<Lock> g(lock_);
    return count_;
  }

 private:
  Lock lock_;
  E slots_[N] = {};
  size_t head_ = 0;
  size_t count_ = 0;
  uint32_t dropped_limited_ = 0;
  uint32_t dropped_reserved_ = 0;
};

}  // namespace loop_event_queue

#endif  // SECURACV_LOOP_EVENT_QUEUE_H
