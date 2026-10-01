/**
 * @file loop_command_ring.h
 * @brief Owner commands handed from a requesting task to the loop task that
 *        owns the state they change (sweep F96).
 *
 * The canary-wap's REST handlers run on esp_http_server's task. The state a
 * mesh command changes (the opera's peer table, the pairing session, the
 * one NVS handle) belongs to the loop task: mesh_network::update() reads and
 * writes it every pass. A handler that changed it in place raced update().
 * Now the handler posts its command here and waits; update() drains the
 * ring on the loop task and leaves each command's result in its slot, where
 * the waiter collects it.
 *
 * Shape: N slots, each free, queued, running or done. post() takes a free
 * slot and stamps it with a ticket, one higher than the last; drain() runs
 * the queued slots in ticket order (post order, whichever slot they took);
 * a waiter polls its ticket and takes the result, which frees the slot.
 * A full ring refuses the post: the caller answers "busy" at once.
 *
 * Waiting (submit): bounded for a command the loop task has not started. At
 * the timeout the waiter withdraws it, and a withdrawn command never runs,
 * so "it did not happen" is a true answer. A command the loop task has
 * started is waited for until it is done: it runs to completion on the loop
 * task without waiting for any other task, and its result is the answer.
 *
 * Locking: every slot read and write happens under the Lock the ring is
 * instantiated with; a command runs outside it. On the device that is a
 * FreeRTOS spinlock (PortMuxLock below): its critical sections only copy a
 * command or a result, so they are short and never block. The host tests
 * pass their own (none, or a std::mutex for the two-thread test).
 *
 * Nothing here includes Arduino or FreeRTOS, so the host tests compile it
 * as is. Callers that wait pass their clock and their sleep (millis() and
 * vTaskDelay on the device).
 */

#ifndef SECURACV_LOOP_COMMAND_RING_H
#define SECURACV_LOOP_COMMAND_RING_H

#include <stddef.h>
#include <stdint.h>

#include <type_traits>

namespace loop_command_ring {

/* What a ticket's command is doing. */
enum class Poll : uint8_t {
  kQueued,   /* waiting for the loop task */
  kRunning,  /* the loop task is running it */
  kDone,     /* it ran; the result was copied out and the slot is free again */
  kUnknown,  /* no live command holds this ticket (never posted, withdrawn,
                or its result already collected) */
};

/* How a submit() ended. */
enum class Wait : uint8_t {
  kDone,       /* the command ran; *result is what it returned */
  kBusy,       /* every slot was taken: it was never queued */
  kWithdrawn,  /* the wait ran out before the loop task started it: it never runs */
};

/* The device's lock: a FreeRTOS spinlock. Defined where FreeRTOS's port
 * macros are, so include <freertos/FreeRTOS.h> before this header. Usable
 * from any task on both chips the canary-wap builds for (the S3's two
 * Xtensa cores and the C3's one RISC-V core), the same portMUX critical
 * section ble_scout.cpp and wifi_presence.h already take. */
#ifdef portMUX_INITIALIZER_UNLOCKED
struct PortMuxLock {
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  void lock() { portENTER_CRITICAL(&mux); }
  void unlock() { portEXIT_CRITICAL(&mux); }
};
#endif

template <typename Cmd, typename Result, size_t N, typename Lock>
class Ring {
  static_assert(N > 0 && N < 256, "a ring holds 1..255 commands");
  static_assert(std::is_trivially_copyable<Cmd>::value &&
                    std::is_trivially_copyable<Result>::value,
                "commands and results are copied in and out under the lock");

 public:
  /* first_ticket: the first ticket post() hands out (a host test starts it
   * near the wrap). Tickets are never 0. */
  explicit Ring(uint32_t first_ticket = 1) : next_ticket_(first_ticket) {}

  /* Any task. The command's ticket, or 0 when every slot is taken. */
  uint32_t post(const Cmd& cmd) {
    Guard g(lock_);
    for (size_t i = 0; i < N; ++i) {
      Slot& s = slots_[i];
      if (s.state != kFree) continue;
      s.state = kQueued;
      s.ticket = take_ticket();
      s.cmd = cmd;
      return s.ticket;
    }
    return 0;
  }

  /* Any task. A done command's result is copied to *out (when out is not
   * null) and its slot freed: a ticket is collected once. */
  Poll poll(uint32_t ticket, Result* out) {
    Guard g(lock_);
    Slot* s = find(ticket);
    if (s == nullptr) return Poll::kUnknown;
    if (s->state == kQueued) return Poll::kQueued;
    if (s->state == kRunning) return Poll::kRunning;
    if (out != nullptr) *out = s->result;
    s->state = kFree;
    s->ticket = 0;
    return Poll::kDone;
  }

  /* Any task. True when the command had not started: it is gone and never
   * runs. False when it is running or done (or the ticket is unknown). */
  bool withdraw(uint32_t ticket) {
    Guard g(lock_);
    Slot* s = find(ticket);
    if (s == nullptr || s->state != kQueued) return false;
    s->state = kFree;
    s->ticket = 0;
    return true;
  }

  /* The loop task, and only it. Runs the queued commands, oldest ticket
   * first, through run(const Cmd&) -> Result, at most N per call (a command
   * posted meanwhile may run in this call or the next). Returns how many
   * ran. run() is called outside the lock. */
  template <typename Run>
  size_t drain(Run&& run) {
    size_t ran = 0;
    while (ran < N) {
      Slot* s = nullptr;
      Cmd cmd;
      {
        Guard g(lock_);
        s = oldest_queued();
        if (s == nullptr) break;
        s->state = kRunning;   /* from here no other task frees or reuses it */
        cmd = s->cmd;
      }
      const Result r = run(static_cast<const Cmd&>(cmd));
      {
        Guard g(lock_);
        s->result = r;
        s->state = kDone;
      }
      ++ran;
    }
    return ran;
  }

  /* Commands waiting for the loop task. */
  size_t queued() {
    Guard g(lock_);
    size_t n = 0;
    for (size_t i = 0; i < N; ++i) n += slots_[i].state == kQueued ? 1 : 0;
    return n;
  }

 private:
  enum State : uint8_t { kFree, kQueued, kRunning, kDone };
  struct Slot {
    State    state = kFree;
    uint32_t ticket = 0;
    Cmd      cmd = {};
    Result   result = {};
  };
  struct Guard {
    explicit Guard(Lock& l) : l_(l) { l_.lock(); }
    ~Guard() { l_.unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Lock& l_;
  };

  uint32_t take_ticket() {
    if (next_ticket_ == 0) next_ticket_ = 1;
    return next_ticket_++;
  }
  Slot* find(uint32_t ticket) {
    if (ticket == 0) return nullptr;
    for (size_t i = 0; i < N; ++i) {
      if (slots_[i].state != kFree && slots_[i].ticket == ticket) return &slots_[i];
    }
    return nullptr;
  }
  /* Ticket order across the wrap: a ticket is older when it is behind by
   * less than half the ticket space. */
  Slot* oldest_queued() {
    Slot* best = nullptr;
    for (size_t i = 0; i < N; ++i) {
      Slot& s = slots_[i];
      if (s.state != kQueued) continue;
      if (best == nullptr || (int32_t)(s.ticket - best->ticket) < 0) best = &s;
    }
    return best;
  }

  Slot     slots_[N];
  uint32_t next_ticket_;
  Lock     lock_;
};

/* Any task but the loop task (it would wait for itself, and the command
 * would be withdrawn unrun). Posts `cmd` and waits for its result: polls
 * every `step_ms` through now() (milliseconds) and sleep(step_ms). At
 * `timeout_ms` a command the loop task has not started is withdrawn; one it
 * has started is waited for until it is done. */
template <typename RingT, typename Cmd, typename Result, typename Now, typename Sleep>
Wait submit(RingT& ring, const Cmd& cmd, Result* result, uint32_t timeout_ms,
            uint32_t step_ms, Now&& now, Sleep&& sleep) {
  const uint32_t ticket = ring.post(cmd);
  if (ticket == 0) return Wait::kBusy;
  const uint32_t start = now();
  for (;;) {
    const Poll p = ring.poll(ticket, result);
    if (p == Poll::kDone) return Wait::kDone;
    /* Only this waiter frees its ticket's slot (by collecting or
     * withdrawing), so kUnknown cannot happen here; were it to, the command
     * is not queued any more and waiting would never end. */
    if (p == Poll::kUnknown) return Wait::kWithdrawn;
    if (p == Poll::kQueued && (uint32_t)(now() - start) >= timeout_ms &&
        ring.withdraw(ticket)) {
      return Wait::kWithdrawn;
    }
    sleep(step_ms);
  }
}

}  // namespace loop_command_ring

#endif  // SECURACV_LOOP_COMMAND_RING_H
