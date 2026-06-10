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
 */

#include "securacv_ota.h"

#include <cassert>
#include <cstdio>
#include <cstring>

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

static int test_friendly_strings()
{
    // Every error code needs a usable primary-UI sentence; raw enum names
    // and hex codes must never reach the user.
    const securacv_ota_error_t errors[] = {
        SECURACV_OTA_ERR_NETWORK, SECURACV_OTA_ERR_MANIFEST_FETCH,
        SECURACV_OTA_ERR_MANIFEST_PARSE, SECURACV_OTA_ERR_MANIFEST_INVALID,
        SECURACV_OTA_ERR_NO_UPDATE, SECURACV_OTA_ERR_DOWNLOAD_FAILED,
        SECURACV_OTA_ERR_SHA256_MISMATCH, SECURACV_OTA_ERR_SIGNATURE_INVALID,
        SECURACV_OTA_ERR_SIZE_MISMATCH, SECURACV_OTA_ERR_FLASH_WRITE,
        SECURACV_OTA_ERR_FLASH_READ, SECURACV_OTA_ERR_PARTITION,
        SECURACV_OTA_ERR_VERSION_ROLLBACK, SECURACV_OTA_ERR_SELF_TEST_FAILED,
        SECURACV_OTA_ERR_ALREADY_RUNNING, SECURACV_OTA_ERR_URL_POLICY,
        SECURACV_OTA_ERR_PUBKEY_MISSING, SECURACV_OTA_ERR_DEFERRED,
    };
    const char *banned[] = {"OTA", "SHA", "Ed25519", "NVS", "manifest",
                            "partition", "0x", "esp_"};

    for (securacv_ota_error_t e : errors) {
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

int main()
{
    if (test_version_compare()) return 1;
    if (test_update_decision()) return 1;
    if (test_url_policy()) return 1;
    if (test_signed_message_layout()) return 1;
    if (test_hex_to_bytes()) return 1;
    if (test_friendly_strings()) return 1;

    printf("ALL %d OTA LOGIC CHECKS PASSED\n", tests_run);
    return 0;
}
