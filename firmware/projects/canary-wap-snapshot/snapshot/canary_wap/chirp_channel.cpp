/*
 * SecuraCV Canary — Chirp Channel (Anonymous Community Witness Network)
 * Version 0.2.0
 *
 * Privacy-first community alert system with ephemeral identities.
 * "Safety in numbers, not surveillance"
 * "Witness authority, not neighbors"
 *
 * Key Properties:
 * - Ephemeral session identity (new each enable/reboot)
 * - Human-in-the-loop (no automated broadcasts)
 * - STRUCTURED TEMPLATES ONLY (no free text - abuse prevention)
 * - 3-hop max range (neighborhood only)
 * - No persistent history
 * - Escalating cooldowns (prevents spam/hysteria)
 * - Witness confirmation requirement (2 needed before relay)
 * - Community suppress voting (50% dismiss = suppress)
 *
 * See spec/chirp_channel_v0.md for full specification.
 */

#include "mesh_network.h"
#include "nvs_store.h"
#include "health_log.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>
#include <Ed25519.h>
#include <time.h>

// ════════════════════════════════════════════════════════════════════════════
// INTERNAL STATE
// ════════════════════════════════════════════════════════════════════════════

namespace chirp_channel {

// Current state
static ChirpState g_state = CHIRP_DISABLED;
static ChirpSession g_session;
static bool g_initialized = false;

// Settings (persisted to NVS)
static bool g_relay_enabled = true;
static ChirpUrgency g_urgency_filter = CHIRP_URG_INFO;

// Cooldown tracking (escalating)
static CooldownState g_cooldown;
static uint32_t g_session_start_ms = 0;  // For presence requirement

// Rate limiting
static uint32_t g_last_presence_ms = 0;
static uint32_t g_last_chirp_sent_ms = 0;
static uint8_t g_relays_this_minute = 0;
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

// Callbacks
static ChirpReceivedCallback g_chirp_callback = nullptr;
static NearbyChangedCallback g_nearby_callback = nullptr;
static ChirpStateCallback g_state_callback = nullptr;

// Emoji set for session display (16 emojis, indexed by nibble)
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
// TEMPLATE TEXT LOOKUP TABLES
// Emergency-focused templates — "Witness authority, not neighbors"
// ════════════════════════════════════════════════════════════════════════════

struct TemplateEntry {
  ChirpTemplate id;
  const char* text;
  ChirpCategory category;
  bool night_allowed;  // Can be sent during night mode
};

static const TemplateEntry TEMPLATE_TABLE[] = {
  // Authority Presence — When power shows up
  { TPL_AUTH_POLICE_ACTIVITY,     "police activity in area",       CHIRP_CAT_AUTHORITY, true },
  { TPL_AUTH_HEAVY_RESPONSE,      "heavy law enforcement response", CHIRP_CAT_AUTHORITY, true },
  { TPL_AUTH_ROAD_BLOCKED_LE,     "road blocked by law enforcement", CHIRP_CAT_AUTHORITY, true },
  { TPL_AUTH_HELICOPTER,          "helicopter circling area",       CHIRP_CAT_AUTHORITY, true },
  { TPL_AUTH_FEDERAL_PRESENCE,    "federal agents in area",         CHIRP_CAT_AUTHORITY, true },

  // Infrastructure — Systems failing
  { TPL_INFRA_POWER_OUT,          "power outage",                   CHIRP_CAT_INFRA, true },
  { TPL_INFRA_WATER_ISSUE,        "water service disruption",       CHIRP_CAT_INFRA, true },
  { TPL_INFRA_GAS_SMELL,          "gas smell - evacuate?",          CHIRP_CAT_INFRA, true },
  { TPL_INFRA_INTERNET_DOWN,      "internet outage in area",        CHIRP_CAT_INFRA, false },
  { TPL_INFRA_ROAD_CLOSED,        "road closed or blocked",         CHIRP_CAT_INFRA, false },

  // Emergency — Immediate danger
  { TPL_EMERG_FIRE_VISIBLE,       "fire or smoke visible",          CHIRP_CAT_EMERGENCY, true },
  { TPL_EMERG_MEDICAL_SCENE,      "medical emergency scene",        CHIRP_CAT_EMERGENCY, true },
  { TPL_EMERG_MULTIPLE_AMBULANCE, "multiple ambulances responding", CHIRP_CAT_EMERGENCY, true },
  { TPL_EMERG_EVACUATION,         "evacuation in progress",         CHIRP_CAT_EMERGENCY, true },
  { TPL_EMERG_SHELTER_IN_PLACE,   "shelter in place advisory",      CHIRP_CAT_EMERGENCY, true },

  // Weather — Environmental threats
  { TPL_WX_SEVERE_WARNING,        "severe weather warning",         CHIRP_CAT_WEATHER, true },
  { TPL_WX_TORNADO,               "tornado warning",                CHIRP_CAT_WEATHER, true },
  { TPL_WX_FLOOD,                 "flooding reported",              CHIRP_CAT_WEATHER, true },
  { TPL_WX_LIGHTNING_CLOSE,       "dangerous lightning nearby",     CHIRP_CAT_WEATHER, true },

  // Mutual Aid — Community support
  { TPL_AID_WELFARE_CHECK,        "neighbor may need help",         CHIRP_CAT_MUTUAL_AID, false },
  { TPL_AID_SUPPLIES_NEEDED,      "supplies needed in area",        CHIRP_CAT_MUTUAL_AID, false },
  { TPL_AID_OFFERING_HELP,        "offering assistance",            CHIRP_CAT_MUTUAL_AID, false },

  // All Clear — De-escalation
  { TPL_CLR_RESOLVED,             "situation resolved",             CHIRP_CAT_ALL_CLEAR, true },
  { TPL_CLR_SAFE,                 "area appears safe now",          CHIRP_CAT_ALL_CLEAR, true },
  { TPL_CLR_FALSE_ALARM,          "false alarm",                    CHIRP_CAT_ALL_CLEAR, true },
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
static void relay_chirp(const ReceivedChirp* chirp);
static void prune_stale_nearby();
static void prune_old_chirps();
static void load_settings();
static void save_settings();
static void on_espnow_recv(const uint8_t* mac, const uint8_t* data, int len);
static const TemplateEntry* find_template(ChirpTemplate id);
static ChirpCategory template_to_category(ChirpTemplate id);
static uint32_t get_cooldown_for_tier(uint8_t tier);

// ════════════════════════════════════════════════════════════════════════════
// STATE MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

static void set_state(ChirpState new_state) {
  if (g_state == new_state) return;

  ChirpState old_state = g_state;
  g_state = new_state;

  // Log state change
  char detail[64];
  snprintf(detail, sizeof(detail), "chirp: %s -> %s",
           state_name(old_state), state_name(new_state));
  health_log(LOG_LEVEL_DEBUG, LOG_CAT_NETWORK, detail);

  // Notify callback
  if (g_state_callback) {
    g_state_callback(old_state, new_state);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// SESSION IDENTITY
// ════════════════════════════════════════════════════════════════════════════

static void generate_session_identity() {
  // Generate fresh random seed (NOT derived from device key - privacy firewall)
  uint8_t seed[32];
  esp_fill_random(seed, sizeof(seed));

  // Derive Ed25519 keypair
  Ed25519::generatePrivateKey(g_session.session_privkey);
  Ed25519::derivePublicKey(g_session.session_pubkey, g_session.session_privkey);

  // Compute session ID: SHA-256("securacv:chirp:session:v0" || pubkey)[0:8]
  mbedtls_sha256_context ctx;
  uint8_t hash[32];
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const uint8_t*)"securacv:chirp:session:v0", 25);
  mbedtls_sha256_update(&ctx, g_session.session_pubkey, 32);
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  memcpy(g_session.session_id, hash, SESSION_ID_SIZE);

  // Generate emoji display
  generate_emoji_string(g_session.session_id, g_session.emoji_display);

  g_session.created_ms = millis();
  g_session.valid = true;

  health_log(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "chirp: new session identity generated");
}

static void generate_emoji_string(const uint8_t* session_id, char* emoji_out) {
  // Use first 3 nibbles to select 3 emojis
  // Each emoji can be up to 6 bytes, so max size is 3*6+1=19 bytes
  size_t pos = 0;
  for (int i = 0; i < 3; i++) {
    uint8_t idx = session_id[i] % 16;
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
    if (TEMPLATE_TABLE[i].id == id) {
      return &TEMPLATE_TABLE[i];
    }
  }
  return nullptr;
}

static ChirpCategory template_to_category(ChirpTemplate id) {
  const TemplateEntry* entry = find_template(id);
  if (entry) return entry->category;
  return CHIRP_CAT_ALL_CLEAR;  // Default
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
    // 24h passed since first chirp, reset tiers
    g_cooldown.chirps_sent_today = 0;
    g_cooldown.first_chirp_today_ms = 0;
    g_cooldown.last_chirp_ms = 0;
  }
}

static bool is_template_night_allowed(ChirpTemplate id) {
  const TemplateEntry* entry = find_template(id);
  return entry ? entry->night_allowed : false;
}

// ════════════════════════════════════════════════════════════════════════════
// NONCE DEDUPLICATION
// ════════════════════════════════════════════════════════════════════════════

static bool is_nonce_seen(const uint8_t* nonce) {
  for (size_t i = 0; i < MAX_NONCE_CACHE; i++) {
    if (memcmp(g_nonce_cache[i], nonce, 8) == 0) {
      return true;
    }
  }
  return false;
}

static void cache_nonce(const uint8_t* nonce) {
  memcpy(g_nonce_cache[g_nonce_cache_idx], nonce, 8);
  g_nonce_cache_idx = (g_nonce_cache_idx + 1) % MAX_NONCE_CACHE;
}

// ════════════════════════════════════════════════════════════════════════════
// BROADCASTING
// ════════════════════════════════════════════════════════════════════════════

static void broadcast_message(const uint8_t* data, size_t len) {
  static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  // Add broadcast peer if not already added
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = CHIRP_CHANNEL;
  peer.encrypt = false;

  if (!esp_now_is_peer_exist(BROADCAST_ADDR)) {
    esp_now_add_peer(&peer);
  }

  esp_err_t err = esp_now_send(BROADCAST_ADDR, data, len);
  if (err != ESP_OK) {
    health_log(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "chirp: broadcast failed");
  }
}

static void send_presence() {
  uint8_t buf[sizeof(ChirpHeader) + sizeof(ChirpPresencePayload)];
  ChirpHeader* hdr = (ChirpHeader*)buf;
  ChirpPresencePayload* payload = (ChirpPresencePayload*)(buf + sizeof(ChirpHeader));

  // Fill header
  hdr->magic = CHIRP_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = CHIRP_MSG_PRESENCE;
  memcpy(hdr->session_id, g_session.session_id, SESSION_ID_SIZE);
  hdr->hop_count = 0;
  hdr->timestamp = (uint32_t)(millis() / 1000);
  esp_fill_random(hdr->nonce, 8);

  // Fill payload
  strncpy(payload->emoji, g_session.emoji_display, EMOJI_DISPLAY_SIZE);
  payload->listening = (g_state == CHIRP_ACTIVE || g_state == CHIRP_LISTENING) ? 1 : 0;

  if (g_last_chirp_sent_ms == 0) {
    payload->last_chirp_age_min = 255;  // Never sent
  } else {
    uint32_t age_ms = millis() - g_last_chirp_sent_ms;
    uint32_t age_min = age_ms / 60000;
    payload->last_chirp_age_min = (age_min > 254) ? 254 : (uint8_t)age_min;
  }

  broadcast_message(buf, sizeof(buf));
  g_last_presence_ms = millis();
}

// ════════════════════════════════════════════════════════════════════════════
// MESSAGE HANDLERS
// ════════════════════════════════════════════════════════════════════════════

static void handle_presence(const uint8_t* data, size_t len, int8_t rssi) {
  if (len < sizeof(ChirpHeader) + sizeof(ChirpPresencePayload)) return;

  const ChirpHeader* hdr = (const ChirpHeader*)data;
  const ChirpPresencePayload* payload = (const ChirpPresencePayload*)(data + sizeof(ChirpHeader));

  // Don't track ourselves
  if (memcmp(hdr->session_id, g_session.session_id, SESSION_ID_SIZE) == 0) {
    return;
  }

  // Find or create nearby device entry
  NearbyDevice* device = nullptr;
  for (size_t i = 0; i < g_nearby_count; i++) {
    if (memcmp(g_nearby_devices[i].session_id, hdr->session_id, SESSION_ID_SIZE) == 0) {
      device = &g_nearby_devices[i];
      break;
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

    if (new_device && g_nearby_callback) {
      g_nearby_callback((uint8_t)g_nearby_count);
    }
  }
}

static void handle_witness(const uint8_t* data, size_t len, int8_t rssi) {
  (void)rssi;  // Could be used for signal strength filtering in future
  if (len < sizeof(ChirpHeader) + sizeof(ChirpWitnessPayload)) return;

  const ChirpHeader* hdr = (const ChirpHeader*)data;
  const ChirpWitnessPayload* payload = (const ChirpWitnessPayload*)(data + sizeof(ChirpHeader));

  // Check nonce deduplication
  if (is_nonce_seen(hdr->nonce)) {
    return;  // Already seen this chirp
  }
  cache_nonce(hdr->nonce);

  // Check message age (max 5 minutes)
  uint32_t now_sec = millis() / 1000;
  if (hdr->timestamp > now_sec + 30 || now_sec - hdr->timestamp > 300) {
    return;  // Too old or in future
  }

  // Check urgency filter
  if (payload->urgency < (uint8_t)g_urgency_filter) {
    return;  // Below filter threshold
  }

  // Ignore if muted
  if (g_muted && millis() < g_mute_until_ms) {
    return;
  }

  // Don't process our own chirps
  if (memcmp(hdr->session_id, g_session.session_id, SESSION_ID_SIZE) == 0) {
    return;
  }

  // Extract template from wire format:
  // - msg_len byte holds template_id (ChirpTemplate)
  // - message[0] holds detail slot (ChirpDetailSlot)
  ChirpTemplate template_id = (ChirpTemplate)payload->msg_len;
  ChirpDetailSlot detail_slot = (ChirpDetailSlot)payload->message[0];

  // Validate template exists
  if (!find_template(template_id)) {
    return;  // Invalid/unknown template - reject
  }

  // Store the chirp
  if (g_recent_chirp_count < MAX_RECENT_CHIRPS) {
    ReceivedChirp* chirp = &g_recent_chirps[g_recent_chirp_count++];

    memcpy(chirp->sender_session, hdr->session_id, SESSION_ID_SIZE);
    generate_emoji_string(hdr->session_id, chirp->sender_emoji);
    chirp->template_id = template_id;
    chirp->detail = detail_slot;
    chirp->urgency = (ChirpUrgency)payload->urgency;
    chirp->hop_count = hdr->hop_count;
    chirp->received_ms = millis();
    chirp->timestamp = hdr->timestamp;
    memcpy(chirp->nonce, hdr->nonce, 8);
    chirp->confirm_count = payload->confirm_count;
    chirp->dismiss_count = 0;
    chirp->relayed = false;
    chirp->dismissed = false;
    chirp->suppressed = false;

    // Determine if validated (has enough witness confirmations)
    // Safety templates only need 1, others need CONFIRMATIONS_REQUIRED
    ChirpCategory cat = template_to_category(template_id);
    uint8_t needed = (cat == CHIRP_CAT_EMERGENCY || cat == CHIRP_CAT_WEATHER)
                     ? CONFIRMATIONS_SAFETY : CONFIRMATIONS_REQUIRED;
    chirp->validated = (payload->confirm_count >= needed);

    // Notify callback
    if (g_chirp_callback) {
      g_chirp_callback(chirp);
    }

    // Log reception
    const char* tpl_text = get_template_text(template_id);
    char log_detail[80];
    snprintf(log_detail, sizeof(log_detail), "chirp: %s (%d confirms, hop %d)",
             tpl_text, payload->confirm_count, hdr->hop_count);
    health_log(LOG_LEVEL_INFO, LOG_CAT_NETWORK, log_detail);

    // Only relay if validated and under hop limit
    if (g_relay_enabled && chirp->validated && hdr->hop_count < MAX_HOP_COUNT) {
      relay_chirp(chirp);
    }
  }
}

static void handle_ack(const uint8_t* data, size_t len) {
  if (len < sizeof(ChirpHeader) + sizeof(ChirpAckPayload)) return;

  const ChirpHeader* hdr = (const ChirpHeader*)data;
  const ChirpAckPayload* payload = (const ChirpAckPayload*)(data + sizeof(ChirpHeader));

  // Find the chirp being acknowledged
  for (size_t i = 0; i < g_recent_chirp_count; i++) {
    if (memcmp(g_recent_chirps[i].nonce, payload->original_nonce, 8) == 0) {
      ReceivedChirp* chirp = &g_recent_chirps[i];

      if (payload->ack_type == CHIRP_ACK_CONFIRMED) {
        // Human witness confirmation - increment count
        chirp->confirm_count++;

        // Check if now validated and can relay
        ChirpCategory cat = template_to_category(chirp->template_id);
        uint8_t needed = (cat == CHIRP_CAT_EMERGENCY || cat == CHIRP_CAT_WEATHER)
                         ? CONFIRMATIONS_SAFETY : CONFIRMATIONS_REQUIRED;

        if (!chirp->validated && chirp->confirm_count >= needed) {
          chirp->validated = true;
          // Relay now that it's validated
          if (g_relay_enabled && !chirp->relayed && chirp->hop_count < MAX_HOP_COUNT) {
            relay_chirp(chirp);
          }
        }
      } else if (payload->ack_type == CHIRP_ACK_SEEN) {
        // Device saw it - track for presence
      } else if (payload->ack_type == CHIRP_ACK_RESOLVED) {
        // User says resolved - treat as de-escalation
        chirp->dismissed = true;
      }
      break;
    }
  }

  (void)hdr;  // Could validate sender
}

static void handle_mute(const uint8_t* data, size_t len) {
  // Just log that a neighbor muted - we don't track individual mutes
  (void)data;
  (void)len;
}

static void relay_chirp(const ReceivedChirp* chirp) {
  if (!chirp) return;

  // Rate limit relays
  uint32_t now = millis();
  if (now - g_relay_minute_start > 60000) {
    g_relay_minute_start = now;
    g_relays_this_minute = 0;
  }

  if (g_relays_this_minute >= MAX_RELAYS_PER_MINUTE) {
    return;  // Rate limited
  }

  // Build relay message with incremented hop count
  uint8_t buf[sizeof(ChirpHeader) + sizeof(ChirpWitnessPayload)];
  ChirpHeader* hdr = (ChirpHeader*)buf;
  ChirpWitnessPayload* payload = (ChirpWitnessPayload*)(buf + sizeof(ChirpHeader));

  hdr->magic = CHIRP_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = CHIRP_MSG_WITNESS;
  memcpy(hdr->session_id, chirp->sender_session, SESSION_ID_SIZE);  // Keep original sender
  hdr->hop_count = chirp->hop_count + 1;
  hdr->timestamp = chirp->timestamp;  // Keep original timestamp
  memcpy(hdr->nonce, chirp->nonce, 8);  // Keep original nonce for dedup

  // Encode template in wire format
  payload->category = (uint8_t)template_to_category(chirp->template_id);
  payload->urgency = (uint8_t)chirp->urgency;
  payload->confirm_count = chirp->confirm_count;  // Include confirmation count
  payload->ttl_minutes = 15;  // Default TTL
  payload->msg_len = (uint8_t)chirp->template_id;  // Template ID in msg_len
  payload->message[0] = (uint8_t)chirp->detail;    // Detail slot in message[0]
  memset(payload->message + 1, 0, MAX_MESSAGE_LEN - 1);
  memset(payload->signature, 0, 64);  // Note: Relay doesn't re-sign

  broadcast_message(buf, sizeof(buf));
  g_relays_this_minute++;

  // Mark chirp as relayed
  for (size_t i = 0; i < g_recent_chirp_count; i++) {
    if (memcmp(g_recent_chirps[i].nonce, chirp->nonce, 8) == 0) {
      g_recent_chirps[i].relayed = true;
      break;
    }
  }

  health_log(LOG_LEVEL_DEBUG, LOG_CAT_NETWORK, "chirp: relayed");
}

// ════════════════════════════════════════════════════════════════════════════
// ESP-NOW CALLBACK
// ════════════════════════════════════════════════════════════════════════════

static void on_espnow_recv(const uint8_t* mac, const uint8_t* data, int len) {
  (void)mac;  // Unused - we use session IDs, not MACs

  if (len < sizeof(ChirpHeader)) return;
  if (g_state == CHIRP_DISABLED) return;

  const ChirpHeader* hdr = (const ChirpHeader*)data;

  // Verify magic and version
  if (hdr->magic != CHIRP_MAGIC) return;
  if (hdr->version != PROTOCOL_VERSION) return;

  // Get RSSI (approximate)
  int8_t rssi = -50;  // TODO: Get actual RSSI from ESP-NOW

  switch (hdr->msg_type) {
    case CHIRP_MSG_PRESENCE:
      handle_presence(data, len, rssi);
      break;
    case CHIRP_MSG_WITNESS:
      handle_witness(data, len, rssi);
      break;
    case CHIRP_MSG_ACK:
      handle_ack(data, len);
      break;
    case CHIRP_MSG_MUTE:
      handle_mute(data, len);
      break;
    default:
      break;
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
      if (write_idx != i) {
        g_nearby_devices[write_idx] = g_nearby_devices[i];
      }
      write_idx++;
    }
  }

  if (write_idx != g_nearby_count) {
    g_nearby_count = write_idx;
    if (g_nearby_callback) {
      g_nearby_callback((uint8_t)g_nearby_count);
    }
  }
}

static void prune_old_chirps() {
  uint32_t now = millis();
  size_t write_idx = 0;

  for (size_t i = 0; i < g_recent_chirp_count; i++) {
    // Keep for 30 minutes unless dismissed
    bool keep = !g_recent_chirps[i].dismissed &&
                (now - g_recent_chirps[i].received_ms < DEFAULT_DISPLAY_MS);
    if (keep) {
      if (write_idx != i) {
        g_recent_chirps[write_idx] = g_recent_chirps[i];
      }
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
  if (nvs_get_u8("chirp_relay", &val)) {
    g_relay_enabled = (val != 0);
  }
  if (nvs_get_u8("chirp_filter", &val)) {
    g_urgency_filter = (ChirpUrgency)val;
  }
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

  // Clear state
  memset(&g_session, 0, sizeof(g_session));
  memset(g_recent_chirps, 0, sizeof(g_recent_chirps));
  memset(g_nearby_devices, 0, sizeof(g_nearby_devices));
  memset(g_nonce_cache, 0, sizeof(g_nonce_cache));
  g_recent_chirp_count = 0;
  g_nearby_count = 0;
  g_nonce_cache_idx = 0;

  // Load settings from NVS
  load_settings();

  // Register ESP-NOW callback (shared with mesh_network)
  // Note: The main firmware should route chirp messages to us
  // based on CHIRP_MAGIC byte

  g_initialized = true;
  health_log(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "chirp channel initialized");

  return true;
}

void deinit() {
  if (!g_initialized) return;

  disable();
  g_initialized = false;
}

bool enable() {
  if (!g_initialized) return false;
  if (g_state != CHIRP_DISABLED) return true;  // Already enabled

  set_state(CHIRP_INITIALIZING);

  // Generate new ephemeral session identity
  generate_session_identity();

  // Initialize ESP-NOW if not already done
  if (esp_now_init() != ESP_OK) {
    // May already be initialized by mesh_network
  }

  // Reset cooldown tracking for new session
  memset(&g_cooldown, 0, sizeof(g_cooldown));

  // Track session start time for presence requirement
  g_session_start_ms = millis();

  set_state(CHIRP_ACTIVE);

  // Send initial presence
  send_presence();

  return true;
}

void disable() {
  if (g_state == CHIRP_DISABLED) return;

  // Clear session identity (privacy - don't keep it around)
  memset(&g_session, 0, sizeof(g_session));

  // Clear nearby and recent data
  g_nearby_count = 0;
  g_recent_chirp_count = 0;

  set_state(CHIRP_DISABLED);
}

bool is_enabled() {
  return g_state != CHIRP_DISABLED;
}

void update() {
  if (g_state == CHIRP_DISABLED) return;

  uint32_t now = millis();

  // Check 24h cooldown reset
  reset_cooldown_if_stale();

  // Check mute timeout
  if (g_muted && now >= g_mute_until_ms) {
    g_muted = false;
    if (g_state == CHIRP_MUTED) {
      set_state(CHIRP_ACTIVE);
    }
  }

  // Check escalating cooldown timeout
  if (g_state == CHIRP_COOLDOWN) {
    uint32_t cooldown_ms = get_cooldown_for_tier(g_cooldown.chirps_sent_today);
    if (now - g_cooldown.last_chirp_ms >= cooldown_ms) {
      set_state(CHIRP_ACTIVE);
    }
  }

  // Send presence beacon
  if (now - g_last_presence_ms >= PRESENCE_INTERVAL_MS) {
    send_presence();
  }

  // Prune stale data
  static uint32_t last_prune_ms = 0;
  if (now - last_prune_ms > 30000) {  // Every 30 seconds
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
    if (elapsed < cooldown_ms) {
      status.cooldown_remaining_ms = cooldown_ms - elapsed;
    } else {
      status.cooldown_remaining_ms = 0;
    }
  } else {
    status.cooldown_remaining_ms = 0;
  }

  status.relay_enabled = g_relay_enabled;
  status.muted = g_muted;

  if (g_muted && millis() < g_mute_until_ms) {
    status.mute_remaining_ms = g_mute_until_ms - millis();
  } else {
    status.mute_remaining_ms = 0;
  }

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
    default:               return "unknown";
  }
}

bool is_active() {
  return g_state == CHIRP_ACTIVE || g_state == CHIRP_LISTENING;
}

bool has_presence_requirement() {
  // Must be active for PRESENCE_REQUIRED_MS before can send
  if (g_session_start_ms == 0) return false;
  return (millis() - g_session_start_ms) >= PRESENCE_REQUIRED_MS;
}

bool can_send_chirp() {
  if (g_state == CHIRP_DISABLED || g_state == CHIRP_COOLDOWN) {
    return false;
  }
  // Presence requirement: must be active for 10 minutes first
  if (!has_presence_requirement()) {
    return false;
  }
  return true;
}

bool is_night_mode() {
  // Check if current hour is during night mode (22:00-06:00)
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  if (tm_info) {
    int hour = tm_info->tm_hour;
    return (hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR);
  }
  return false;  // If time unknown, assume day
}

bool is_valid_template(ChirpTemplate template_id) {
  return find_template(template_id) != nullptr;
}

const char* get_template_text(ChirpTemplate template_id) {
  const TemplateEntry* entry = find_template(template_id);
  return entry ? entry->text : "unknown alert";
}

const char* get_detail_text(ChirpDetailSlot detail) {
  for (size_t i = 0; i < DETAIL_COUNT; i++) {
    if (DETAIL_TABLE[i].id == detail) {
      return DETAIL_TABLE[i].text;
    }
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
  if (elapsed >= cooldown_ms) return 0;
  return cooldown_ms - elapsed;
}

const char* get_validation_status(const ReceivedChirp* chirp) {
  if (!chirp) return "unknown";
  if (chirp->suppressed) return "suppressed";
  if (chirp->validated) return "validated";
  ChirpCategory cat = template_to_category(chirp->template_id);
  uint8_t needed = (cat == CHIRP_CAT_EMERGENCY || cat == CHIRP_CAT_WEATHER)
                   ? CONFIRMATIONS_SAFETY : CONFIRMATIONS_REQUIRED;
  if (chirp->confirm_count < needed) return "awaiting_confirmation";
  return "validated";
}

bool send_chirp(ChirpTemplate template_id, ChirpUrgency urgency,
                ChirpDetailSlot detail, uint8_t ttl_minutes) {
  if (!can_send_chirp()) {
    return false;
  }

  // Validate template
  const TemplateEntry* entry = find_template(template_id);
  if (!entry) {
    health_log(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "chirp: invalid template");
    return false;
  }

  // Night mode restriction
  if (is_night_mode() && !entry->night_allowed) {
    health_log(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "chirp: template not allowed at night");
    return false;
  }

  // Build message
  uint8_t buf[sizeof(ChirpHeader) + sizeof(ChirpWitnessPayload)];
  ChirpHeader* hdr = (ChirpHeader*)buf;
  ChirpWitnessPayload* payload = (ChirpWitnessPayload*)(buf + sizeof(ChirpHeader));

  // Fill header
  hdr->magic = CHIRP_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = CHIRP_MSG_WITNESS;
  memcpy(hdr->session_id, g_session.session_id, SESSION_ID_SIZE);
  hdr->hop_count = 0;
  hdr->timestamp = (uint32_t)(millis() / 1000);
  esp_fill_random(hdr->nonce, 8);

  // Fill payload with template-based format
  payload->category = (uint8_t)entry->category;
  payload->urgency = (uint8_t)urgency;
  payload->confirm_count = 1;  // We're the first confirmer
  payload->ttl_minutes = ttl_minutes;
  payload->msg_len = (uint8_t)template_id;  // Template ID in msg_len field
  payload->message[0] = (uint8_t)detail;     // Detail slot in message[0]
  memset(payload->message + 1, 0, MAX_MESSAGE_LEN - 1);

  // Sign the message with session key
  uint8_t sign_input[128];
  size_t sign_len = 0;
  memcpy(sign_input + sign_len, "securacv:chirp:witness:v0", 25);
  sign_len += 25;
  memcpy(sign_input + sign_len, hdr->nonce, 8);
  sign_len += 8;
  sign_input[sign_len++] = payload->category;
  sign_input[sign_len++] = payload->urgency;
  sign_input[sign_len++] = (uint8_t)template_id;
  sign_input[sign_len++] = (uint8_t)detail;

  Ed25519::sign(payload->signature, g_session.session_privkey,
                g_session.session_pubkey, sign_input, sign_len);

  // Broadcast
  broadcast_message(buf, sizeof(buf));

  // Update cooldown tracking
  uint32_t now = millis();
  if (g_cooldown.chirps_sent_today == 0) {
    g_cooldown.first_chirp_today_ms = now;
  }
  g_cooldown.chirps_sent_today++;
  g_cooldown.last_chirp_ms = now;
  g_last_chirp_sent_ms = now;
  cache_nonce(hdr->nonce);  // Don't process our own chirp
  set_state(CHIRP_COOLDOWN);

  // Log
  char log_detail[96];
  snprintf(log_detail, sizeof(log_detail), "chirp sent: %s (%s, tier %d)",
           entry->text, urgency_name(urgency), g_cooldown.chirps_sent_today);
  health_log(LOG_LEVEL_INFO, LOG_CAT_NETWORK, log_detail);

  return true;
}

bool send_all_clear(ChirpTemplate clear_type) {
  // Validate it's actually an all-clear template
  if (clear_type != TPL_CLR_RESOLVED &&
      clear_type != TPL_CLR_SAFE &&
      clear_type != TPL_CLR_FALSE_ALARM) {
    clear_type = TPL_CLR_RESOLVED;  // Default to resolved
  }
  return send_chirp(clear_type, CHIRP_URG_INFO, DETAIL_NONE, 15);
}

const ReceivedChirp* get_recent_chirps(size_t* count) {
  if (count) *count = g_recent_chirp_count;
  return g_recent_chirps;
}

const ReceivedChirp* get_pending_chirps(size_t* count) {
  // Return chirps that are not yet validated (awaiting confirmation)
  static ReceivedChirp pending[MAX_RECENT_CHIRPS];
  size_t pending_count = 0;

  for (size_t i = 0; i < g_recent_chirp_count; i++) {
    if (!g_recent_chirps[i].validated && !g_recent_chirps[i].dismissed) {
      pending[pending_count++] = g_recent_chirps[i];
    }
  }

  if (count) *count = pending_count;
  return pending;
}

bool confirm_chirp(const uint8_t* nonce) {
  // Find the chirp and add our witness confirmation
  for (size_t i = 0; i < g_recent_chirp_count; i++) {
    if (memcmp(g_recent_chirps[i].nonce, nonce, 8) == 0) {
      // Send a confirmation acknowledgment
      uint8_t buf[sizeof(ChirpHeader) + sizeof(ChirpAckPayload)];
      ChirpHeader* hdr = (ChirpHeader*)buf;
      ChirpAckPayload* payload = (ChirpAckPayload*)(buf + sizeof(ChirpHeader));

      hdr->magic = CHIRP_MAGIC;
      hdr->version = PROTOCOL_VERSION;
      hdr->msg_type = CHIRP_MSG_ACK;
      memcpy(hdr->session_id, g_session.session_id, SESSION_ID_SIZE);
      hdr->hop_count = 0;
      hdr->timestamp = (uint32_t)(millis() / 1000);
      esp_fill_random(hdr->nonce, 8);

      memcpy(payload->original_nonce, nonce, 8);
      payload->ack_type = (uint8_t)CHIRP_ACK_CONFIRMED;

      broadcast_message(buf, sizeof(buf));

      health_log(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "chirp: confirmed witness");
      return true;
    }
  }
  return false;
}

static bool acknowledge_chirp(const uint8_t* nonce, ChirpAckType ack_type) {
  // Build ACK message
  uint8_t buf[sizeof(ChirpHeader) + sizeof(ChirpAckPayload)];
  ChirpHeader* hdr = (ChirpHeader*)buf;
  ChirpAckPayload* payload = (ChirpAckPayload*)(buf + sizeof(ChirpHeader));

  hdr->magic = CHIRP_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = CHIRP_MSG_ACK;
  memcpy(hdr->session_id, g_session.session_id, SESSION_ID_SIZE);
  hdr->hop_count = 0;
  hdr->timestamp = (uint32_t)(millis() / 1000);
  esp_fill_random(hdr->nonce, 8);

  memcpy(payload->original_nonce, nonce, 8);
  payload->ack_type = (uint8_t)ack_type;

  broadcast_message(buf, sizeof(buf));
  return true;
}

bool dismiss_chirp(const uint8_t* nonce) {
  for (size_t i = 0; i < g_recent_chirp_count; i++) {
    if (memcmp(g_recent_chirps[i].nonce, nonce, 8) == 0) {
      g_recent_chirps[i].dismissed = true;
      return true;
    }
  }
  return false;
}

void clear_chirps() {
  g_recent_chirp_count = 0;
}

uint8_t get_nearby_count() {
  return (uint8_t)g_nearby_count;
}

const NearbyDevice* get_nearby_devices(size_t* count) {
  if (count) *count = g_nearby_count;
  return g_nearby_devices;
}

bool mute(uint8_t duration_minutes) {
  if (duration_minutes != 15 && duration_minutes != 30 &&
      duration_minutes != 60 && duration_minutes != 120) {
    return false;  // Invalid duration
  }

  g_muted = true;
  g_mute_until_ms = millis() + (duration_minutes * 60000UL);

  // Broadcast mute to neighbors (optional courtesy)
  uint8_t buf[sizeof(ChirpHeader) + sizeof(ChirpMutePayload)];
  ChirpHeader* hdr = (ChirpHeader*)buf;
  ChirpMutePayload* payload = (ChirpMutePayload*)(buf + sizeof(ChirpHeader));

  hdr->magic = CHIRP_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = CHIRP_MSG_MUTE;
  memcpy(hdr->session_id, g_session.session_id, SESSION_ID_SIZE);
  hdr->hop_count = 0;
  hdr->timestamp = (uint32_t)(millis() / 1000);
  esp_fill_random(hdr->nonce, 8);

  payload->duration_minutes = duration_minutes;
  payload->reason = 255;  // Not specified

  broadcast_message(buf, sizeof(buf));

  set_state(CHIRP_MUTED);
  return true;
}

void unmute() {
  g_muted = false;
  g_mute_until_ms = 0;
  if (g_state == CHIRP_MUTED) {
    set_state(CHIRP_ACTIVE);
  }
}

bool is_muted() {
  return g_muted && millis() < g_mute_until_ms;
}

void set_relay_enabled(bool enabled) {
  g_relay_enabled = enabled;
  save_settings();
}

bool is_relay_enabled() {
  return g_relay_enabled;
}

void set_urgency_filter(ChirpUrgency min_urgency) {
  g_urgency_filter = min_urgency;
  save_settings();
}

ChirpUrgency get_urgency_filter() {
  return g_urgency_filter;
}

void set_chirp_callback(ChirpReceivedCallback callback) {
  g_chirp_callback = callback;
}

void set_nearby_callback(NearbyChangedCallback callback) {
  g_nearby_callback = callback;
}

void set_state_callback(ChirpStateCallback callback) {
  g_state_callback = callback;
}

const char* get_session_emoji() {
  return g_session.emoji_display;
}

const uint8_t* get_session_id() {
  return g_session.session_id;
}

// ESP-NOW receive dispatcher - called by main firmware
void dispatch_espnow_message(const uint8_t* mac, const uint8_t* data, int len) {
  on_espnow_recv(mac, data, len);
}

} // namespace chirp_channel
