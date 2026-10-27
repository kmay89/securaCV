/*
 * Host-build test for the securacv_ota engine's pure-logic subset.
 *
 * Compiles standalone (no ESP-IDF / Arduino deps):
 *
 *   g++ -std=c++17 -DSECURACV_OTA_HOST_BUILD -Wall -Wextra \
 *       firmware/common/ota/test_ota_logic.cpp \
 *       firmware/common/ota/src/securacv_ota.cpp \
 *       -I firmware/common/ota/src -o /tmp/ota_logic && /tmp/ota_logic
 *
 * Covers:
 *   1. Semantic version compare
 *   2. Update decision vs the anti-rollback floor (NVS + running version)
 *   3. URL transport policy (https always; http only local + opted-in)
 *   4. Signed-message layout — must match ble_ota.cpp and ota_release.py
 *   5. Hex parsing strictness
 *   6. Friendly UI strings exist for every state/error (no raw jargon
 *      leaks into the primary UI when new codes are added)
 *   7. Canonical manifest-signature message — byte-identical to
 *      ota_release.py (shared fixture in test_ota_release.py)
 *   8. Inactive-slot pre-check — an image an old partition table's slot
 *      cannot hold is refused before any download (fw-v2.4.16 Vision C3)
 *   9. Manifest fields → struct: every field whole or refused, never cut
 *      (securacv_ota_manifest_from_fields)
 *  10. Every product id the build files declare fits product[] and comes
 *      back out of parse + the signed message byte for byte (reads
 *      firmware/flavors.json and every .ini in firmware/envs/platformio, so
 *      run it from anywhere inside the checkout)
 *  11. The pending-marker rule: kept while the image is pending, handed back
 *      once it is confirmed, handed back on a rollback boot
 */

#include "securacv_ota.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static int tests_run = 0;

#define CHECK(cond) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static int test_version_compare()
{
    CHECK(securacv_version_compare("1.0.0", "1.0.1") == -1);
    CHECK(securacv_version_compare("1.2.0", "1.1.9") == 1);
    CHECK(securacv_version_compare("2.0.0", "1.9.9") == 1);
    CHECK(securacv_version_compare("2.1.0", "2.1.0") == 0);
    CHECK(securacv_version_compare("10.0.0", "9.9.9") == 1);   // numeric, not lexicographic
    CHECK(securacv_version_compare("1.0", "1.0.0") == 0);      // patch defaults to 0
    CHECK(securacv_version_compare("garbage", "1.0.0") == 0);  // unparseable -> equal (no update)
    CHECK(securacv_version_compare(NULL, "1.0.0") == 0);

    // Prerelease ranking (release channels, docs/RELEASE_PROCESS.md):
    // stable > rc.N > dev.N at an equal triple; numeric within a band.
    CHECK(securacv_version_compare("2.3.0", "2.3.0-dev.1") == 1);        // promotion is an update
    CHECK(securacv_version_compare("2.3.0-dev.1", "2.3.0") == -1);
    CHECK(securacv_version_compare("2.3.0-rc.1", "2.3.0-dev.9") == 1);   // rc outranks dev
    CHECK(securacv_version_compare("2.3.0-dev.2", "2.3.0-dev.1") == 1);  // numeric within band
    CHECK(securacv_version_compare("2.3.0-dev.10", "2.3.0-dev.9") == 1); // numeric, not lexicographic
    CHECK(securacv_version_compare("2.3.0-rc.2", "2.3.0-rc.2") == 0);
    CHECK(securacv_version_compare("2.4.0-dev.1", "2.3.0") == 1);        // triple still dominates
    // Variant suffixes are NOT prerelease markers.
    CHECK(securacv_version_compare("2.2.0-wap", "2.2.0") == 0);
    CHECK(securacv_version_compare("2.3.0-wap", "2.3.0-wap.dev.2") == 1); // wap dev build ranks below
    CHECK(securacv_version_compare("2.3.0-wap.dev.2", "2.3.0-wap.dev.1") == 1);
    return 0;
}

static int test_update_decision()
{
    // Plain upgrade.
    CHECK(securacv_ota_update_decision("2.2.0", "2.1.0", "") == SECURACV_OTA_DECISION_UPDATE);
    // Same version.
    CHECK(securacv_ota_update_decision("2.1.0", "2.1.0", "") == SECURACV_OTA_DECISION_UP_TO_DATE);
    // Downgrade vs running version.
    CHECK(securacv_ota_update_decision("2.0.0", "2.1.0", "") == SECURACV_OTA_DECISION_ROLLBACK);
    // NVS floor is higher than running (physical rollback happened):
    // a manifest equal to running but below the floor must be rejected.
    CHECK(securacv_ota_update_decision("2.1.0", "2.1.0", "2.3.0") == SECURACV_OTA_DECISION_ROLLBACK);
    // Manifest above the floor wins.
    CHECK(securacv_ota_update_decision("2.4.0", "2.1.0", "2.3.0") == SECURACV_OTA_DECISION_UPDATE);
    // Floor equal to manifest -> up to date, not update (replay of the
    // exact floor version is not an upgrade).
    CHECK(securacv_ota_update_decision("2.3.0", "2.1.0", "2.3.0") == SECURACV_OTA_DECISION_UP_TO_DATE);
    // NULL floor behaves like empty.
    CHECK(securacv_ota_update_decision("2.2.0", "2.1.0", NULL) == SECURACV_OTA_DECISION_UPDATE);
    return 0;
}

static int test_image_fits_slot()
{
    // The incident, in its own numbers: fw-v2.4.16's Vision C3 images
    // (canary-vision, canary-vision-xiao-c3) against the 0x140000
    // default.csv slots every factory image fw-v2.2.0..fw-v2.4.15 laid down.
    // The engine used to download and write until the slot ran out, then
    // try again on every poll; now it refuses before the download.
    CHECK(!securacv_ota_image_fits_slot(1400368, 0x140000));
    CHECK(!securacv_ota_image_fits_slot(1409952, 0x140000));
    // ...and the min_spiffs.csv 0x1E0000 slots fw-v2.4.16's factory image
    // lays down take them.
    CHECK(securacv_ota_image_fits_slot(1400368, 0x1E0000));
    CHECK(securacv_ota_image_fits_slot(1409952, 0x1E0000));

    // Boundary: exactly full fits, one byte over does not.
    CHECK(securacv_ota_image_fits_slot(0x140000, 0x140000));
    CHECK(!securacv_ota_image_fits_slot(0x140000 + 1, 0x140000));

    // No inactive slot (none found, or a single-app table where IDF hands
    // back the running partition): refused whatever the size.
    CHECK(!securacv_ota_image_fits_slot(1, 0));
    CHECK(!securacv_ota_image_fits_slot(0, 0));

    // An undeclared size cannot be pre-checked: not refused here (the
    // post-download length check and the write bound still apply).
    CHECK(securacv_ota_image_fits_slot(0, 0x140000));

    // Top of the range: compared as-is, no arithmetic to overflow.
    CHECK(securacv_ota_image_fits_slot(UINT32_MAX, UINT32_MAX));
    CHECK(!securacv_ota_image_fits_slot(UINT32_MAX, UINT32_MAX - 1));

    // The refusal must tell the owner the one thing that works. Retrying
    // never does (the slot size is fixed by the USB flash), so the UI
    // sentence names USB rather than "try again".
    const char *msg = securacv_ota_friendly_error(SECURACV_OTA_ERR_PARTITION);
    CHECK(msg != NULL && strstr(msg, "USB") != NULL);
    CHECK(strstr(msg, "again") == NULL);
    return 0;
}

static int test_url_policy()
{
    // https is always allowed, opt-in or not.
    CHECK(securacv_ota_url_allowed("https://github.com/x/releases/latest/download/m.json", false));
    CHECK(securacv_ota_url_allowed("https://192.168.1.10:8443/m.json", false));

    // http to public hosts: never.
    CHECK(!securacv_ota_url_allowed("http://github.com/m.json", false));
    CHECK(!securacv_ota_url_allowed("http://github.com/m.json", true));
    CHECK(!securacv_ota_url_allowed("http://8.8.8.8/m.json", true));

    // http to private hosts: only with the opt-in flag.
    CHECK(!securacv_ota_url_allowed("http://192.168.1.10/m.json", false));
    CHECK(securacv_ota_url_allowed("http://192.168.1.10/m.json", true));
    CHECK(securacv_ota_url_allowed("http://10.0.0.5:8000/m.json", true));
    CHECK(securacv_ota_url_allowed("http://172.16.0.1/m.json", true));
    CHECK(securacv_ota_url_allowed("http://172.31.255.254/m.json", true));
    CHECK(!securacv_ota_url_allowed("http://172.32.0.1/m.json", true));   // outside 172.16/12
    CHECK(securacv_ota_url_allowed("http://127.0.0.1:8070/m.json", true));
    CHECK(securacv_ota_url_allowed("http://169.254.1.1/m.json", true));
    CHECK(securacv_ota_url_allowed("http://localhost:8070/m.json", true));

    // Private-use name suffixes.
    CHECK(securacv_ota_url_allowed("http://homeassistant.local:8123/m.json", true));
    CHECK(securacv_ota_url_allowed("http://nas.lan/m.json", true));
    CHECK(securacv_ota_url_allowed("http://updates.internal/m.json", true));
    CHECK(securacv_ota_url_allowed("http://hub.home.arpa/m.json", true));
    CHECK(!securacv_ota_url_allowed("http://evil.example.com/m.json", true));
    // ".local" must be a suffix match on the host, not a substring.
    CHECK(!securacv_ota_url_allowed("http://evil.localhost.example.com/m.json", true));
    CHECK(!securacv_ota_url_allowed("http://example.com/x.local/m.json", true));

    // Garbage in.
    CHECK(!securacv_ota_url_allowed("", true));
    CHECK(!securacv_ota_url_allowed(NULL, true));
    CHECK(!securacv_ota_url_allowed("ftp://192.168.1.1/m.json", true));
    CHECK(!securacv_ota_url_allowed("http://", true));
    return 0;
}

static int test_signed_message_layout()
{
    // The contract shared with ble_ota.cpp and ota_release.py:
    // message = image_size as uint32 LE || sha256 (32 bytes), 36 total.
    uint8_t sha[32];
    for (int i = 0; i < 32; i++) sha[i] = (uint8_t)(i + 1);

    uint8_t msg[36];
    securacv_ota_build_signed_message(0x12345678, sha, msg);

    CHECK(msg[0] == 0x78);
    CHECK(msg[1] == 0x56);
    CHECK(msg[2] == 0x34);
    CHECK(msg[3] == 0x12);
    CHECK(memcmp(msg + 4, sha, 32) == 0);

    securacv_ota_build_signed_message(1, sha, msg);
    CHECK(msg[0] == 0x01 && msg[1] == 0x00 && msg[2] == 0x00 && msg[3] == 0x00);
    return 0;
}

static int test_hex_to_bytes()
{
    uint8_t out[4];
    CHECK(securacv_ota_hex_to_bytes("deadBEEF", out, 4));
    CHECK(out[0] == 0xde && out[1] == 0xad && out[2] == 0xbe && out[3] == 0xef);

    CHECK(!securacv_ota_hex_to_bytes("dead", out, 4));      // too short
    CHECK(!securacv_ota_hex_to_bytes("deadbeef00", out, 4)); // too long
    CHECK(!securacv_ota_hex_to_bytes("deadbeeg", out, 4));   // invalid char
    CHECK(!securacv_ota_hex_to_bytes(NULL, out, 4));

    // A 64-byte signature round-trip length check.
    uint8_t sig[64];
    char hex[129];
    for (int i = 0; i < 128; i++) hex[i] = "0123456789abcdef"[i % 16];
    hex[128] = '\0';
    CHECK(securacv_ota_hex_to_bytes(hex, sig, 64));
    return 0;
}

static int test_manifest_message_cross_language_fixture()
{
    // Shared with firmware/scripts/test_ota_release.py — if either
    // implementation drifts, devices reject every release manifest.
    securacv_ota_manifest_t m;
    memset(&m, 0, sizeof(m));
    strcpy(m.product, "securacv-canary");
    strcpy(m.version, "2.2.0");
    strcpy(m.min_version, "2.1.0");
    strcpy(m.url, "https://example.com/fw.bin");
    strcpy(m.sha256, "AAbb");  // mixed case: builder must canonicalize lower
    for (int i = 4; i < 64; i++) m.sha256[i] = '0';
    m.sha256[64] = '\0';
    m.size = 123456;
    strcpy(m.release_notes, "Notes, with punctuation!");
    strcpy(m.release_url, "https://example.com/notes");

    static const char expected_hex[] =
        "7363762d6d616e69666573742d76310073656375726163762d63616e61727900322e322e3000322e312e300068747470733a2f2f6578616d706c652e636f6d2f66772e62696e006161626230303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303000313233343536004e6f7465732c20776974682070756e6374756174696f6e210068747470733a2f2f6578616d706c652e636f6d2f6e6f74657300";

    uint8_t msg[SECURACV_OTA_MANIFEST_MSG_MAX];
    size_t msg_len = 0;
    CHECK(securacv_ota_build_manifest_message(&m, msg, sizeof(msg), &msg_len));
    CHECK(msg_len == strlen(expected_hex) / 2);

    uint8_t expected[sizeof(msg)];
    CHECK(securacv_ota_hex_to_bytes(expected_hex, expected, msg_len));
    CHECK(memcmp(msg, expected, msg_len) == 0);

    // Too-small buffer must fail cleanly, never truncate-and-sign.
    uint8_t tiny[16];
    size_t tiny_len = 0;
    CHECK(!securacv_ota_build_manifest_message(&m, tiny, sizeof(tiny), &tiny_len));
    return 0;
}

static int test_manifest_v2_message_and_rules()
{
    // manifest_version 2 appends the board and chip under its own prefix —
    // byte-identical to ota_release.py's manifest_signed_message(...,
    // manifest_version=2); the Python test pins the same shape.
    securacv_ota_manifest_t m;
    memset(&m, 0, sizeof(m));
    m.manifest_version = 2;
    strcpy(m.product, "securacv-canary-sense");
    strcpy(m.version, "2.5.0");
    strcpy(m.min_version, "2.4.0");
    strcpy(m.url, "https://example.com/sense.bin");
    strcpy(m.sha256, "AABB");
    for (int i = 4; i < 64; i++) m.sha256[i] = '0';
    m.sha256[64] = '\0';
    m.size = 123456;
    strcpy(m.board, "xiao-esp32c6-mr60");
    strcpy(m.chip, "esp32c6");

    static const char expected_hex[] = "7363762d6d616e69666573742d76320073656375726163762d63616e6172792d73656e736500322e352e3000322e342e300068747470733a2f2f6578616d706c652e636f6d2f73656e73652e62696e0061616262303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030003132333435360000007869616f2d657370333263362d6d723630006573703332633600";
    uint8_t msg[SECURACV_OTA_MANIFEST_MSG_MAX];
    size_t msg_len = 0;
    CHECK(securacv_ota_build_manifest_message(&m, msg, sizeof(msg), &msg_len));
    CHECK(msg_len == strlen(expected_hex) / 2);
    uint8_t expected[sizeof(msg)];
    CHECK(securacv_ota_hex_to_bytes(expected_hex, expected, msg_len));
    CHECK(memcmp(msg, expected, msg_len) == 0);

    // A v1 manifest (manifest_version 0 = absent, or 1) keeps the v1 prefix.
    m.manifest_version = 1;
    CHECK(securacv_ota_build_manifest_message(&m, msg, sizeof(msg), &msg_len));
    CHECK(memcmp(msg, "scv-manifest-v1", 15) == 0);
    m.manifest_version = 3;
    CHECK(!securacv_ota_build_manifest_message(&m, msg, sizeof(msg), &msg_len));
    m.manifest_version = 2;

    // The board rule: each named field must equal the build's; an empty
    // manifest field binds nothing; a build that cannot name its own passes.
    CHECK(securacv_ota_board_ok(&m, "xiao-esp32c6-mr60", "esp32c6"));
    CHECK(!securacv_ota_board_ok(&m, "xiao-esp32s3-sense", "esp32c6"));
    CHECK(!securacv_ota_board_ok(&m, "xiao-esp32c6-mr60", "esp32s3"));
    CHECK(securacv_ota_board_ok(&m, NULL, NULL));
    CHECK(securacv_ota_board_ok(&m, "", ""));
    m.board[0] = '\0';
    CHECK(securacv_ota_board_ok(&m, "anything", "esp32c6"));
    m.manifest_version = 1;
    strcpy(m.board, "xiao-esp32c6-mr60");
    CHECK(securacv_ota_board_ok(&m, "xiao-esp32s3-sense", "esp32s3"));  // v1: no binding
    CHECK(!securacv_ota_board_ok(NULL, "x", "y"));

    // The floor rule, enforced: below min_version refuses; no floor is no floor.
    CHECK(securacv_ota_min_version_ok("2.4.0", "2.4.0"));
    CHECK(securacv_ota_min_version_ok("2.4.1", "2.4.0"));
    CHECK(!securacv_ota_min_version_ok("2.3.9", "2.4.0"));
    CHECK(securacv_ota_min_version_ok("1.0.0", ""));
    CHECK(securacv_ota_min_version_ok("1.0.0", NULL));
    CHECK(!securacv_ota_min_version_ok(NULL, "2.4.0"));
    CHECK(!securacv_ota_min_version_ok("", "2.4.0"));

    // The lowercase state words every node's status body speaks.
    CHECK(strcmp(securacv_ota_state_word(SECURACV_OTA_IDLE), "idle") == 0);
    CHECK(strcmp(securacv_ota_state_word(SECURACV_OTA_CHECKING), "checking") == 0);
    CHECK(strcmp(securacv_ota_state_word(SECURACV_OTA_DOWNLOADING), "downloading") == 0);
    CHECK(strcmp(securacv_ota_state_word(SECURACV_OTA_VERIFYING), "verifying") == 0);
    CHECK(strcmp(securacv_ota_state_word(SECURACV_OTA_FLASHING), "flashing") == 0);
    CHECK(strcmp(securacv_ota_state_word(SECURACV_OTA_REBOOTING), "rebooting") == 0);
    CHECK(strcmp(securacv_ota_state_word(SECURACV_OTA_ERROR), "error") == 0);
    CHECK(strcmp(securacv_ota_state_word((securacv_ota_state_t)99), "unknown") == 0);
    return 0;
}


// The confirm dwell as the pure core sees it: the order a board calls
// boot_self_test() and init() in must not change when the image is confirmed.
static int test_confirm_dwell()
{
    int fails = 0;
    const uint32_t DWELL = 60000;

    // Every board's order: probes first, init later, with a dwell.
    {
        securacv_ota_dwell_t d = {false, false, 0};
        CHECK(securacv_ota_dwell_probes_passed(&d, /*initialized=*/false, 0) == SECURACV_OTA_DWELL_WAIT);  // probes before init: never confirm at the probe (the dwell is unknown)
        CHECK(d.probes_passed);  // probes before init: armed
        CHECK(securacv_ota_dwell_tick(&d, 1000, true, 0, true) == SECURACV_OTA_DWELL_IDLE);  // a tick before init (dwell still 0) decides nothing
        CHECK(d.probes_passed);  // ...and does not disarm
        CHECK(securacv_ota_dwell_init(&d, DWELL) == SECURACV_OTA_DWELL_WAIT);  // init with a dwell: wait for the ticks
        CHECK(securacv_ota_dwell_tick(&d, 2000, false, DWELL, true) == SECURACV_OTA_DWELL_WAIT);  // unhealthy: wait
        CHECK(!d.dwell_started);  // unhealthy: the dwell has not started
        CHECK(securacv_ota_dwell_tick(&d, 3000, true, DWELL, true) == SECURACV_OTA_DWELL_WAIT);  // first healthy pass starts the dwell
        CHECK(d.dwell_started && d.healthy_since_ms == 3000);  // started at the first healthy pass
        CHECK(securacv_ota_dwell_tick(&d, 3000 + DWELL - 1, true, DWELL, true) == SECURACV_OTA_DWELL_WAIT);  // one ms short: wait
        CHECK(securacv_ota_dwell_tick(&d, 3000 + DWELL, true, DWELL, true) == SECURACV_OTA_DWELL_CONFIRM);  // the dwell held: confirm
        CHECK(d.probes_passed);  // CONFIRM leaves the state armed until the mark lands
        CHECK(securacv_ota_dwell_tick(&d, 3000 + DWELL + 1, true, DWELL, true) == SECURACV_OTA_DWELL_CONFIRM);  // a failed mark is retried
        securacv_ota_dwell_clear(&d);
        CHECK(securacv_ota_dwell_tick(&d, 3000 + DWELL + 2, true, DWELL, true) == SECURACV_OTA_DWELL_IDLE);  // cleared: idle
    }
    // An unhealthy pass inside the dwell restarts it.
    {
        securacv_ota_dwell_t d = {false, false, 0};
        securacv_ota_dwell_probes_passed(&d, false, 0);
        securacv_ota_dwell_init(&d, DWELL);
        securacv_ota_dwell_tick(&d, 0, true, DWELL, true);
        CHECK(securacv_ota_dwell_tick(&d, 30000, false, DWELL, true) == SECURACV_OTA_DWELL_WAIT);  // link lost at 30 s
        CHECK(securacv_ota_dwell_tick(&d, 31000, true, DWELL, true) == SECURACV_OTA_DWELL_WAIT);  // link back at 31 s: the dwell restarts
        CHECK(securacv_ota_dwell_tick(&d, 60000, true, DWELL, true) == SECURACV_OTA_DWELL_WAIT);  // 60 s after boot is not 60 s of health
        CHECK(securacv_ota_dwell_tick(&d, 31000 + DWELL, true, DWELL, true) == SECURACV_OTA_DWELL_CONFIRM);  // 60 s after the link came back: confirm
    }
    // Probes before init, no dwell configured: init confirms at once (legacy).
    {
        securacv_ota_dwell_t d = {false, false, 0};
        CHECK(securacv_ota_dwell_probes_passed(&d, false, 0) == SECURACV_OTA_DWELL_WAIT);  // probes before init wait
        CHECK(securacv_ota_dwell_init(&d, 0) == SECURACV_OTA_DWELL_CONFIRM);  // init with dwell 0 confirms the deferred image
        securacv_ota_dwell_clear(&d);
        CHECK(securacv_ota_dwell_init(&d, 0) == SECURACV_OTA_DWELL_IDLE);  // nothing pending: init has nothing to confirm
    }
    // Init first (the old canary order): a zero dwell confirms at the probe, a dwell waits.
    {
        securacv_ota_dwell_t d = {false, false, 0};
        CHECK(securacv_ota_dwell_probes_passed(&d, true, 0) == SECURACV_OTA_DWELL_CONFIRM);  // init first, dwell 0: confirm at the probe
        CHECK(!d.probes_passed);  // ...and nothing stays armed
        CHECK(securacv_ota_dwell_probes_passed(&d, true, DWELL) == SECURACV_OTA_DWELL_WAIT);  // init first, a dwell: wait
        CHECK(securacv_ota_dwell_tick(&d, 10, true, DWELL, true) == SECURACV_OTA_DWELL_WAIT);  // starts
        CHECK(securacv_ota_dwell_tick(&d, 10 + DWELL, true, DWELL, true) == SECURACV_OTA_DWELL_CONFIRM);  // confirms after the dwell
    }
    // The image stopped being pending underneath (confirmed elsewhere): disarm.
    {
        securacv_ota_dwell_t d = {false, false, 0};
        securacv_ota_dwell_probes_passed(&d, false, 0);
        securacv_ota_dwell_init(&d, DWELL);
        CHECK(securacv_ota_dwell_tick(&d, 5, true, DWELL, /*still_pending=*/false) == SECURACV_OTA_DWELL_IDLE);  // not pending: idle
        CHECK(!d.probes_passed);  // not pending: disarmed
    }
    // millis() wrap inside the dwell.
    {
        securacv_ota_dwell_t d = {false, false, 0};
        securacv_ota_dwell_probes_passed(&d, false, 0);
        securacv_ota_dwell_init(&d, DWELL);
        const uint32_t t0 = 0xFFFFFF00u;
        securacv_ota_dwell_tick(&d, t0, true, DWELL, true);
        CHECK(securacv_ota_dwell_tick(&d, t0 + DWELL - 1, true, DWELL, true) == SECURACV_OTA_DWELL_WAIT);  // across the wrap, one ms short
        CHECK(securacv_ota_dwell_tick(&d, t0 + DWELL, true, DWELL, true) == SECURACV_OTA_DWELL_CONFIRM);  // across the wrap, the dwell held
    }
    return fails;
}

static int test_friendly_strings()
{
    // Every error code needs a usable primary-UI sentence; raw enum names
    // and hex codes must never reach the user. Iterate the WHOLE enum via
    // the SECURACV_OTA_ERR__COUNT sentinel — a hand-copied list here once
    // silently missed three newer codes (MANIFEST_SIG, NOT_INITIALIZED,
    // OUT_OF_MEMORY), which is exactly the drift this test claims to stop.
    const char *banned[] = {"OTA", "SHA", "Ed25519", "NVS", "manifest",
                            "partition", "0x", "esp_"};

    for (int i = SECURACV_OTA_ERR_NONE + 1; i < SECURACV_OTA_ERR__COUNT; i++) {
        const securacv_ota_error_t e = (securacv_ota_error_t)i;
        const char *msg = securacv_ota_friendly_error(e);
        CHECK(msg != NULL && strlen(msg) > 10);
        for (const char *word : banned) {
            CHECK(strstr(msg, word) == NULL);
        }
    }

    const securacv_ota_state_t states[] = {
        SECURACV_OTA_CHECKING, SECURACV_OTA_DOWNLOADING,
        SECURACV_OTA_VERIFYING, SECURACV_OTA_FLASHING,
        SECURACV_OTA_REBOOTING, SECURACV_OTA_ERROR,
    };
    for (securacv_ota_state_t s : states) {
        const char *msg = securacv_ota_friendly_state(s);
        CHECK(msg != NULL && strlen(msg) > 5);
        for (const char *word : banned) {
            CHECK(strstr(msg, word) == NULL);
        }
    }
    return 0;
}

static int test_channel_manifest_url()
{
    char buf[160];

    // DEV rewrites exactly the canonical release segment, keeping the
    // product manifest name — the published dev channel mirrors every
    // per-product manifest under fw-dev-latest with the same filenames.
    CHECK(securacv_ota_channel_manifest_url(
        "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-display-dash7.json",
        SECURACV_OTA_CHANNEL_DEV, buf, sizeof(buf)));
    CHECK(strcmp(buf,
        "https://github.com/kmay89/securaCV/releases/download/fw-dev-latest/manifest-canary-display-dash7.json") == 0);

    // RELEASE derives nothing: the compiled-in default IS the release channel.
    CHECK(!securacv_ota_channel_manifest_url(
        "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary.json",
        SECURACV_OTA_CHANNEL_RELEASE, buf, sizeof(buf)));

    // A custom server has no GitHub channel structure to point into —
    // callers stay on the configured URL rather than guessing.
    CHECK(!securacv_ota_channel_manifest_url(
        "https://updates.example.com/canary/manifest.json",
        SECURACV_OTA_CHANNEL_DEV, buf, sizeof(buf)));

    // An undersized buffer refuses instead of truncating a fetch URL.
    char tiny[32];
    CHECK(!securacv_ota_channel_manifest_url(
        "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary.json",
        SECURACV_OTA_CHANNEL_DEV, tiny, sizeof(tiny)));

    CHECK(!securacv_ota_channel_manifest_url(NULL, SECURACV_OTA_CHANNEL_DEV, buf, sizeof(buf)));
    CHECK(!securacv_ota_channel_manifest_url("x", SECURACV_OTA_CHANNEL_DEV, NULL, 0));

    return 0;
}

// ── Manifest fields → struct (securacv_ota_manifest_from_fields) ─────────────

static const char k_sha_hex[] = "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90";
static const char k_sig_hex[] =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

// A manifest the release workflow could have written, as the parser hands it over.
static securacv_ota_manifest_fields_t release_fields(const char *product)
{
    securacv_ota_manifest_fields_t f;
    memset(&f, 0, sizeof(f));
    f.product = product;
    f.version = "2.4.17";
    f.min_version = "2.4.0";
    f.url = "https://github.com/kmay89/securaCV/releases/download/fw-v2.4.17/canary-2.4.17.bin";
    f.sha256 = k_sha_hex;
    f.signature = k_sig_hex;
    f.manifest_signature = k_sig_hex;
    f.signing_key_id = "532429078dc47c04";
    f.release_notes = "Updates confirm only after a healthy minute.";
    f.release_url = "https://github.com/kmay89/securaCV/releases/tag/fw-v2.4.17";
    f.size = 1409952;
    f.manifest_version = 1;
    return f;
}

// The canonical message, built here independently of the engine: the bytes
// ota_release.py signs for these fields.
static std::string signed_bytes_for(const securacv_ota_manifest_fields_t &f)
{
    const bool v2 = (f.manifest_version == 2);
    std::string s(v2 ? "scv-manifest-v2" : "scv-manifest-v1");
    s.push_back('\0');
    const auto add = [&s](const char *v) {
        if (v != NULL) s += v;
        s.push_back('\0');
    };
    std::string sha = f.sha256 ? f.sha256 : "";
    for (char &c : sha) {
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
    }
    add(f.product);
    add(f.version);
    add(f.min_version);
    add(f.url);
    add(sha.c_str());
    add(std::to_string(f.size).c_str());
    add(f.release_notes);
    add(f.release_url);
    if (v2) {
        add(f.board);
        add(f.chip);
    }
    return s;
}

static bool manifest_is_zero(const securacv_ota_manifest_t &m)
{
    const unsigned char *p = (const unsigned char *)&m;
    for (size_t i = 0; i < sizeof(m); i++) {
        if (p[i] != 0) return false;
    }
    return true;
}

static int test_manifest_from_fields()
{
    securacv_ota_manifest_t m;
    const char *bad = NULL;

    // A whole manifest comes through whole, and builds the bytes that were signed.
    securacv_ota_manifest_fields_t f = release_fields("securacv-canary");
    CHECK(securacv_ota_manifest_from_fields(&f, &m, &bad) == SECURACV_OTA_FIELDS_OK);
    CHECK(bad == NULL);
    CHECK(strcmp(m.product, f.product) == 0 && strcmp(m.version, f.version) == 0);
    CHECK(strcmp(m.min_version, f.min_version) == 0 && strcmp(m.url, f.url) == 0);
    CHECK(strcmp(m.sha256, f.sha256) == 0 && strcmp(m.signature, f.signature) == 0);
    CHECK(strcmp(m.manifest_signature, f.manifest_signature) == 0);
    CHECK(strcmp(m.signing_key_id, f.signing_key_id) == 0);
    CHECK(strcmp(m.release_notes, f.release_notes) == 0 && strcmp(m.release_url, f.release_url) == 0);
    CHECK(m.size == f.size && m.manifest_version == 1 && m.board[0] == '\0' && m.chip[0] == '\0');
    {
        uint8_t msg[SECURACV_OTA_MANIFEST_MSG_MAX];
        size_t len = 0;
        CHECK(securacv_ota_build_manifest_message(&m, msg, sizeof(msg), &len));
        const std::string want = signed_bytes_for(f);
        CHECK(len == want.size() && memcmp(msg, want.data(), len) == 0);
    }

    // Each string field: one byte short of its buffer fits; a full buffer's
    // worth (no room for the NUL) refuses the WHOLE manifest — never a cut
    // field, which could only fail the signature check later, and quietly.
    struct FieldCase {
        const char *name;
        const char *securacv_ota_manifest_fields_t::*in;
        size_t cap;
    };
    const FieldCase cases[] = {
        {"product", &securacv_ota_manifest_fields_t::product, sizeof(m.product)},
        {"version", &securacv_ota_manifest_fields_t::version, sizeof(m.version)},
        {"min_version", &securacv_ota_manifest_fields_t::min_version, sizeof(m.min_version)},
        {"url", &securacv_ota_manifest_fields_t::url, sizeof(m.url)},
        {"sha256", &securacv_ota_manifest_fields_t::sha256, sizeof(m.sha256)},
        {"signature", &securacv_ota_manifest_fields_t::signature, sizeof(m.signature)},
        {"manifest_signature", &securacv_ota_manifest_fields_t::manifest_signature, sizeof(m.manifest_signature)},
        {"signing_key_id", &securacv_ota_manifest_fields_t::signing_key_id, sizeof(m.signing_key_id)},
        {"release_notes", &securacv_ota_manifest_fields_t::release_notes, sizeof(m.release_notes)},
        {"release_url", &securacv_ota_manifest_fields_t::release_url, sizeof(m.release_url)},
        {"board", &securacv_ota_manifest_fields_t::board, sizeof(m.board)},
        {"chip", &securacv_ota_manifest_fields_t::chip, sizeof(m.chip)},
    };
    for (const FieldCase &c : cases) {
        const std::string fits(c.cap - 1, 'x');
        const std::string over(c.cap, 'x');
        f = release_fields("securacv-canary");
        f.manifest_version = 2;  // so board / chip are allowed at all
        f.*c.in = fits.c_str();
        CHECK(securacv_ota_manifest_from_fields(&f, &m, &bad) == SECURACV_OTA_FIELDS_OK);
        f.*c.in = over.c_str();
        bad = NULL;
        CHECK(securacv_ota_manifest_from_fields(&f, &m, &bad) == SECURACV_OTA_FIELDS_TOO_LONG);
        CHECK(bad != NULL && strcmp(bad, c.name) == 0);
        CHECK(manifest_is_zero(m));  // nothing half-filled survives a refusal
    }

    // The product buffer holds what the old one could not: a 47-byte id fits.
    CHECK(sizeof(m.product) == SECURACV_OTA_PRODUCT_MAX && SECURACV_OTA_PRODUCT_MAX >= 48);
    CHECK(sizeof(m.version) == SECURACV_OTA_VERSION_MAX && sizeof(m.min_version) == SECURACV_OTA_VERSION_MAX);

    // Required fields: absent or empty refuses, and names the field.
    const char *securacv_ota_manifest_fields_t::*const required[] = {
        &securacv_ota_manifest_fields_t::product, &securacv_ota_manifest_fields_t::version,
        &securacv_ota_manifest_fields_t::url, &securacv_ota_manifest_fields_t::sha256,
    };
    const char *required_names[] = {"product", "version", "url", "sha256"};
    for (size_t i = 0; i < 4; i++) {
        f = release_fields("securacv-canary");
        f.*required[i] = NULL;
        CHECK(securacv_ota_manifest_from_fields(&f, &m, &bad) == SECURACV_OTA_FIELDS_MISSING);
        CHECK(bad != NULL && strcmp(bad, required_names[i]) == 0);
        f.*required[i] = "";
        CHECK(securacv_ota_manifest_from_fields(&f, &m, &bad) == SECURACV_OTA_FIELDS_MISSING);
    }
    // Optional fields may be absent: they read as empty, and sign as empty.
    f = release_fields("securacv-canary");
    f.min_version = NULL;
    f.release_notes = NULL;
    f.release_url = NULL;
    CHECK(securacv_ota_manifest_from_fields(&f, &m, NULL) == SECURACV_OTA_FIELDS_OK);
    CHECK(m.min_version[0] == '\0' && m.release_notes[0] == '\0' && m.release_url[0] == '\0');

    // manifest_version: 1 and 2 only; the hardware binding only under 2.
    f = release_fields("securacv-canary");
    f.manifest_version = 0;
    CHECK(securacv_ota_manifest_from_fields(&f, &m, &bad) == SECURACV_OTA_FIELDS_UNKNOWN_VERSION);
    f.manifest_version = 3;
    CHECK(securacv_ota_manifest_from_fields(&f, &m, &bad) == SECURACV_OTA_FIELDS_UNKNOWN_VERSION);
    f.manifest_version = 1;
    f.board = "xiao-esp32c6-mr60";
    CHECK(securacv_ota_manifest_from_fields(&f, &m, &bad) == SECURACV_OTA_FIELDS_UNSIGNED_BINDING);
    CHECK(strcmp(bad, "board") == 0);
    f.board = NULL;
    f.chip = "esp32c6";
    CHECK(securacv_ota_manifest_from_fields(&f, &m, &bad) == SECURACV_OTA_FIELDS_UNSIGNED_BINDING);
    CHECK(strcmp(bad, "chip") == 0);
    f.chip = "";
    CHECK(securacv_ota_manifest_from_fields(&f, &m, &bad) == SECURACV_OTA_FIELDS_OK);  // empty binds nothing
    f.manifest_version = 2;
    f.board = "xiao-esp32c6-mr60";
    f.chip = "esp32c6";
    CHECK(securacv_ota_manifest_from_fields(&f, &m, &bad) == SECURACV_OTA_FIELDS_OK);
    CHECK(m.manifest_version == 2 && strcmp(m.board, "xiao-esp32c6-mr60") == 0 && strcmp(m.chip, "esp32c6") == 0);
    {
        uint8_t msg[SECURACV_OTA_MANIFEST_MSG_MAX];
        size_t len = 0;
        CHECK(securacv_ota_build_manifest_message(&m, msg, sizeof(msg), &len));
        const std::string want = signed_bytes_for(f);
        CHECK(len == want.size() && memcmp(msg, want.data(), len) == 0);
    }

    // Every field at its full length still builds within the message buffer.
    {
        const std::string p(sizeof(m.product) - 1, 'p'), v(sizeof(m.version) - 1, '9');
        const std::string u(sizeof(m.url) - 1, 'u'), s(sizeof(m.sha256) - 1, 'A');
        const std::string n(sizeof(m.release_notes) - 1, 'n'), r(sizeof(m.release_url) - 1, 'r');
        const std::string b(sizeof(m.board) - 1, 'b'), c(sizeof(m.chip) - 1, 'c');
        f = release_fields(p.c_str());
        f.version = v.c_str();
        f.min_version = v.c_str();
        f.url = u.c_str();
        f.sha256 = s.c_str();
        f.release_notes = n.c_str();
        f.release_url = r.c_str();
        f.board = b.c_str();
        f.chip = c.c_str();
        f.size = UINT32_MAX;
        f.manifest_version = 2;
        CHECK(securacv_ota_manifest_from_fields(&f, &m, NULL) == SECURACV_OTA_FIELDS_OK);
        uint8_t msg[SECURACV_OTA_MANIFEST_MSG_MAX];
        size_t len = 0;
        CHECK(securacv_ota_build_manifest_message(&m, msg, sizeof(msg), &len));
        CHECK(len == signed_bytes_for(f).size());
    }

    // Defensive edges.
    CHECK(securacv_ota_manifest_from_fields(NULL, &m, NULL) == SECURACV_OTA_FIELDS_MISSING);
    f = release_fields("securacv-canary");
    CHECK(securacv_ota_manifest_from_fields(&f, NULL, NULL) == SECURACV_OTA_FIELDS_MISSING);
    return 0;
}

// ── Every product id in the tree fits, and round-trips ───────────────────────

namespace fs = std::filesystem;

static std::string slurp(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The checkout this test belongs to: walk up from the working directory
// (CI runs at the repo root), then from this source file's own path.
static fs::path find_repo_root()
{
    const fs::path starts[] = {fs::current_path(), fs::path(__FILE__).parent_path()};
    for (const fs::path &start : starts) {
        std::error_code ec;
        fs::path p = fs::absolute(start, ec);
        if (ec) continue;
        while (!p.empty()) {
            if (fs::exists(p / "firmware" / "flavors.json", ec) &&
                fs::is_directory(p / "firmware" / "envs" / "platformio", ec)) {
                return p;
            }
            if (p == p.parent_path()) break;
            p = p.parent_path();
        }
    }
    return fs::path();
}

static bool id_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.';
}

// Every product id a line declares: an OTA_PRODUCT / SECURACV_OTA_PRODUCT
// token (a whole word — not SECURACV_OTA_PRODUCT_MAX) followed on the same
// line by a "securacv-…" string. Covers `#define SECURACV_OTA_PRODUCT "x"`,
// `-DSECURACV_OTA_PRODUCT="x"` / `=\"x\"` build flags and the WAP sketch's
// `OTA_PRODUCT = "x"`.
static void collect_product_ids(const std::string &text, std::set<std::string> &out)
{
    static const char token[] = "OTA_PRODUCT";
    size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos) {
        const size_t end = pos + sizeof(token) - 1;
        const size_t eol = text.find('\n', end);
        const size_t line_end = (eol == std::string::npos) ? text.size() : eol;
        pos = end;
        if (end < text.size() && (id_char(text[end]) && text[end] != '-' && text[end] != '.')) continue;  // a longer name
        size_t q = text.find("\"securacv-", end);
        if (q == std::string::npos || q >= line_end) continue;
        q += 1;
        size_t e = q;
        while (e < line_end && id_char(text[e])) e++;
        out.insert(text.substr(q, e - q));
    }
}

static bool scanned_extension(const fs::path &p)
{
    const std::string ext = p.extension().string();
    return ext == ".ini" || ext == ".h" || ext == ".hpp" || ext == ".ino" || ext == ".cpp";
}

static int test_every_product_id_fits_and_round_trips()
{
    const fs::path root = find_repo_root();
    if (root.empty()) {
        fprintf(stderr, "FAILED: could not find the checkout (firmware/flavors.json) from %s or %s — "
                        "this check reads the build files, it cannot pass without them\n",
                fs::current_path().string().c_str(), __FILE__);
        return 1;
    }

    std::set<std::string> ids;
    std::error_code ec;

    // 1. The shared PlatformIO env files.
    for (const fs::directory_entry &e : fs::directory_iterator(root / "firmware" / "envs" / "platformio", ec)) {
        if (e.path().extension() == ".ini") collect_product_ids(slurp(e.path()), ids);
    }
    // 2. Every flavor firmware/flavors.json names: its own build files and
    //    sources (the config.h fallbacks, the flagship's platformio.ini, the
    //    WAP sketch).
    const std::string flavors = slurp(root / "firmware" / "flavors.json");
    size_t dirs = 0;
    for (size_t pos = 0; (pos = flavors.find("\"dir\"", pos)) != std::string::npos;) {
        pos += 5;
        const size_t open = flavors.find('"', flavors.find(':', pos));
        const size_t close = flavors.find('"', open + 1);
        if (open == std::string::npos || close == std::string::npos) break;
        const fs::path dir = root / flavors.substr(open + 1, close - open - 1);
        CHECK(fs::is_directory(dir, ec));
        dirs++;
        std::error_code walk;
        fs::recursive_directory_iterator it(dir, walk), end;
        while (!walk && it != end) {
            std::error_code entry;  // a dangling link is skipped, not fatal
            const std::string name = it->path().filename().string();
            if (it->is_directory(entry) && (name == ".pio" || name == ".git" || name == "node_modules")) {
                it.disable_recursion_pending();
            } else if (it->is_regular_file(entry) && scanned_extension(it->path())) {
                collect_product_ids(slurp(it->path()), ids);
            }
            it.increment(walk);
        }
        CHECK(!walk);
    }
    CHECK(dirs >= 6);  // canary, wap, vision, display, sense, sentinel

    // The scan must actually see the tree: the flagship's id, and the seven
    // published Canary Display ids the old 32-byte buffer could not hold.
    static const char *const long_published[] = {
        "securacv-canary-display-dash-modes", "securacv-canary-display-nightstand7",
        "securacv-canary-display-nightstand-s3", "securacv-canary-display-touch169",
        "securacv-canary-display-amoled241", "securacv-canary-display-nightstand-c6",
        "securacv-canary-display-nightlight-c3",
    };
    CHECK(ids.count("securacv-canary") == 1 && ids.count("securacv-canary-wap") == 1);
    for (const char *id : long_published) {
        if (ids.count(id) != 1) fprintf(stderr, "scan did not find %s\n", id);
        CHECK(ids.count(id) == 1);
        CHECK(strlen(id) >= 32);  // what the pre-2.4.17 engine cut short
    }

    size_t legacy_cut = 0;
    for (const std::string &id : ids) {
        // Fits, with the NUL.
        if (id.size() >= SECURACV_OTA_PRODUCT_MAX) fprintf(stderr, "product id too long: %s\n", id.c_str());
        CHECK(id.size() < SECURACV_OTA_PRODUCT_MAX);

        // Through the parser's copy, unchanged.
        securacv_ota_manifest_fields_t f = release_fields(id.c_str());
        securacv_ota_manifest_t m;
        CHECK(securacv_ota_manifest_from_fields(&f, &m, NULL) == SECURACV_OTA_FIELDS_OK);
        CHECK(id == m.product);

        // Into the signed message, byte for byte: the bytes the signer signs.
        uint8_t msg[SECURACV_OTA_MANIFEST_MSG_MAX];
        size_t len = 0;
        CHECK(securacv_ota_build_manifest_message(&m, msg, sizeof(msg), &len));
        const std::string want = signed_bytes_for(f);
        CHECK(len == want.size() && memcmp(msg, want.data(), len) == 0);
        const size_t at = sizeof("scv-manifest-v1");  // prefix + its NUL
        CHECK(memcmp(msg + at, id.data(), id.size()) == 0 && msg[at + id.size()] == '\0');

        // ...which is exactly what the old strncpy into char[32] broke.
        if (id.size() >= 32) {
            securacv_ota_manifest_t old = m;
            memset(old.product, 0, sizeof(old.product));
            memcpy(old.product, id.data(), 31);
            size_t old_len = 0;
            uint8_t old_msg[SECURACV_OTA_MANIFEST_MSG_MAX];
            CHECK(securacv_ota_build_manifest_message(&old, old_msg, sizeof(old_msg), &old_len));
            CHECK(old_len != len || memcmp(old_msg, msg, len) != 0);
            legacy_cut++;
        }
    }
    CHECK(ids.size() >= 20);
    CHECK(legacy_cut >= 7);
    printf("product ids: %zu found, longest fits %d bytes; %zu would have been cut before 2.4.17\n",
           ids.size(), SECURACV_OTA_PRODUCT_MAX - 1, legacy_cut);
    return 0;
}

// ── The pending-marker rule (securacv_ota_take_marker) ───────────────────────

struct FakeMarker {
    std::string value;
    int loads = 0;
    int erases = 0;
};

static bool fake_marker_load(void *ctx, char *buf, size_t buf_len)
{
    FakeMarker *s = (FakeMarker *)ctx;
    s->loads++;
    if (s->value.empty() || s->value.size() + 1 > buf_len) return false;  // NVS refuses a short buffer
    memcpy(buf, s->value.c_str(), s->value.size() + 1);
    return true;
}

static void fake_marker_erase(void *ctx)
{
    FakeMarker *s = (FakeMarker *)ctx;
    s->erases++;
    s->value.clear();
}

// One tree's "record the update outcome" function, as the contract asks every
// tree to write it: called at boot after the probes, and again on the pass the
// health tick confirmed. Returns "applied", "rolled_back" or "" (nothing yet).
static std::string record_outcome(FakeMarker *engine, FakeMarker *legacy, bool pending,
                                  const char *running_version)
{
    const securacv_ota_marker_store_t stores[] = {
        {fake_marker_load, fake_marker_erase, engine},
        {fake_marker_load, fake_marker_erase, legacy},
    };
    char target[SECURACV_OTA_VERSION_MAX];
    if (!securacv_ota_take_marker(stores, 2, pending, target, sizeof(target))) {
        return target[0] == '\0' ? "" : "BUG: false with a target";
    }
    return strcmp(target, running_version) == 0 ? "applied" : "rolled_back";
}

static int test_pending_marker_rule()
{
    const uint32_t DWELL = 60000;

    // An update that sticks: the old 2.4.16 image recorded the marker when the
    // boot partition flipped; 2.4.17 boots PENDING_VERIFY.
    {
        FakeMarker engine, legacy;
        engine.value = "2.4.17";
        bool pending = true;

        // At boot, after the probes: still pending — nothing recorded, and the
        // marker is neither read nor erased.
        CHECK(record_outcome(&engine, &legacy, pending, "2.4.17") == "");
        CHECK(engine.value == "2.4.17" && engine.loads == 0 && engine.erases == 0);
        CHECK(legacy.loads == 0);

        // The dwell runs (the engine's own pure core); the outcome is asked for
        // only on the pass that confirmed.
        securacv_ota_dwell_t d = {false, false, 0};
        CHECK(securacv_ota_dwell_probes_passed(&d, false, 0) == SECURACV_OTA_DWELL_WAIT);
        CHECK(securacv_ota_dwell_init(&d, DWELL) == SECURACV_OTA_DWELL_WAIT);
        int confirmed_passes = 0;
        for (uint32_t t = 0; t <= DWELL + 5000; t += 1000) {
            if (securacv_ota_dwell_tick(&d, t, true, DWELL, pending) == SECURACV_OTA_DWELL_CONFIRM) {
                pending = false;               // esp_ota_mark_app_valid_cancel_rollback() landed
                securacv_ota_dwell_clear(&d);
                confirmed_passes++;
                CHECK(record_outcome(&engine, &legacy, pending, "2.4.17") == "applied");
            }
        }
        CHECK(confirmed_passes == 1);
        CHECK(engine.value.empty() && engine.erases == 1);
        // Exactly once: the next boot (confirmed, VALID) finds nothing.
        CHECK(record_outcome(&engine, &legacy, false, "2.4.17") == "");
    }

    // Power cut (or a crash, or a watchdog) inside the dwell: the bootloader
    // goes back to 2.4.16, which finds the marker and records the rollback —
    // the case engines before 2.4.17 lost, having erased the marker and
    // recorded "applied" on the new image's first boot.
    {
        FakeMarker engine, legacy;
        engine.value = "2.4.17";
        CHECK(record_outcome(&engine, &legacy, /*pending=*/true, "2.4.17") == "");
        securacv_ota_dwell_t d = {false, false, 0};
        securacv_ota_dwell_probes_passed(&d, false, 0);
        securacv_ota_dwell_init(&d, DWELL);
        CHECK(securacv_ota_dwell_tick(&d, 1000, true, DWELL, true) == SECURACV_OTA_DWELL_WAIT);
        CHECK(securacv_ota_dwell_tick(&d, 30000, true, DWELL, true) == SECURACV_OTA_DWELL_WAIT);
        // -- power lost here; next boot is the previous image, VALID --
        CHECK(engine.value == "2.4.17");
        CHECK(record_outcome(&engine, &legacy, /*pending=*/false, "2.4.16") == "rolled_back");
        CHECK(engine.value.empty());
        CHECK(record_outcome(&engine, &legacy, false, "2.4.16") == "");
    }

    // A deliberate restart after the probes passed confirms first
    // (securacv_ota_confirm_now): the next boot records "applied".
    {
        FakeMarker engine, legacy;
        engine.value = "2.4.17";
        CHECK(record_outcome(&engine, &legacy, true, "2.4.17") == "");
        securacv_ota_dwell_t d = {false, false, 0};
        securacv_ota_dwell_probes_passed(&d, false, 0);
        securacv_ota_dwell_init(&d, DWELL);
        CHECK(securacv_ota_dwell_init(&d, 0) == SECURACV_OTA_DWELL_CONFIRM);  // confirm_now's verdict
        CHECK(record_outcome(&engine, &legacy, false, "2.4.17") == "applied");
    }

    // The pre-engine marker (Preferences "securacv"/"ota_target") follows the
    // same rule, and only the store that held the marker is erased.
    {
        FakeMarker engine, legacy;
        legacy.value = "2.3.0";
        CHECK(record_outcome(&engine, &legacy, true, "2.3.0") == "");
        CHECK(legacy.value == "2.3.0" && legacy.loads == 0);
        CHECK(record_outcome(&engine, &legacy, false, "2.3.0") == "applied");
        CHECK(legacy.value.empty() && legacy.erases == 1 && engine.erases == 0);
    }

    // No install, or a confirmed boot with nothing pending: nothing to say.
    {
        FakeMarker engine, legacy;
        CHECK(record_outcome(&engine, &legacy, false, "2.4.17") == "");
        CHECK(engine.erases == 0 && legacy.erases == 0);
    }

    // Defensive edges: no buffer, no stores.
    {
        FakeMarker engine;
        engine.value = "2.4.17";
        const securacv_ota_marker_store_t one[] = {{fake_marker_load, fake_marker_erase, &engine}};
        char buf[SECURACV_OTA_VERSION_MAX];
        CHECK(!securacv_ota_take_marker(one, 1, false, NULL, 0));
        CHECK(!securacv_ota_take_marker(one, 1, false, buf, 0));
        CHECK(!securacv_ota_take_marker(NULL, 0, false, buf, sizeof(buf)) && buf[0] == '\0');
        CHECK(engine.value == "2.4.17");
        CHECK(securacv_ota_take_marker(one, 1, false, buf, sizeof(buf)) && strcmp(buf, "2.4.17") == 0);
    }
    return 0;
}

int main()
{
    if (test_version_compare()) return 1;
    if (test_update_decision()) return 1;
    if (test_image_fits_slot()) return 1;
    if (test_url_policy()) return 1;
    if (test_signed_message_layout()) return 1;
    if (test_hex_to_bytes()) return 1;
    if (test_manifest_message_cross_language_fixture()) return 1;
    if (test_manifest_v2_message_and_rules()) return 1;
    if (test_confirm_dwell()) return 1;
    if (test_friendly_strings()) return 1;
    if (test_channel_manifest_url()) return 1;
    if (test_manifest_from_fields()) return 1;
    if (test_every_product_id_fits_and_round_trips()) return 1;
    if (test_pending_marker_rule()) return 1;

    printf("ALL %d OTA LOGIC CHECKS PASSED\n", tests_run);
    return 0;
}
