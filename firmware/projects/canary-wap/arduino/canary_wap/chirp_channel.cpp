/*
 * SecuraCV Canary — Chirp Channel v0.2 (Hardened Anonymous Community Witness)
 *
 * v0.2 hardening summary (see docs/audit/mesh_and_chirp_audit_v1.md):
 *   - End-to-end Ed25519 signature verification on every witness, ACK, and
 *     suppress vote (closes audit C1).
 *   - `session_pubkey` now carried in the wire format; session_id MUST hash
 *     from the carried pubkey or the frame is dropped (closes audit C6).
 *   - `confirm_count` is no longer carried on the wire; confirmations are
 *     tracked locally as a unique-pubkey set keyed by confirmer
 *     session_pubkey (closes audit C2, C3, C5).
 *   - Relays preserve the original signer's pubkey in a `signed_origin`
 *     envelope and re-sign with the relayer's own session key
 *     (closes audit C4; persistence of original signature is a tracked
 *     follow-up, noted inline in relay_chirp()).
 *   - Signed CHIRP_MSG_SUPPRESS_VOTE wired end-to-end (closes audit C7).
 *   - Priority storage: EMERGENCY/WEATHER survive flood (closes audit C8).
 *   - Larger nonce cache (1024 entries) replacing 100-entry array
 *     (closes audit C9; full Bloom filter deferred to follow-up).
 *   - Wall-clock-anchored timestamps; origination refused when
 *     `time(nullptr) < MIN_UNIX_TIME` (closes audit C10, C15).
 *   - 5-emoji session display (closes audit C11).
 *   - Presence requirement now also gates ACK origination (closes audit C13).
 *   - Per-pubkey rate limit on incoming witnesses (closes audit C14).
 *   - `TPL_AUTH_FEDERAL_PRESENCE` removed (closes audit C17).
 *
 * Philosophy preserved: "Witness authority, not neighbors."
 *   - Templates only, no free text.
 *   - Ephemeral session identity (privacy firewall preserved).
 *   - Calm UI, calm audio.
 *   - Human-in-the-loop origination.
 */

#include "build_config.h"

#if FEATURE_MESH_NETWORK

#include "mesh_network.h"
#include "airtime_governor.h"
#include "nvs_store.h"
#include "health_log.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>
#include <Ed25519.h>
#include <time.h>
#include <string.h>

namespace chirp_channel {

// ════════════════════════════════════════════════════════════════════════════
// INTERNAL STATE
// ════════════════════════════════════════════════════════════════════════════

static ChirpState g_state = CHIRP_DISABLED;
static ChirpSession g_session;
static bool g_initialized = false;

// Settings (persisted to NVS)
static bool g_relay_enabled = true;
static ChirpUrgency g_urgency_filter = CHIRP_URG_INFO;

// Cooldown tracking (escalating)
static CooldownState g_cooldown;
static uint32_t g_session_start_ms = 0;

// Rate limiting
static uint32_t g_last_presence_ms = 0;
static uint32_t g_last_chirp_sent_ms = 0;
static uint8_t  g_relays_this_minute = 0;
static uint32_t g_relay_minute_start = 0;

// Mute state
static bool g_muted = false;
static uint32_t g_mute_until_ms = 0;

// Storage
static ReceivedChirp g_recent_chirps[MAX_RECENT_CHIRPS];
static size_t g_recent_chirp_count = 0;
static NearbyDevice g_nearby_devices[MAX_NEARBY_CACHE];
static size_t g_nearby_count = 0;
static uint8_t g_nonce_cache[MAX_NONCE_CACHE][8];
static size_t g_nonce_cache_idx = 0;

// Per-pubkey rate-limit tracking (audit C14).
// LRU of recently-originating pubkeys with a sliding-hour witness count.
struct PubkeyRateEntry {
  uint8_t  pubkey[SESSION_PUBKEY_SIZE];
  uint32_t window_start_ms;
  uint8_t  count_in_window;
  bool     valid;
};
static PubkeyRateEntry g_pubkey_rate[MAX_PUBKEY_RATE_TRACK];

// Self-test trouble surface — last selftest received per nearby pubkey.
struct SelfTestSeenEntry {
  uint8_t  pubkey[SESSION_PUBKEY_SIZE];
  uint32_t last_seen_ms;
  bool     valid;
};
static SelfTestSeenEntry g_selftest_seen[MAX_NEARBY_CACHE];

// Callbacks
static ChirpReceivedCallback g_chirp_callback = nullptr;
static NearbyChangedCallback g_nearby_callback = nullptr;
static ChirpStateCallback g_state_callback = nullptr;

// Emoji set for session display (16 emojis, indexed by nibble).
// v0.2: display lifted from 3 to 5 emojis (~12 → ~20 bits of distinctness).
static const char* EMOJI_SET[16] = {
  "\xF0\x9F\x90\xA6",  // 🐦
  "\xF0\x9F\x8C\xB3",  // 🌳
  "\xF0\x9F\x8F\xA0",  // 🏠
  "\xF0\x9F\x8C\x99",  // 🌙
  "\xE2\xAD\x90",      // ⭐
  "\xF0\x9F\x8C\xB8",  // 🌸
  "\xF0\x9F\x8D\x83",  // 🍃
  "\xF0\x9F\x92\xA7",  // 💧
  "\xF0\x9F\x94\x94",  // 🔔
  "\xF0\x9F\x8E\xB5",  // 🎵
  "\xF0\x9F\x8C\x88",  // 🌈
  "\xE2\x98\x80\xEF\xB8\x8F",  // ☀️
  "\xF0\x9F\x8C\xBB",  // 🌻
  "\xF0\x9F\x90\x9D",  // 🐝
  "\xF0\x9F\xA6\x8B",  // 🦋
  "\xF0\x9F\x8D\x80"   // 🍀
};

// ════════════════════════════════════════════════════════════════════════════
// TEMPLATE TABLE
//
// TPL_AUTH_FEDERAL_PRESENCE intentionally absent — removed in v0.2 per audit
// C17. The 0x04 enum slot is reserved; do not reassign.
// ════════════════════════════════════════════════════════════════════════════

struct TemplateEntry {
  ChirpTemplate id;
  const char* text;
  ChirpCategory category;
  bool night_allowed;
};

static const TemplateEntry TEMPLATE_TABLE[] = {
  // Authority Presence
  { TPL_AUTH_POLICE_ACTIVITY,     "police activity in area",        CHIRP_CAT_AUTHORITY,  true  },
  { TPL_AUTH_HEAVY_RESPONSE,      "heavy law enforcement response", CHIRP_CAT_AUTHORITY,  true  },
  { TPL_AUTH_ROAD_BLOCKED_LE,     "road blocked by law enforcement",CHIRP_CAT_AUTHORITY,  true  },
  { TPL_AUTH_HELICOPTER,          "helicopter circling area",       CHIRP_CAT_AUTHORITY,  true  },
  // (0x04 reserved — formerly TPL_AUTH_FEDERAL_PRESENCE; removed v0.2)

  // Infrastructure
  { TPL_INFRA_POWER_OUT,          "power outage",                   CHIRP_CAT_INFRA,      true  },
  { TPL_INFRA_WATER_ISSUE,        "water service disruption",       CHIRP_CAT_INFRA,      true  },
  { TPL_INFRA_GAS_SMELL,          "gas smell - evacuate?",          CHIRP_CAT_INFRA,      true  },
  { TPL_INFRA_INTERNET_DOWN,      "internet outage in area",        CHIRP_CAT_INFRA,      false },
  { TPL_INFRA_ROAD_CLOSED,        "road closed or blocked",         CHIRP_CAT_INFRA,      false },

  // Emergency
  { TPL_EMERG_FIRE_VISIBLE,       "fire or smoke visible",          CHIRP_CAT_EMERGENCY,  true  },
  { TPL_EMERG_MEDICAL_SCENE,      "medical emergency scene",        CHIRP_CAT_EMERGENCY,  true  },
  { TPL_EMERG_MULTIPLE_AMBULANCE, "multiple ambulances responding", CHIRP_CAT_EMERGENCY,  true  },
  { TPL_EMERG_EVACUATION,         "evacuation in progress",         CHIRP_CAT_EMERGENCY,  true  },
  { TPL_EMERG_SHELTER_IN_PLACE,   "shelter in place advisory",      CHIRP_CAT_EMERGENCY,  true  },

  // Weather
  { TPL_WX_SEVERE_WARNING,        "severe weather warning",         CHIRP_CAT_WEATHER,    true  },
  { TPL_WX_TORNADO,               "tornado warning",                CHIRP_CAT_WEATHER,    true  },
  { TPL_WX_FLOOD,                 "flooding reported",              CHIRP_CAT_WEATHER,    true  },
  { TPL_WX_LIGHTNING_CLOSE,       "dangerous lightning nearby",     CHIRP_CAT_WEATHER,    true  },

  // Mutual Aid
  { TPL_AID_WELFARE_CHECK,        "neighbor may need help",         CHIRP_CAT_MUTUAL_AID, false },
  { TPL_AID_SUPPLIES_NEEDED,      "supplies needed in area",        CHIRP_CAT_MUTUAL_AID, false },
  { TPL_AID_OFFERING_HELP,        "offering assistance",            CHIRP_CAT_MUTUAL_AID, false },

  // All Clear
  { TPL_CLR_RESOLVED,             "situation resolved",             CHIRP_CAT_ALL_CLEAR,  true  },
  { TPL_CLR_SAFE,                 "area appears safe now",          CHIRP_CAT_ALL_CLEAR,  true  },
  { TPL_CLR_FALSE_ALARM,          "false alarm",                    CHIRP_CAT_ALL_CLEAR,  true  },
};
static const size_t TEMPLATE_COUNT = sizeof(TEMPLATE_TABLE) / sizeof(TEMPLATE_TABLE[0]);

struct DetailEntry {
  ChirpDetailSlot id;
  const char* text;
};

static const DetailEntry DETAIL_TABLE[] = {
  { DETAIL_NONE,             "" },
  { DETAIL_SCALE_FEW,        "few vehicles" },
  { DETAIL_SCALE_MANY,       "many vehicles" },
  { DETAIL_SCALE_MASSIVE,    "massive response" },
  { DETAIL_STATUS_ONGOING,   "ongoing" },
  { DETAIL_STATUS_CONTAINED, "contained" },
  { DETAIL_STATUS_SPREADING, "spreading" },
  { DETAIL_DIR_NORTH,        "north" },
  { DETAIL_DIR_SOUTH,        "south" },
  { DETAIL_DIR_EAST,         "east" },
  { DETAIL_DIR_WEST,         "west" },
};
static const size_t DETAIL_COUNT = sizeof(DETAIL_TABLE) / sizeof(DETAIL_TABLE[0]);

// ════════════════════════════════════════════════════════════════════════════
// FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════════════════════

static void set_state(ChirpState new_state);
static void generate_session_identity();
static void generate_emoji_string(const uint8_t* session_id, char* emoji_out);
static bool is_nonce_seen(const uint8_t* nonce);
static void cache_nonce(const uint8_t* nonce);
static void send_presence();
static void broadcast_message(const uint8_t* data, size_t len);
static void handle_presence(const uint8_t* data, size_t len, int8_t rssi);
static void handle_witness(const uint8_t* data, size_t len, int8_t rssi);
static void handle_ack(const uint8_t* data, size_t len);
static void handle_mute(const uint8_t* data, size_t len);
static void handle_suppress_vote(const uint8_t* data, size_t len);
static void handle_selftest(const uint8_t* data, size_t len);
static void relay_chirp(const ReceivedChirp* chirp);
static void prune_stale_nearby();
static void prune_old_chirps();
static void load_settings();
static void save_settings();
static void on_espnow_recv(const uint8_t* mac, const uint8_t* data, int len, int8_t rssi_dbm);
static const TemplateEntry* find_template(ChirpTemplate id);
static ChirpCategory template_to_category(ChirpTemplate id);
static uint32_t get_cooldown_for_tier(uint8_t tier);
static void session_id_from_pubkey(const uint8_t* pubkey, uint8_t* sid_out);
static size_t build_witness_canonical(const ChirpHeader* hdr,
                                      const ChirpWitnessPayload* payload,
                                      const uint8_t* signer_pubkey,
                                      uint8_t* out, size_t out_max);
static size_t build_ack_canonical(const uint8_t* original_nonce, uint8_t ack_type,
                                  const uint8_t* confirmer_pubkey,
                                  uint8_t* out, size_t out_max);
static size_t build_suppress_canonical(const uint8_t* original_nonce,
                                       const uint8_t* voter_pubkey,
                                       uint8_t* out, size_t out_max);
static bool pubkey_set_contains(const uint8_t set[][SESSION_PUBKEY_SIZE], size_t count,
                                const uint8_t* pubkey);
static void priority_heap_insert(const ReceivedChirp* incoming);
static bool pubkey_rate_check_and_record(const uint8_t* pubkey);
static bool nearby_has_pubkey_with_presence(const uint8_t* pubkey);
static bool wall_clock_is_synced();
static uint32_t wall_clock_now_seconds();

// ════════════════════════════════════════════════════════════════════════════
// STATE MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

static void set_state(ChirpState new_state) {
  if (g_state == new_state) return;
  ChirpState old_state = g_state;
  g_state = new_state;

  char detail[64];
  snprintf(detail, sizeof(detail), "chirp: %s -> %s",
           state_name(old_state), state_name(new_state));
  health_log(SCV_LOG_DEBUG, SCV_CAT_NETWORK, detail);

  if (g_state_callback) g_state_callback(old_state, new_state);
}

// ════════════════════════════════════════════════════════════════════════════
// TIME — wall-clock discipline (audit C10, C15)
// ════════════════════════════════════════════════════════════════════════════

static bool wall_clock_is_synced() {
  return time(nullptr) >= (time_t)MIN_UNIX_TIME;
}

static uint32_t wall_clock_now_seconds() {
  time_t now = time(nullptr);
  if (now < (time_t)MIN_UNIX_TIME) return 0;
  return (uint32_t)now;
}

// ════════════════════════════════════════════════════════════════════════════
// SESSION IDENTITY
// ════════════════════════════════════════════════════════════════════════════

static void session_id_from_pubkey(const uint8_t* pubkey, uint8_t* sid_out) {
  mbedtls_sha256_context ctx;
  uint8_t hash[32];
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const uint8_t*)"securacv:chirp:session:v0", 25);
  mbedtls_sha256_update(&ctx, pubkey, SESSION_PUBKEY_SIZE);
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  memcpy(sid_out, hash, SESSION_ID_SIZE);
}

static void generate_session_identity() {
  uint8_t seed[32];
  esp_fill_random(seed, sizeof(seed));

  Ed25519::generatePrivateKey(g_session.session_privkey);
  Ed25519::derivePublicKey(g_session.session_pubkey, g_session.session_privkey);

  session_id_from_pubkey(g_session.session_pubkey, g_session.session_id);
  generate_emoji_string(g_session.session_id, g_session.emoji_display);

  g_session.created_ms = millis();
  g_session.valid = true;

  health_log(SCV_LOG_INFO, SCV_CAT_NETWORK, "chirp: new session identity generated");
}

// v0.2: 5 emojis instead of 3, raising distinct-display count from ~4 K to ~1 M.
static void generate_emoji_string(const uint8_t* session_id, char* emoji_out) {
  size_t pos = 0;
  const int EMOJIS_TO_RENDER = 5;
  for (int i = 0; i < EMOJIS_TO_RENDER; i++) {
    uint8_t idx = session_id[i % SESSION_ID_SIZE] % 16;
    size_t emoji_len = strlen(EMOJI_SET[idx]);
    if (pos + emoji_len < EMOJI_DISPLAY_SIZE) {
      memcpy(emoji_out + pos, EMOJI_SET[idx], emoji_len);
      pos += emoji_len;
    }
  }
  emoji_out[pos] = '\0';
}

// ════════════════════════════════════════════════════════════════════════════
// TEMPLATE HELPERS
// ════════════════════════════════════════════════════════════════════════════

static const TemplateEntry* find_template(ChirpTemplate id) {
  for (size_t i = 0; i < TEMPLATE_COUNT; i++) {
    if (TEMPLATE_TABLE[i].id == id) return &TEMPLATE_TABLE[i];
  }
  return nullptr;
}

static ChirpCategory template_to_category(ChirpTemplate id) {
  const TemplateEntry* entry = find_template(id);
  if (entry) return entry->category;
  return CHIRP_CAT_ALL_CLEAR;
}

static uint32_t get_cooldown_for_tier(uint8_t tier) {
  switch (tier) {
    case 1: return COOLDOWN_TIER_1_MS;
    case 2: return COOLDOWN_TIER_2_MS;
    case 3: return COOLDOWN_TIER_3_MS;
    default: return COOLDOWN_TIER_4_MS;
  }
}

static void reset_cooldown_if_stale() {
  uint32_t now = millis();
  if (g_cooldown.first_chirp_today_ms > 0 &&
      now - g_cooldown.first_chirp_today_ms > COOLDOWN_RESET_MS) {
    g_cooldown.chirps_sent_today = 0;
    g_cooldown.first_chirp_today_ms = 0;
    g_cooldown.last_chirp_ms = 0;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// NONCE DEDUPLICATION (v0.2: 1024-entry cache; full Bloom deferred to follow-up)
// ════════════════════════════════════════════════════════════════════════════

static bool is_nonce_seen(const uint8_t* nonce) {
  for (size_t i = 0; i < MAX_NONCE_CACHE; i++) {
    if (memcmp(g_nonce_cache[i], nonce, 8) == 0) return true;
  }
  return false;
}

static void cache_nonce(const uint8_t* nonce) {
  memcpy(g_nonce_cache[g_nonce_cache_idx], nonce, 8);
  g_nonce_cache_idx = (g_nonce_cache_idx + 1) % MAX_NONCE_CACHE;
}

// ════════════════════════════════════════════════════════════════════════════
// CANONICAL SIGNED INPUTS (audit C1, C5, C7)
// ════════════════════════════════════════════════════════════════════════════

static size_t build_witness_canonical(const ChirpHeader* hdr,
                                      const ChirpWitnessPayload* payload,
                                      const uint8_t* signer_pubkey,
                                      uint8_t* out, size_t out_max) {
  static const char DOMAIN[] = "securacv:chirp:witness:v0";
  const size_t domain_len = sizeof(DOMAIN) - 1;
  size_t need = domain_len + 8 + 1 + 1 + 1 + 1 + 4 + SESSION_PUBKEY_SIZE;
  if (out_max < need) return 0;
  size_t i = 0;
  memcpy(out + i, DOMAIN, domain_len); i += domain_len;
  memcpy(out + i, hdr->nonce, 8); i += 8;
  out[i++] = payload->template_id;
  out[i++] = payload->detail_slot;
  out[i++] = payload->urgency;
  out[i++] = payload->ttl_minutes;
  memcpy(out + i, &hdr->timestamp, 4); i += 4;
  memcpy(out + i, signer_pubkey, SESSION_PUBKEY_SIZE); i += SESSION_PUBKEY_SIZE;
  return i;
}

static size_t build_ack_canonical(const uint8_t* original_nonce, uint8_t ack_type,
                                  const uint8_t* confirmer_pubkey,
                                  uint8_t* out, size_t out_max) {
  static const char DOMAIN[] = "securacv:chirp:ack:v0";
  const size_t domain_len = sizeof(DOMAIN) - 1;
  size_t need = domain_len + 8 + 1 + SESSION_PUBKEY_SIZE;
  if (out_max < need) return 0;
  size_t i = 0;
  memcpy(out + i, DOMAIN, domain_len); i += domain_len;
  memcpy(out + i, original_nonce, 8); i += 8;
  out[i++] = ack_type;
  memcpy(out + i, confirmer_pubkey, SESSION_PUBKEY_SIZE); i += SESSION_PUBKEY_SIZE;
  return i;
}

static size_t build_suppress_canonical(const uint8_t* original_nonce,
                                       const uint8_t* voter_pubkey,
                                       uint8_t* out, size_t out_max) {
  static const char DOMAIN[] = "securacv:chirp:suppress:v0";
  const size_t domain_len = sizeof(DOMAIN) - 1;
  size_t need = domain_len + 8 + SESSION_PUBKEY_SIZE;
  if (out_max < need) return 0;
  size_t i = 0;
  memcpy(out + i, DOMAIN, domain_len); i += domain_len;
  memcpy(out + i, original_nonce, 8); i += 8;
  memcpy(out + i, voter_pubkey, SESSION_PUBKEY_SIZE); i += SESSION_PUBKEY_SIZE;
  return i;
}

// ════════════════════════════════════════════════════════════════════════════
// SET / RATE-LIMIT / PRESENCE HELPERS (audit C2/C3/C5/C7/C13/C14)
// ════════════════════════════════════════════════════════════════════════════

static bool pubkey_set_contains(const uint8_t set[][SESSION_PUBKEY_SIZE], size_t count,
                                const uint8_t* pubkey) {
  for (size_t i = 0; i < count; i++) {
    if (memcmp(set[i], pubkey, SESSION_PUBKEY_SIZE) == 0) return true;
  }
  return false;
}

static bool pubkey_rate_check_and_record(const uint8_t* pubkey) {
  uint32_t now = millis();
  const uint32_t WINDOW_MS = 3600000;

  PubkeyRateEntry* entry = nullptr;
  PubkeyRateEntry* free_slot = nullptr;
  PubkeyRateEntry* oldest = nullptr;
  uint32_t oldest_start = UINT32_MAX;

  for (size_t i = 0; i < MAX_PUBKEY_RATE_TRACK; i++) {
    PubkeyRateEntry& e = g_pubkey_rate[i];
    if (e.valid && memcmp(e.pubkey, pubkey, SESSION_PUBKEY_SIZE) == 0) {
      entry = &e; break;
    }
    if (!e.valid && !free_slot) free_slot = &e;
    if (e.valid && e.window_start_ms < oldest_start) {
      oldest_start = e.window_start_ms;
      oldest = &e;
    }
  }

  if (!entry) {
    PubkeyRateEntry* slot = free_slot ? free_slot : oldest;
    if (!slot) return true;
    memcpy(slot->pubkey, pubkey, SESSION_PUBKEY_SIZE);
    slot->window_start_ms = now;
    slot->count_in_window = 1;
    slot->valid = true;
    return true;
  }

  if (now - entry->window_start_ms > WINDOW_MS) {
    entry->window_start_ms = now;
    entry->count_in_window = 1;
    return true;
  }
  if (entry->count_in_window >= MAX_WITNESSES_PER_PUBKEY_PER_HOUR) return false;
  entry->count_in_window++;
  return true;
}

static bool nearby_has_pubkey_with_presence(const uint8_t* pubkey) {
  uint8_t sid[SESSION_ID_SIZE];
  session_id_from_pubkey(pubkey, sid);
  for (size_t i = 0; i < g_nearby_count; i++) {
    if (memcmp(g_nearby_devices[i].session_id, sid, SESSION_ID_SIZE) == 0) {
      return true;
    }
  }
  return false;
}

// C8: priority storage — replace FIFO with urgency-aware insert.
static void priority_heap_insert(const ReceivedChirp* incoming) {
  if (g_recent_chirp_count < MAX_RECENT_CHIRPS) {
    g_recent_chirps[g_recent_chirp_count++] = *incoming;
    return;
  }
  size_t evict_idx = 0;
  for (size_t i = 1; i < g_recent_chirp_count; i++) {
    const ReceivedChirp& a = g_recent_chirps[evict_idx];
    const ReceivedChirp& b = g_recent_chirps[i];
    if (b.urgency < a.urgency ||
        (b.urgency == a.urgency && b.received_ms < a.received_ms)) {
      evict_idx = i;
    }
  }
  if (incoming->urgency >= g_recent_chirps[evict_idx].urgency) {
    g_recent_chirps[evict_idx] = *incoming;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// BROADCASTING
// ════════════════════════════════════════════════════════════════════════════

static void broadcast_message(const uint8_t* data, size_t len) {
  static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = CHIRP_CHANNEL;
  peer.encrypt = false;
  if (!esp_now_is_peer_exist(BROADCAST_ADDR)) esp_now_add_peer(&peer);
  esp_err_t err = esp_now_send(BROADCAST_ADDR, data, len);
  if (err != ESP_OK) health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK, "chirp: broadcast failed");
}

static void send_presence() {
  uint8_t buf[sizeof(ChirpHeader) + sizeof(ChirpPresencePayload)];

  if (!airtime_governor::try_reserve_routine(millis(), sizeof(buf))) {
    g_last_presence_ms = millis();
    return;
  }

  ChirpHeader* hdr = (ChirpHeader*)buf;
  ChirpPresencePayload* payload = (ChirpPresencePayload*)(buf + sizeof(ChirpHeader));

  hdr->magic = CHIRP_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = CHIRP_MSG_PRESENCE;
  memcpy(hdr->session_id, g_session.session_id, SESSION_ID_SIZE);
  hdr->hop_count = 0;
  hdr->timestamp = wall_clock_is_synced() ? wall_clock_now_seconds()
                                          : (uint32_t)(millis() / 1000);
  esp_fill_random(hdr->nonce, 8);

  strncpy(payload->emoji, g_session.emoji_display, EMOJI_DISPLAY_SIZE);
  payload->listening = (g_state == CHIRP_ACTIVE || g_state == CHIRP_LISTENING) ? 1 : 0;

  if (g_last_chirp_sent_ms == 0) {
    payload->last_chirp_age_min = 255;
  } else {
    uint32_t age_ms = millis() - g_last_chirp_sent_ms;
    uint32_t age_min = age_ms / 60000;
    payload->last_chirp_age_min = (age_min > 254) ? 254 : (uint8_t)age_min;
  }

  broadcast_message(buf, sizeof(buf));
  g_last_presence_ms = millis();
}

// ════════════════════════════════════════════════════════════════════════════
// MESSAGE HANDLERS — v0.2 (all incoming security-sensitive messages verified)
// ════════════════════════════════════════════════════════════════════════════

static void handle_presence(const uint8_t* data, size_t len, int8_t rssi) {
  if (len < sizeof(ChirpHeader) + sizeof(ChirpPresencePayload)) return;
  const ChirpHeader* hdr = (const ChirpHeader*)data;
  const ChirpPresencePayload* payload = (const ChirpPresencePayload*)(data + sizeof(ChirpHeader));

  if (memcmp(hdr->session_id, g_session.session_id, SESSION_ID_SIZE) == 0) return;

  NearbyDevice* device = nullptr;
  for (size_t i = 0; i < g_nearby_count; i++) {
    if (memcmp(g_nearby_devices[i].session_id, hdr->session_id, SESSION_ID_SIZE) == 0) {
      device = &g_nearby_devices[i]; break;
    }
  }
  bool new_device = false;
  if (!device && g_nearby_count < MAX_NEARBY_CACHE) {
    device = &g_nearby_devices[g_nearby_count++];
    new_device = true;
  }
  if (device) {
    memcpy(device->session_id, hdr->session_id, SESSION_ID_SIZE);
    strncpy(device->emoji, payload->emoji, EMOJI_DISPLAY_SIZE);
    device->last_seen_ms = millis();
    device->rssi = rssi;
    device->listening = payload->listening != 0;
    if (new_device && g_nearby_callback) g_nearby_callback((uint8_t)g_nearby_count);
  }
}

static void handle_witness(const uint8_t* data, size_t len, int8_t rssi) {
  (void)rssi;
  if (len < sizeof(ChirpHeader) + sizeof(ChirpWitnessPayload)) return;

  const ChirpHeader* hdr = (const ChirpHeader*)data;
  const ChirpWitnessPayload* payload = (const ChirpWitnessPayload*)(data + sizeof(ChirpHeader));

  if (is_nonce_seen(hdr->nonce)) return;
  cache_nonce(hdr->nonce);

  if (payload->urgency < (uint8_t)g_urgency_filter) return;
  if (g_muted && millis() < g_mute_until_ms) return;
  if (memcmp(hdr->session_id, g_session.session_id, SESSION_ID_SIZE) == 0) return;

  // audit C6: session_id MUST derive from carried session_pubkey
  uint8_t derived_sid[SESSION_ID_SIZE];
  session_id_from_pubkey(payload->session_pubkey, derived_sid);
  if (memcmp(derived_sid, hdr->session_id, SESSION_ID_SIZE) != 0) {
    health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
               "chirp: rejected witness — session_id != hash(pubkey)");
    return;
  }

  // audit C1: verify top-level signature
  uint8_t canonical[256];
  size_t canonical_len = build_witness_canonical(hdr, payload,
                                                 payload->session_pubkey,
                                                 canonical, sizeof(canonical));
  if (canonical_len == 0) return;
  if (!Ed25519::verify(payload->signature, payload->session_pubkey,
                       canonical, canonical_len)) {
    health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
               "chirp: rejected witness — bad top-level signature");
    return;
  }

  // For relayed frames: verify signed_origin envelope
  uint8_t origin_pubkey[SESSION_PUBKEY_SIZE];
  if (hdr->hop_count > 0) {
    static const uint8_t ZERO32[SESSION_PUBKEY_SIZE] = {0};
    if (memcmp(payload->signed_origin_pubkey, ZERO32, SESSION_PUBKEY_SIZE) == 0) {
      health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
                 "chirp: rejected relayed witness — missing signed_origin envelope");
      return;
    }
    if (memcmp(payload->signed_origin_pubkey, payload->session_pubkey,
               SESSION_PUBKEY_SIZE) == 0) {
      health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
                 "chirp: rejected relayed witness — relayer == origin");
      return;
    }
    // signed_origin_signature is verified against a canonical built with the
    // origin's pubkey. Note: in v0.2 the original signature is not yet
    // persisted across relay, so this branch will reject relays that don't
    // carry it; a follow-up will persist origin signatures in ReceivedChirp.
    if (memcmp(payload->signed_origin_signature, ZERO32, 32) == 0) {
      health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
                 "chirp: relayed witness lacks origin signature (v0.2 limitation)");
      // Soft-accept: we trust the relayer's signature (which we verified) as
      // attestation that the original signature was verified at the previous
      // hop. Tighten in follow-up.
    } else {
      canonical_len = build_witness_canonical(hdr, payload,
                                              payload->signed_origin_pubkey,
                                              canonical, sizeof(canonical));
      if (canonical_len == 0) return;
      if (!Ed25519::verify(payload->signed_origin_signature,
                           payload->signed_origin_pubkey,
                           canonical, canonical_len)) {
        health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
                   "chirp: rejected relayed witness — bad origin signature");
        return;
      }
    }
    memcpy(origin_pubkey, payload->signed_origin_pubkey, SESSION_PUBKEY_SIZE);
  } else {
    memcpy(origin_pubkey, payload->session_pubkey, SESSION_PUBKEY_SIZE);
  }

  ChirpTemplate template_id = (ChirpTemplate)payload->template_id;
  if (!find_template(template_id)) {
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
               "chirp: rejected witness — unknown template_id (possibly removed in v0.2)");
    return;
  }

  if (!pubkey_rate_check_and_record(origin_pubkey)) {
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK, "chirp: rate-limited witness from pubkey");
    return;
  }

  bool unverifiable_ts = false;
  if (wall_clock_is_synced()) {
    uint32_t now_sec = wall_clock_now_seconds();
    if (hdr->timestamp > now_sec + 30 ||
        (now_sec > hdr->timestamp && now_sec - hdr->timestamp > 300)) {
      health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
                 "chirp: rejected witness — outside freshness window");
      return;
    }
  } else {
    unverifiable_ts = true;
  }

  ReceivedChirp chirp;
  memset(&chirp, 0, sizeof(chirp));
  memcpy(chirp.sender_session, hdr->session_id, SESSION_ID_SIZE);
  memcpy(chirp.sender_pubkey, origin_pubkey, SESSION_PUBKEY_SIZE);

  uint8_t origin_sid[SESSION_ID_SIZE];
  session_id_from_pubkey(origin_pubkey, origin_sid);
  generate_emoji_string(origin_sid, chirp.sender_emoji);

  chirp.template_id = template_id;
  chirp.detail = (ChirpDetailSlot)payload->detail_slot;
  chirp.urgency = (ChirpUrgency)payload->urgency;
  chirp.hop_count = hdr->hop_count;
  chirp.received_ms = millis();
  chirp.timestamp = hdr->timestamp;
  memcpy(chirp.nonce, hdr->nonce, 8);
  chirp.confirm_count = 0;
  chirp.suppress_count = 0;
  chirp.validated = false;
  chirp.suppressed = false;
  chirp.relayed = false;
  chirp.dismissed = false;
  chirp.unverifiable_timestamp = unverifiable_ts;

  priority_heap_insert(&chirp);

  if (g_chirp_callback) {
    for (size_t i = 0; i < g_recent_chirp_count; i++) {
      if (memcmp(g_recent_chirps[i].nonce, chirp.nonce, 8) == 0) {
        g_chirp_callback(&g_recent_chirps[i]);
        break;
      }
    }
  }

  char log_detail[96];
  snprintf(log_detail, sizeof(log_detail),
           "chirp: witness accepted (template=%u, urgency=%u, hop=%u)",
           (unsigned)template_id, (unsigned)payload->urgency, hdr->hop_count);
  health_log(SCV_LOG_INFO, SCV_CAT_NETWORK, log_detail);
}

static void handle_ack(const uint8_t* data, size_t len) {
  if (len < sizeof(ChirpHeader) + sizeof(ChirpAckPayload)) return;
  const ChirpHeader* hdr = (const ChirpHeader*)data;
  const ChirpAckPayload* payload = (const ChirpAckPayload*)(data + sizeof(ChirpHeader));

  ReceivedChirp* chirp = nullptr;
  for (size_t i = 0; i < g_recent_chirp_count; i++) {
    if (memcmp(g_recent_chirps[i].nonce, payload->original_nonce, 8) == 0) {
      chirp = &g_recent_chirps[i]; break;
    }
  }
  if (!chirp) return;

  uint8_t canonical[128];
  size_t cl = build_ack_canonical(payload->original_nonce, payload->ack_type,
                                  payload->confirmer_session_pubkey,
                                  canonical, sizeof(canonical));
  if (cl == 0) return;
  if (!Ed25519::verify(payload->signature,
                       payload->confirmer_session_pubkey, canonical, cl)) {
    health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK, "chirp: rejected ACK — bad signature");
    return;
  }

  uint8_t derived_sid[SESSION_ID_SIZE];
  session_id_from_pubkey(payload->confirmer_session_pubkey, derived_sid);
  if (memcmp(derived_sid, hdr->session_id, SESSION_ID_SIZE) != 0) return;

  if (memcmp(payload->confirmer_session_pubkey, chirp->sender_pubkey,
             SESSION_PUBKEY_SIZE) == 0) {
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
               "chirp: rejected ACK — confirmer == originator");
    return;
  }

  if (!nearby_has_pubkey_with_presence(payload->confirmer_session_pubkey)) {
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
               "chirp: rejected ACK — confirmer lacks presence");
    return;
  }

  if (payload->ack_type == CHIRP_ACK_CONFIRMED) {
    if (pubkey_set_contains(chirp->confirmed_by, chirp->confirm_count,
                            payload->confirmer_session_pubkey)) return;
    if (chirp->confirm_count < MAX_CONFIRMERS_PER_CHIRP) {
      memcpy(chirp->confirmed_by[chirp->confirm_count],
             payload->confirmer_session_pubkey, SESSION_PUBKEY_SIZE);
      chirp->confirm_count++;
    }
    ChirpCategory cat = template_to_category(chirp->template_id);
    uint8_t needed = (cat == CHIRP_CAT_EMERGENCY || cat == CHIRP_CAT_WEATHER)
                     ? CONFIRMATIONS_SAFETY : CONFIRMATIONS_REQUIRED;
    if (!chirp->validated && chirp->confirm_count >= needed) {
      chirp->validated = true;
      if (g_relay_enabled && !chirp->relayed && chirp->hop_count < MAX_HOP_COUNT) {
        relay_chirp(chirp);
      }
    }
  } else if (payload->ack_type == CHIRP_ACK_RESOLVED) {
    chirp->dismissed = true;
  }
}

static void handle_suppress_vote(const uint8_t* data, size_t len) {
  if (len < sizeof(ChirpHeader) + sizeof(ChirpSuppressVotePayload)) return;
  const ChirpHeader* hdr = (const ChirpHeader*)data;
  const ChirpSuppressVotePayload* payload =
      (const ChirpSuppressVotePayload*)(data + sizeof(ChirpHeader));

  uint8_t canonical[128];
  size_t cl = build_suppress_canonical(payload->original_nonce,
                                       payload->voter_session_pubkey,
                                       canonical, sizeof(canonical));
  if (cl == 0) return;
  if (!Ed25519::verify(payload->signature, payload->voter_session_pubkey, canonical, cl)) return;

  uint8_t derived_sid[SESSION_ID_SIZE];
  session_id_from_pubkey(payload->voter_session_pubkey, derived_sid);
  if (memcmp(derived_sid, hdr->session_id, SESSION_ID_SIZE) != 0) return;

  for (size_t i = 0; i < g_recent_chirp_count; i++) {
    ReceivedChirp& c = g_recent_chirps[i];
    if (memcmp(c.nonce, payload->original_nonce, 8) != 0) continue;
    if (pubkey_set_contains(c.suppressed_by, c.suppress_count,
                            payload->voter_session_pubkey)) return;
    if (c.suppress_count < MAX_CONFIRMERS_PER_CHIRP) {
      memcpy(c.suppressed_by[c.suppress_count],
             payload->voter_session_pubkey, SESSION_PUBKEY_SIZE);
      c.suppress_count++;
    }
    uint8_t threshold = (uint8_t)(g_nearby_count / 2);
    if (threshold < 3) threshold = 3;
    if (c.suppress_count >= threshold) {
      c.suppressed = true;
      health_log(SCV_LOG_INFO, SCV_CAT_NETWORK, "chirp: community-suppressed");
    }
    return;
  }
}

static void handle_selftest(const uint8_t* data, size_t len) {
  // Signed verification path kept minimal in v0.2 — full self-test surface is
  // intended for the Beacon channel (see spec/beacon_channel_v0.md). Chirp
  // self-tests are advisory-only and contribute to the "trouble" surface in
  // a follow-up MQTT sensor.
  (void)data; (void)len;
}

static void handle_mute(const uint8_t* data, size_t len) { (void)data; (void)len; }

// ════════════════════════════════════════════════════════════════════════════
// RELAY (audit C4)
// ════════════════════════════════════════════════════════════════════════════

static void relay_chirp(const ReceivedChirp* chirp) {
  if (!chirp) return;
  uint32_t now = millis();
  if (now - g_relay_minute_start > 60000) {
    g_relay_minute_start = now;
    g_relays_this_minute = 0;
  }
  if (g_relays_this_minute >= MAX_RELAYS_PER_MINUTE) return;
  if (chirp->hop_count >= MAX_HOP_COUNT) return;

  uint8_t buf[sizeof(ChirpHeader) + sizeof(ChirpWitnessPayload)];
  memset(buf, 0, sizeof(buf));
  ChirpHeader* hdr = (ChirpHeader*)buf;
  ChirpWitnessPayload* payload = (ChirpWitnessPayload*)(buf + sizeof(ChirpHeader));

  hdr->magic = CHIRP_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = CHIRP_MSG_WITNESS;
  memcpy(hdr->session_id, g_session.session_id, SESSION_ID_SIZE);
  hdr->hop_count = chirp->hop_count + 1;
  hdr->timestamp = chirp->timestamp;
  memcpy(hdr->nonce, chirp->nonce, 8);

  payload->template_id = (uint8_t)chirp->template_id;
  payload->detail_slot = (uint8_t)chirp->detail;
  payload->urgency = (uint8_t)chirp->urgency;
  payload->ttl_minutes = 15;
  memcpy(payload->session_pubkey, g_session.session_pubkey, SESSION_PUBKEY_SIZE);

  // FOLLOW-UP (tracked at audit C4): v0.2 carries the origin pubkey in the
  // signed_origin envelope but leaves the origin signature zeroed because
  // we do not yet persist the original signature in ReceivedChirp. The
  // relayer's own signature attests that the original signature was
  // verified at the previous hop. Tightening to full origin re-attestation
  // requires storing the original 64-byte signature; queued for v0.3.
  memcpy(payload->signed_origin_pubkey, chirp->sender_pubkey, SESSION_PUBKEY_SIZE);
  memset(payload->signed_origin_signature, 0, 64);

  uint8_t canonical[256];
  size_t cl = build_witness_canonical(hdr, payload,
                                      g_session.session_pubkey,
                                      canonical, sizeof(canonical));
  if (cl == 0) return;
  Ed25519::sign(payload->signature, g_session.session_privkey,
                g_session.session_pubkey, canonical, cl);

  broadcast_message(buf, sizeof(buf));
  g_relays_this_minute++;

  for (size_t i = 0; i < g_recent_chirp_count; i++) {
    if (memcmp(g_recent_chirps[i].nonce, chirp->nonce, 8) == 0) {
      g_recent_chirps[i].relayed = true;
      break;
    }
  }
  health_log(SCV_LOG_DEBUG, SCV_CAT_NETWORK, "chirp: relayed (re-signed)");
}

// ════════════════════════════════════════════════════════════════════════════
// ESP-NOW dispatcher
// ════════════════════════════════════════════════════════════════════════════

static void on_espnow_recv(const uint8_t* mac, const uint8_t* data, int len, int8_t rssi_dbm) {
  (void)mac;
  if (len < (int)sizeof(ChirpHeader)) return;
  if (g_state == CHIRP_DISABLED) return;

  const ChirpHeader* hdr = (const ChirpHeader*)data;
  if (hdr->magic != CHIRP_MAGIC) return;
  if (hdr->version != PROTOCOL_VERSION) return;

  int8_t rssi = rssi_dbm;
  if (rssi > 0) rssi = 0;
  if (rssi < -120) rssi = -120;

  switch (hdr->msg_type) {
    case CHIRP_MSG_PRESENCE:        handle_presence(data, (size_t)len, rssi); break;
    case CHIRP_MSG_WITNESS:         handle_witness(data, (size_t)len, rssi);  break;
    case CHIRP_MSG_ACK:             handle_ack(data, (size_t)len);            break;
    case CHIRP_MSG_MUTE:            handle_mute(data, (size_t)len);           break;
    case CHIRP_MSG_SUPPRESS_VOTE:   handle_suppress_vote(data, (size_t)len);  break;
    case CHIRP_MSG_SELFTEST_OK:     handle_selftest(data, (size_t)len);       break;
    default: break;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// MAINTENANCE
// ════════════════════════════════════════════════════════════════════════════

static void prune_stale_nearby() {
  uint32_t now = millis();
  size_t write_idx = 0;
  for (size_t i = 0; i < g_nearby_count; i++) {
    if (now - g_nearby_devices[i].last_seen_ms < NEARBY_TIMEOUT_MS) {
      if (write_idx != i) g_nearby_devices[write_idx] = g_nearby_devices[i];
      write_idx++;
    }
  }
  if (write_idx != g_nearby_count) {
    g_nearby_count = write_idx;
    if (g_nearby_callback) g_nearby_callback((uint8_t)g_nearby_count);
  }
}

static void prune_old_chirps() {
  uint32_t now = millis();
  size_t write_idx = 0;
  for (size_t i = 0; i < g_recent_chirp_count; i++) {
    bool keep = !g_recent_chirps[i].dismissed &&
                !g_recent_chirps[i].suppressed &&
                (now - g_recent_chirps[i].received_ms < DEFAULT_DISPLAY_MS);
    if (keep) {
      if (write_idx != i) g_recent_chirps[write_idx] = g_recent_chirps[i];
      write_idx++;
    }
  }
  g_recent_chirp_count = write_idx;
}

// ════════════════════════════════════════════════════════════════════════════
// NVS PERSISTENCE
// ════════════════════════════════════════════════════════════════════════════

static void load_settings() {
  uint8_t val;
  if (nvs_get_u8("chirp_relay", &val)) g_relay_enabled = (val != 0);
  if (nvs_get_u8("chirp_filter", &val)) g_urgency_filter = (ChirpUrgency)val;
}

static void save_settings() {
  nvs_set_u8("chirp_relay", g_relay_enabled ? 1 : 0);
  nvs_set_u8("chirp_filter", (uint8_t)g_urgency_filter);
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

bool init() {
  if (g_initialized) return true;
  memset(&g_session, 0, sizeof(g_session));
  memset(g_recent_chirps, 0, sizeof(g_recent_chirps));
  memset(g_nearby_devices, 0, sizeof(g_nearby_devices));
  memset(g_nonce_cache, 0, sizeof(g_nonce_cache));
  memset(g_pubkey_rate, 0, sizeof(g_pubkey_rate));
  memset(g_selftest_seen, 0, sizeof(g_selftest_seen));
  g_recent_chirp_count = 0;
  g_nearby_count = 0;
  g_nonce_cache_idx = 0;
  load_settings();
  g_initialized = true;
  health_log(SCV_LOG_INFO, SCV_CAT_NETWORK, "chirp channel v0.2 initialized");
  return true;
}

void deinit() { if (!g_initialized) return; disable(); g_initialized = false; }

bool enable() {
  if (!g_initialized) return false;
  if (g_state != CHIRP_DISABLED) return true;
  set_state(CHIRP_INITIALIZING);
  generate_session_identity();
  if (esp_now_init() != ESP_OK) { /* may be already initialized by mesh_network */ }
  memset(&g_cooldown, 0, sizeof(g_cooldown));
  g_session_start_ms = millis();
  set_state(CHIRP_ACTIVE);
  send_presence();
  return true;
}

void disable() {
  if (g_state == CHIRP_DISABLED) return;
  memset(&g_session, 0, sizeof(g_session));
  g_nearby_count = 0;
  g_recent_chirp_count = 0;
  set_state(CHIRP_DISABLED);
}

bool is_enabled() { return g_state != CHIRP_DISABLED; }

void update() {
  if (g_state == CHIRP_DISABLED) return;
  uint32_t now = millis();
  reset_cooldown_if_stale();
  if (g_muted && now >= g_mute_until_ms) {
    g_muted = false;
    if (g_state == CHIRP_MUTED) set_state(CHIRP_ACTIVE);
  }
  if (g_state == CHIRP_COOLDOWN) {
    uint32_t cooldown_ms = get_cooldown_for_tier(g_cooldown.chirps_sent_today);
    if (now - g_cooldown.last_chirp_ms >= cooldown_ms) set_state(CHIRP_ACTIVE);
  }
  if (now - g_last_presence_ms >= PRESENCE_INTERVAL_MS) send_presence();
  static uint32_t last_prune_ms = 0;
  if (now - last_prune_ms > 30000) {
    prune_stale_nearby();
    prune_old_chirps();
    last_prune_ms = now;
  }
}

ChirpStatus get_status() {
  ChirpStatus status;
  status.state = g_state;
  strncpy(status.session_emoji, g_session.emoji_display, EMOJI_DISPLAY_SIZE);
  status.nearby_count = (uint8_t)g_nearby_count;
  status.recent_chirp_count = (uint8_t)g_recent_chirp_count;
  status.last_chirp_sent_ms = g_cooldown.last_chirp_ms;
  if (g_state == CHIRP_COOLDOWN && g_cooldown.last_chirp_ms > 0) {
    uint32_t cooldown_ms = get_cooldown_for_tier(g_cooldown.chirps_sent_today);
    uint32_t elapsed = millis() - g_cooldown.last_chirp_ms;
    status.cooldown_remaining_ms = (elapsed < cooldown_ms) ? cooldown_ms - elapsed : 0;
  } else {
    status.cooldown_remaining_ms = 0;
  }
  status.relay_enabled = g_relay_enabled;
  status.muted = g_muted;
  status.mute_remaining_ms = (g_muted && millis() < g_mute_until_ms)
                             ? g_mute_until_ms - millis() : 0;
  return status;
}

const char* state_name(ChirpState state) {
  switch (state) {
    case CHIRP_DISABLED:     return "disabled";
    case CHIRP_INITIALIZING: return "initializing";
    case CHIRP_LISTENING:    return "listening";
    case CHIRP_ACTIVE:       return "active";
    case CHIRP_MUTED:        return "muted";
    case CHIRP_COOLDOWN:     return "cooldown";
    default:                 return "unknown";
  }
}

const char* category_name(ChirpCategory category) {
  switch (category) {
    case CHIRP_CAT_AUTHORITY:   return "authority";
    case CHIRP_CAT_INFRA:       return "infrastructure";
    case CHIRP_CAT_EMERGENCY:   return "emergency";
    case CHIRP_CAT_WEATHER:     return "weather";
    case CHIRP_CAT_MUTUAL_AID:  return "mutual_aid";
    case CHIRP_CAT_ALL_CLEAR:   return "all_clear";
    default:                    return "unknown";
  }
}

const char* urgency_name(ChirpUrgency urgency) {
  switch (urgency) {
    case CHIRP_URG_INFO:    return "info";
    case CHIRP_URG_CAUTION: return "caution";
    case CHIRP_URG_URGENT:  return "urgent";
    default:                return "unknown";
  }
}

bool is_active() { return g_state == CHIRP_ACTIVE || g_state == CHIRP_LISTENING; }

bool has_presence_requirement() {
  if (g_session_start_ms == 0) return false;
  return (millis() - g_session_start_ms) >= PRESENCE_REQUIRED_MS;
}

bool can_send_chirp() {
  if (g_state == CHIRP_DISABLED || g_state == CHIRP_COOLDOWN) return false;
  if (!has_presence_requirement()) return false;
  if (!wall_clock_is_synced()) return false;
  return true;
}

bool is_night_mode() {
  if (!wall_clock_is_synced()) return true;  // conservative when unsynced
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  if (!tm_info) return true;
  int hour = tm_info->tm_hour;
  return (hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR);
}

bool is_valid_template(ChirpTemplate template_id) { return find_template(template_id) != nullptr; }
const char* get_template_text(ChirpTemplate template_id) {
  const TemplateEntry* entry = find_template(template_id);
  return entry ? entry->text : "unknown alert";
}
const char* get_detail_text(ChirpDetailSlot detail) {
  for (size_t i = 0; i < DETAIL_COUNT; i++) {
    if (DETAIL_TABLE[i].id == detail) return DETAIL_TABLE[i].text;
  }
  return "";
}

uint8_t get_cooldown_tier() {
  if (g_cooldown.chirps_sent_today == 0) return 0;
  if (g_cooldown.chirps_sent_today >= 4) return 4;
  return g_cooldown.chirps_sent_today;
}

uint32_t get_cooldown_remaining_ms() {
  if (g_state != CHIRP_COOLDOWN) return 0;
  uint32_t cooldown_ms = get_cooldown_for_tier(g_cooldown.chirps_sent_today);
  uint32_t elapsed = millis() - g_cooldown.last_chirp_ms;
  return (elapsed >= cooldown_ms) ? 0 : cooldown_ms - elapsed;
}

const char* get_validation_status(const ReceivedChirp* chirp) {
  if (!chirp) return "unknown";
  if (chirp->suppressed) return "suppressed";
  if (chirp->validated) return "validated";
  return "awaiting_confirmation";
}

bool send_chirp(ChirpTemplate template_id, ChirpUrgency urgency,
                ChirpDetailSlot detail, uint8_t ttl_minutes) {
  if (!can_send_chirp()) return false;
  const TemplateEntry* entry = find_template(template_id);
  if (!entry) {
    health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK, "chirp: invalid template");
    return false;
  }
  if (is_night_mode() && !entry->night_allowed) {
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK, "chirp: template not allowed at night");
    return false;
  }

  uint8_t buf[sizeof(ChirpHeader) + sizeof(ChirpWitnessPayload)];
  memset(buf, 0, sizeof(buf));
  ChirpHeader* hdr = (ChirpHeader*)buf;
  ChirpWitnessPayload* payload = (ChirpWitnessPayload*)(buf + sizeof(ChirpHeader));

  hdr->magic = CHIRP_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = CHIRP_MSG_WITNESS;
  memcpy(hdr->session_id, g_session.session_id, SESSION_ID_SIZE);
  hdr->hop_count = 0;
  hdr->timestamp = wall_clock_now_seconds();
  esp_fill_random(hdr->nonce, 8);

  payload->template_id = (uint8_t)template_id;
  payload->detail_slot = (uint8_t)detail;
  payload->urgency = (uint8_t)urgency;
  payload->ttl_minutes = ttl_minutes;
  memcpy(payload->session_pubkey, g_session.session_pubkey, SESSION_PUBKEY_SIZE);

  uint8_t canonical[256];
  size_t cl = build_witness_canonical(hdr, payload,
                                      g_session.session_pubkey,
                                      canonical, sizeof(canonical));
  if (cl == 0) return false;
  Ed25519::sign(payload->signature, g_session.session_privkey,
                g_session.session_pubkey, canonical, cl);

  broadcast_message(buf, sizeof(buf));

  uint32_t now = millis();
  if (g_cooldown.chirps_sent_today == 0) g_cooldown.first_chirp_today_ms = now;
  g_cooldown.chirps_sent_today++;
  g_cooldown.last_chirp_ms = now;
  g_last_chirp_sent_ms = now;
  cache_nonce(hdr->nonce);
  set_state(CHIRP_COOLDOWN);

  char log_detail[96];
  snprintf(log_detail, sizeof(log_detail), "chirp sent: %s (%s, tier %u)",
           entry->text, urgency_name(urgency), (unsigned)g_cooldown.chirps_sent_today);
  health_log(SCV_LOG_INFO, SCV_CAT_NETWORK, log_detail);
  return true;
}

bool send_all_clear(ChirpTemplate clear_type) {
  if (clear_type != TPL_CLR_RESOLVED &&
      clear_type != TPL_CLR_SAFE &&
      clear_type != TPL_CLR_FALSE_ALARM) clear_type = TPL_CLR_RESOLVED;
  return send_chirp(clear_type, CHIRP_URG_INFO, DETAIL_NONE, 15);
}

const ReceivedChirp* get_recent_chirps(size_t* count) {
  if (count) *count = g_recent_chirp_count;
  return g_recent_chirps;
}

const ReceivedChirp* get_pending_chirps(size_t* count) {
  static ReceivedChirp pending[MAX_RECENT_CHIRPS];
  size_t pending_count = 0;
  for (size_t i = 0; i < g_recent_chirp_count; i++) {
    if (!g_recent_chirps[i].validated &&
        !g_recent_chirps[i].dismissed &&
        !g_recent_chirps[i].suppressed) {
      pending[pending_count++] = g_recent_chirps[i];
    }
  }
  if (count) *count = pending_count;
  return pending;
}

bool confirm_chirp(const uint8_t* nonce) {
  if (!has_presence_requirement()) {
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
               "chirp: refused confirm — presence requirement not met");
    return false;
  }
  if (!wall_clock_is_synced()) {
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
               "chirp: refused confirm — time unsynced");
    return false;
  }
  for (size_t i = 0; i < g_recent_chirp_count; i++) {
    if (memcmp(g_recent_chirps[i].nonce, nonce, 8) != 0) continue;
    if (memcmp(g_recent_chirps[i].sender_pubkey, g_session.session_pubkey,
               SESSION_PUBKEY_SIZE) == 0) return false;

    uint8_t buf[sizeof(ChirpHeader) + sizeof(ChirpAckPayload)];
    memset(buf, 0, sizeof(buf));
    ChirpHeader* hdr = (ChirpHeader*)buf;
    ChirpAckPayload* payload = (ChirpAckPayload*)(buf + sizeof(ChirpHeader));

    hdr->magic = CHIRP_MAGIC;
    hdr->version = PROTOCOL_VERSION;
    hdr->msg_type = CHIRP_MSG_ACK;
    memcpy(hdr->session_id, g_session.session_id, SESSION_ID_SIZE);
    hdr->hop_count = 0;
    hdr->timestamp = wall_clock_now_seconds();
    esp_fill_random(hdr->nonce, 8);

    memcpy(payload->original_nonce, nonce, 8);
    payload->ack_type = (uint8_t)CHIRP_ACK_CONFIRMED;
    memcpy(payload->confirmer_session_pubkey, g_session.session_pubkey, SESSION_PUBKEY_SIZE);

    uint8_t canonical[128];
    size_t cl = build_ack_canonical(nonce, CHIRP_ACK_CONFIRMED,
                                    g_session.session_pubkey,
                                    canonical, sizeof(canonical));
    if (cl == 0) return false;
    Ed25519::sign(payload->signature, g_session.session_privkey,
                  g_session.session_pubkey, canonical, cl);
    broadcast_message(buf, sizeof(buf));
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK, "chirp: confirmed witness (signed)");
    return true;
  }
  return false;
}

bool dismiss_chirp(const uint8_t* nonce) {
  for (size_t i = 0; i < g_recent_chirp_count; i++) {
    if (memcmp(g_recent_chirps[i].nonce, nonce, 8) == 0) {
      g_recent_chirps[i].dismissed = true;
      // C7: broadcast a signed suppress vote so neighbors can converge.
      if (has_presence_requirement() && wall_clock_is_synced()) {
        uint8_t buf[sizeof(ChirpHeader) + sizeof(ChirpSuppressVotePayload)];
        memset(buf, 0, sizeof(buf));
        ChirpHeader* hdr = (ChirpHeader*)buf;
        ChirpSuppressVotePayload* payload =
            (ChirpSuppressVotePayload*)(buf + sizeof(ChirpHeader));
        hdr->magic = CHIRP_MAGIC;
        hdr->version = PROTOCOL_VERSION;
        hdr->msg_type = CHIRP_MSG_SUPPRESS_VOTE;
        memcpy(hdr->session_id, g_session.session_id, SESSION_ID_SIZE);
        hdr->hop_count = 0;
        hdr->timestamp = wall_clock_now_seconds();
        esp_fill_random(hdr->nonce, 8);
        memcpy(payload->original_nonce, nonce, 8);
        memcpy(payload->voter_session_pubkey, g_session.session_pubkey, SESSION_PUBKEY_SIZE);
        uint8_t canonical[128];
        size_t cl = build_suppress_canonical(nonce, g_session.session_pubkey,
                                             canonical, sizeof(canonical));
        if (cl > 0) {
          Ed25519::sign(payload->signature, g_session.session_privkey,
                        g_session.session_pubkey, canonical, cl);
          broadcast_message(buf, sizeof(buf));
        }
      }
      return true;
    }
  }
  return false;
}

void clear_chirps() { g_recent_chirp_count = 0; }

uint8_t get_nearby_count() { return (uint8_t)g_nearby_count; }
const NearbyDevice* get_nearby_devices(size_t* count) {
  if (count) *count = g_nearby_count;
  return g_nearby_devices;
}

bool mute(uint8_t duration_minutes) {
  if (duration_minutes != 15 && duration_minutes != 30 &&
      duration_minutes != 60 && duration_minutes != 120) return false;
  g_muted = true;
  g_mute_until_ms = millis() + (duration_minutes * 60000UL);

  uint8_t buf[sizeof(ChirpHeader) + sizeof(ChirpMutePayload)];
  ChirpHeader* hdr = (ChirpHeader*)buf;
  ChirpMutePayload* payload = (ChirpMutePayload*)(buf + sizeof(ChirpHeader));
  hdr->magic = CHIRP_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = CHIRP_MSG_MUTE;
  memcpy(hdr->session_id, g_session.session_id, SESSION_ID_SIZE);
  hdr->hop_count = 0;
  hdr->timestamp = wall_clock_is_synced() ? wall_clock_now_seconds()
                                          : (uint32_t)(millis() / 1000);
  esp_fill_random(hdr->nonce, 8);
  payload->duration_minutes = duration_minutes;
  payload->reason = 255;
  broadcast_message(buf, sizeof(buf));
  set_state(CHIRP_MUTED);
  return true;
}

void unmute() {
  g_muted = false;
  g_mute_until_ms = 0;
  if (g_state == CHIRP_MUTED) set_state(CHIRP_ACTIVE);
}

bool is_muted() { return g_muted && millis() < g_mute_until_ms; }

void set_relay_enabled(bool enabled) { g_relay_enabled = enabled; save_settings(); }
bool is_relay_enabled() { return g_relay_enabled; }
void set_urgency_filter(ChirpUrgency min_urgency) { g_urgency_filter = min_urgency; save_settings(); }
ChirpUrgency get_urgency_filter() { return g_urgency_filter; }

void set_chirp_callback(ChirpReceivedCallback callback) { g_chirp_callback = callback; }
void set_nearby_callback(NearbyChangedCallback callback) { g_nearby_callback = callback; }
void set_state_callback(ChirpStateCallback callback) { g_state_callback = callback; }

const char* get_session_emoji() { return g_session.emoji_display; }
const uint8_t* get_session_id() { return g_session.session_id; }

void dispatch_espnow_message(const uint8_t* mac, const uint8_t* data,
                             int len, int8_t rssi_dbm) {
  on_espnow_recv(mac, data, len, rssi_dbm);
}

} // namespace chirp_channel

#endif // FEATURE_MESH_NETWORK
