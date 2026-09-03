/*
 * securacv_witness_core.h — the C surface the Witness Wall's Swift code calls.
 *
 * Hand-written on purpose (it is five small functions), so there is no
 * bindgen step that can fail on a Mac without extra tooling. `WitnessWall/project.yml`
 * points SWIFT_INCLUDE_PATHS at this directory and the app imports it through
 * `Support/module.modulemap` — no bridging header, so the same import works
 * from the app target and the test target.
 *
 * Ownership, once: every non-NULL char* returned by scv_verify_sealed_log,
 * scv_parse_fleet and scv_normalize_source_host is owned by Rust and must be
 * handed back to scv_string_free exactly once. scv_core_version returns a
 * static string that must NOT be freed. Swift's WitnessCore.swift wraps all
 * of this so no call site has to remember.
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
 *     "devices": [ { "name", "online", "chain"?, "product"?, "hw"?, "hub"?,
 *                    "presence"?, "occupants"?, "breathing"?,
 *                    "seeing"?, "seeing_score"? } ] }
 * or, on bad input, { "error": "<why>" }.
 * `hw` is the board id (resolves the figure a client draws); `hub` is the
 * device's hub standing ("none" / "down" / "ok"); the wellbeing keys are the
 * contract's coarse room words ("clear"/"present", "0"/"1"/"2+", a breathing
 * lock held/lapsed, "person"/"vehicle"/"animal"/"package" with an optional
 * 0-100 score), carried verbatim — tolerance lives in the reader. All of
 * them pass through only when the device sent them — absent stays absent,
 * never a default: an absent wellbeing key means "cannot say", never an
 * empty calm room.
 *
 * Caller frees the result with scv_string_free.
 */
char *scv_parse_fleet(const char *json);

/*
 * Validate and normalize a Bonjour TXT `host` value into the hostname the
 * Wall may poll. The rule (witness-core/src/host.rs, the same gate the
 * iPhone's DeviceAPI.isPrivate applies): a bare DNS label, which comes back
 * qualified as "<label>.local"; a ".local" hostname of well-formed labels
 * ([A-Za-z0-9-], 1-63 chars, no leading/trailing hyphen); or a dotted-decimal
 * IPv4 address that is private (10/8, 172.16/12, 192.168/16, 169.254/16,
 * 127/8), returned as itself. The result is lower-cased.
 *
 * Returns NULL when the advert must be SKIPPED — a public name or address,
 * a malformed label, empty input, a NULL pointer, or non-UTF-8 bytes. This
 * is the one function here where NULL is an answer rather than an allocation
 * failure; both mean "do not poll it".
 *
 * Caller frees a non-NULL result with scv_string_free.
 */
char *scv_normalize_source_host(const char *host);

/*
 * The linked core's version, for the About/Health panel — so the TV can prove
 * which core it links rather than guessing. Static storage: do NOT free.
 */
const char *scv_core_version(void);

/*
 * Release a string returned by scv_verify_sealed_log, scv_parse_fleet or
 * scv_normalize_source_host. Passing NULL is allowed and does nothing. Never
 * pass scv_core_version's pointer, and never free the same pointer twice.
 */
void scv_string_free(char *ptr);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_WITNESS_CORE_H */
