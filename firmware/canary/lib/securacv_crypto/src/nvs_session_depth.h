/*
 * SecuraCV Canary — NvsManager session depth, the pure half.
 *
 * NvsManager (securacv_crypto.h) is one Preferences handle on the "securacv"
 * namespace, shared by every task that calls it. Three tasks open sessions on
 * it after setup: the loop (the MQTT reload, the witness chain persist, the
 * birth stamp, factory reset), the one httpd task that serves the API (MQTT
 * status, config and CA; Wi-Fi connect and disconnect; the reboot's chain
 * persist) and the pull-OTA task (the chain persist before its reboot). The
 * handle used to carry a bare open flag, so a session ending on one task
 * closed the handle under the other: a status poll during a reload could
 * read the broker host as empty and leave MQTT off, and a write racing that
 * end() could land nothing while nvs_store_bytes() still returned true.
 *
 * The fix is in NvsManager, not its callers: a recursive FreeRTOS mutex is
 * taken in begin() and held until the matching end(), so a session belongs
 * to one task at a time. Holding a lock across a session makes nesting on
 * one task a question too — the lock itself is recursive, so the second
 * begin() does not deadlock, but the first end() must not close the handle
 * the outer session is still using. That is this header: a depth count and
 * what begin() / end() do to the Preferences handle at each depth. It is
 * the arithmetic only, with no Arduino, no FreeRTOS and no NVS, so
 * firmware/tests_host/test_nvs_session_depth.cpp proves it on the host, and
 * test_nvs_manager_lock.cpp runs NvsManager's own begin() and end() over it
 * against a fake recursive mutex. The FreeRTOS mutex itself is
 * compile-tested by CI's canary envs and has not run on a bench.
 *
 * No caller nests today (every one of the canary's NvsManager sessions is a
 * begin, NVS reads or writes, and its end). The recursive lock and the count
 * make a future nested helper safe: it neither stalls 2 s on its own lock
 * nor closes the handle under the session it runs inside.
 *
 * THE RULES, per call, for the task that holds the lock:
 *
 *   begin(ro)  depth 0, or the handle is closed  -> Open (begin as asked)
 *              read-only handle, read-write asked -> ReopenRw (end, begin RW)
 *              otherwise                          -> Keep (RO inside RW keeps
 *                                                    the RW handle)
 *              depth at kMaxDepth                 -> Refuse
 *   end()      depth 0                            -> Unbalanced (no change)
 *              depth > 1                          -> Keep (depth - 1)
 *              the outermost end                  -> Close, if the handle is
 *                                                    open (else Keep)
 *
 * A failed begin leaves the depth where it was, so the caller that got false
 * never calls end() (none does). A failed ReopenRw has already ended the
 * outer session's handle, so it is marked closed and the next begin at any
 * depth opens it again rather than keeping a handle that is not there.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#pragma once

#include <stdint.h>

namespace nvs_session {

// Deeper than any caller could need (none nests today); a bound so a
// runaway recursion refuses instead of wrapping the uint8_t count.
constexpr uint8_t kMaxDepth = 8;

// How long begin() waits for another task's session before it fails soft
// (returns false, as a failed Preferences::begin always could). The same
// 2 s the camera lifecycle lock waits (securacv_camera.cpp), well under the
// 8 s task watchdog the loop is subscribed to; securacv_crypto.cpp holds it
// under WATCHDOG_TIMEOUT_SEC with a static_assert. That bounds one wait, not
// a loop pass: a pass that meets a leaked session on several calls can
// still outlast the watchdog (whose reset then frees the leaked session).
constexpr uint32_t kSessionWaitMs = 2000;

// What begin() does to the Preferences handle.
enum class Begin : uint8_t {
  Open,      // Preferences::begin(ns, ro)
  ReopenRw,  // Preferences::end(), then Preferences::begin(ns, false)
  Keep,      // nothing: the handle already serves this request
  Refuse,    // nothing, and begin() returns false
};

// What end() does to the Preferences handle.
enum class End : uint8_t {
  Close,       // Preferences::end(): the outermost session closed
  Keep,        // nothing: an outer session is still open, or the handle
               // is already closed
  Unbalanced,  // nothing: there was no session to end
};

struct State {
  uint8_t depth = 0;       // sessions open on the holding task
  bool open = false;       // the Preferences handle is open
  bool read_only = false;  // ...and read-only (meaningful only when open)
};

// Decide what a begin(read_only) does. Pure: the caller acts on the answer,
// then reports how it went through commit_begin().
inline Begin on_begin(const State& s, bool read_only) {
  if (s.depth >= kMaxDepth) return Begin::Refuse;
  if (s.depth == 0 || !s.open) return Begin::Open;
  if (s.read_only && !read_only) return Begin::ReopenRw;
  return Begin::Keep;
}

// Record the outcome of the action on_begin() chose. `ok` is whether the
// Preferences call succeeded (Keep always succeeds). A success adds one
// session; a failure adds none and, unless nothing was touched, leaves the
// handle marked closed. Returns whether a session was added: true means
// begin() returns true and KEEPS the lock it took until the matching end();
// false means begin() gives that take back and returns false.
inline bool commit_begin(State& s, Begin action, bool read_only, bool ok) {
  if (action == Begin::Refuse) return false;
  if (!ok) {
    if (action != Begin::Keep) {
      s.open = false;
      s.read_only = false;
    }
    return false;
  }
  s.depth++;
  if (action == Begin::Open) {
    s.open = true;
    s.read_only = read_only;
  } else if (action == Begin::ReopenRw) {
    s.open = true;
    s.read_only = false;
  }
  return true;
}

// One end(): drop a session and say what happens to the handle.
inline End on_end(State& s) {
  if (s.depth == 0) return End::Unbalanced;
  s.depth--;
  if (s.depth > 0) return End::Keep;
  const bool was_open = s.open;
  s.open = false;
  s.read_only = false;
  return was_open ? End::Close : End::Keep;
}

// How many times end() gives the recursive mutex back, after the zero-wait
// take that proved this task holds it (or that nobody does). Once for that
// take, and once more for the take its matching begin() kept, unless there
// was no session to end. So the lock's recursion count is the session depth
// after every call, and the lock is free exactly when the depth is 0 — the
// invariant end()'s zero-wait take relies on to tell "my session" from
// "another task's session". test_nvs_session_depth.cpp drives it across
// three tasks through a model; test_nvs_manager_lock.cpp does the same with
// securacv_crypto.cpp's own begin() and end().
inline uint8_t end_gives(End e) {
  return e == End::Unbalanced ? 1 : 2;
}

}  // namespace nvs_session
