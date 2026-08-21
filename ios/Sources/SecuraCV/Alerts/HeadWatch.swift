// HeadWatch.swift
//
// The lightning half of "lightning-fast alerts" on the LAN.
//
// The full refresh polls every 20 seconds; the liveness sentinel already
// probes every 5. This gives the sentinel eyes for EVENTS, not just life: a
// one-record witness-head read per paired Canary rides the same 5-second
// pass, and the moment any head sequence moves, the sentinel triggers a full
// refresh instead of letting the news wait out the tail of the 20-second
// cycle. Worst-case LAN latency for a new event drops from ~20 s to ~5 s,
// with no cloud, no push, and one tiny authenticated GET as the cost.
//
// Pure state machine, host-tested. Two rules keep it honest:
//   * FIRST SIGHT IS NOT NEWS. The first head we ever see for a device is
//     the baseline — pairing a Canary with 4,000 chain records must not
//     read as 4,000 fresh events.
//   * ANY MOVEMENT IS NEWS, including backwards. A head that went DOWN
//     means the chain restarted (a wipe, a reset) — exactly the kind of
//     thing worth a full refresh and a verify, never something to ignore
//     because it wasn't an increment.

import Foundation

struct HeadWatch: Sendable {
    private(set) var seqs: [String: UInt32] = [:]

    /// Record the freshly-read head for a device; true when it moved.
    mutating func hasNews(id: String, headSeq: UInt32) -> Bool {
        defer { seqs[id] = headSeq }
        guard let known = seqs[id] else { return false }   // baseline, not news
        return known != headSeq
    }

    /// A device that unpaired must not leave a stale baseline behind — if it
    /// re-pairs later, its first head should be a baseline again.
    mutating func forget(id: String) {
        seqs[id] = nil
    }
}
