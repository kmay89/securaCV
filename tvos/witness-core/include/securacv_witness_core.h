/*
 * securacv_witness_core.h — the C surface the Witness Wall's Swift code calls.
 *
 * Hand-written on purpose (it is five functions), so there is no bindgen step
 * that can fail on a Mac without extra tooling. `WitnessWall/project.yml`
 * points SWIFT_INCLUDE_PATHS at this directory and the app imports it through
 * `Support/module.modulemap` — no bridging header, so the same import works
 * from the app target and the test target.
 *
 * Ownership, once: every char* returned by scv_verify_sealed_log and
 * scv_parse_fleet is owned by Rust and must be handed back to scv_string_free
 * exactly once. scv_core_version returns a static string that must NOT be
 * freed. Swift's WitnessCore.swift wraps all of this so no call site has to
 * remember.
 *
 * Thread safety: every function is pure and holds no global state, so calls
 * from any thread (and concurrently) are safe.
 */
#ifndef SECURACV_WITNESS_CORE_H
#define SECURACV_WITNESS_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Verify a sealed-log document.
 *
 * `json` is a NUL-terminated UTF-8 document:
 *   { "verifying_key": "<64 hex>",
 *     "checkpoint_head": "<64 hex>"?,     // optional anchor
 *     "entries": [ { "id", "payload", "prev_hash", "entry_hash", "signature" } ] }
 *
 * Returns a NUL-terminated UTF-8 VerifyReport as JSON:
 *   { "ok", "verified", "head", "failed_at"?, "kind"?, "detail"?, "message" }
 *
 * Never returns a report-shaped lie: a null pointer, non-UTF-8 bytes, or
 * unparseable JSON all come back as an ok:false report with kind "malformed".
 * Returns NULL only if the result string could not be allocated.
 *
 * Caller frees the result with scv_string_free.
 */
char *scv_verify_sealed_log(const char *json);

/*
 * Parse a `GET /api/fleet` response (tvos/discovery/DISCOVERY.md).
 * Accepts the documented object form or a bare array of devices.
 *
 * Returns the normalized snapshot as JSON:
 *   { "kernel"?, "verified_through"?,
 *     "devices": [ { "name", "online", "chain"?, "product"?, "hw"?, "hub"? } ] }
 * or, on bad input, { "error": "<why>" }.
 * `hw` is the board id (resolves the figure a client draws); `hub` is the
 * device's hub standing ("none" / "down" / "ok"). Both pass through only
 * when the device sent them — absent stays absent, never a default.
 *
 * Caller frees the result with scv_string_free.
 */
char *scv_parse_fleet(const char *json);

/*
 * The linked core's version, for the About/Health panel — so the TV can prove
 * which core it links rather than guessing. Static storage: do NOT free.
 */
const char *scv_core_version(void);

/*
 * Release a string returned by scv_verify_sealed_log or scv_parse_fleet.
 * Passing NULL is allowed and does nothing. Never pass scv_core_version's
 * pointer, and never free the same pointer twice.
 */
void scv_string_free(char *ptr);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_WITNESS_CORE_H */
