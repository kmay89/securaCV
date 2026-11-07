// The sealed bearer (api_seal.h, seal_api.h, api_auth.h's Sealed branch):
// the token the Flasher's fleet book sends a Canary rides sealed under a key
// agreed with this device's identity key, so a host answering to the
// device's name, or relaying to it, cannot read it.
//
// This runs the REAL api_seal.h over real X25519, HKDF-SHA256, SHA-256 and
// ChaCha20-Poly1305 (OpenSSL behind stubs/pair_crypto and stubs/mesh_net),
// the real device_signature.cpp's canonical and gates, and the REAL
// api_auth.h (api_auth_check_bearer and api_auth_bearer_valid over
// stubs/receipt_grant's request) with this file's api_seal_open(), the
// twin of seal_api.h's. What it pins:
//   - the canonical's shape and field order (device_id, nonce, client key,
//     device key) and both key gates;
//   - against REFERENCE VECTORS computed by python-cryptography (an
//     implementation neither this header nor the Rust client shares):
//     the ephemeral public key, the shared secret, the transcript, the
//     session key, the session id, and two sealed headers the Rust client
//     (desktop/src-tauri/src/seal.rs) pins to the same bytes — so the two
//     ends agree on every byte or one of the three suites says so;
//   - a sealed header opens once (its counter moves), a replay is refused,
//     a lower counter is refused, a header for another method or path is
//     refused (the associated data), a tampered byte is refused, an
//     unknown session, an expired one and a malformed value each get their
//     own word;
//   - the table holds four sessions and a fifth evicts the oldest; an
//     all-zero shared secret (a low-order client key) opens nothing;
//   - the REAL api_auth_check_bearer admits the sealed token, answers 401
//     with the seal's word when the seal does not open (one strike, like
//     any wrong credential), 403 when the token inside is not this
//     device's, and still takes a plain Bearer; api_auth_bearer_valid
//     takes the sealed token too;
//   - the sketch's text: canary_wap.ino includes seal_api.h and registers
//     GET /api/device/seal on seal_api::handle_seal; seal_api.h opens the
//     session through api_seal::open_session, signs through
//     device_signature::sign_seal and defines api_seal_open over
//     api_seal::unseal; api_auth.h reads the credential through one reader
//     in both gates and names the hook.
//
// Run: make -C firmware/projects/canary-wap/tests_host run-api-seal

#include "Arduino.h"           // stubs/receipt_grant
#include "esp_http_server.h"   // stubs/receipt_grant
#include "peer_source.h"       // stubs/receipt_grant

#include "api_auth.h"          // the REAL one
#include "api_seal.h"          // the REAL one

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

// The sketch's lockout source read, over the stub peer (as the receipt
// suites cut it): every request here is one address unless a test moves it.
uint32_t auth_request_source(httpd_req_t* req) {
  uint8_t key[canary::net::provisioning_gate::PEER_KEY_LEN];
  if (!request_peer_key(req, key)) return 0;
  return canary::net::provisioning_gate::ipv4_host_order_from_addr(
      canary::net::provisioning_gate::AddrFamily::IPV6, key);
}

// The request's method and URI, which the stub request does not carry:
// seal_api.h's hook reads req->method and req->uri; this one reads these.
static const char* g_method = "GET";
static const char* g_uri = "/api/ota/status";

// seal_api.h's hook, as the sketch defines it (pinned by text below).
bool api_seal_open(httpd_req_t* req, const char* value, char* token_out, size_t token_cap,
                   const char** reason) {
  (void)req;
  const api_seal::Unseal u = api_seal::unseal(api_seal::g_table, g_method, g_uri, value, millis(),
                                              token_out, token_cap);
  if (reason) *reason = api_seal::reason(u);
  return u == api_seal::Unseal::OK;
}

namespace {

int g_checks = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

void check_eq(const std::string& got, const std::string& want, const char* what) {
  ++g_checks;
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s\n  got:  '%s'\n  want: '%s'\n", what, got.c_str(), want.c_str());
    std::exit(1);
  }
}

std::string hex(const uint8_t* p, size_t n) {
  char out[2 * 80 + 1];
  api_seal::hex_encode(p, n, out);
  return out;
}

void unhex(const std::string& s, uint8_t* out) {
  check(api_seal::hex_decode(s.c_str(), s.size() / 2, out), "test hex decodes");
}

// ── the reference vectors (scratch gen.py, python-cryptography) ────────────
const char* const DEVICE_ID  = "canary_wap_a1";
const char* const NONCE      = "00112233445566778899aabbccddeeff";
const char* const CLIENT_PRIV = "1011111111111111111111111111111111111111111111111111111111111151";
const char* const CLIENT_PUB  = "7b4e909bbe7ffe44c465a220037d608ee35897d31ef972f07f74892cb0f73f13";
const char* const DEVICE_PUB  = "0faa684ed28867b97f4a6a2dee5df8ce974e76b7018e3f22a1c4cf2678570f20";
const char* const SHARED      = "9e004098efc091d4ec2663b4e9f5cfd4d7064571690b4bea97ab146ab9f35056";
const char* const CANONICAL   =
    "securacv-canary-sig|v1|seal|canary_wap_a1|00112233445566778899aabbccddeeff|"
    "7b4e909bbe7ffe44c465a220037d608ee35897d31ef972f07f74892cb0f73f13|"
    "0faa684ed28867b97f4a6a2dee5df8ce974e76b7018e3f22a1c4cf2678570f20";
const char* const KEY         = "634cb1324c4783a5163c33dd458a926e225b1094aeab74750315524d9b6aef44";
const char* const SID         = "c47ccac8932f3cb2";
const char* const TOKEN       = "cv_a1B2c3D4e5F6g7H8i9J0k1L2m3N4o5P6";
const char* const SEALED_IDENTIFY_1 =
    "c47ccac8932f3cb2.1.e935397a49b1784f0a00e9b882c4510f67a9cefc069d1432865ed42aef79a0b1dc394d6257b29775cbbc4639caf91ba4bc28dc";
const char* const SEALED_STATUS_2 =
    "c47ccac8932f3cb2.2.4da26059880d43c037ac09fa4b96861865b91b5fe4f0699240e198651facd214fc8cd711968a92f1e8b4ada2c3e1f47475bc4b";

// The device's ephemeral scalar, before clamping: 0x22 bytes (the vector's
// device_priv is its clamped form).
void fill_22(void* buf, size_t len) { memset(buf, 0x22, len); }
uint8_t g_fill_byte = 0x33;
void fill_counted(void* buf, size_t len) { memset(buf, g_fill_byte++, len); }

// A client-side seal over the stub AEAD, as seal.rs does it.
std::string seal(const uint8_t key[32], const char* sid_hex, uint32_t seq, const char* method,
                 const char* path, const char* token) {
  uint8_t iv[12] = {0};
  iv[8] = (uint8_t)(seq >> 24); iv[9] = (uint8_t)(seq >> 16);
  iv[10] = (uint8_t)(seq >> 8); iv[11] = (uint8_t)seq;
  std::string aad = std::string(method) + " " + path;
  ChaChaPoly aead;
  check(aead.setKey(key, 32) && aead.setIV(iv, 12), "client AEAD takes the key and iv");
  aead.addAuthData(aad.data(), aad.size());
  uint8_t ct[96], tag[16];
  const size_t n = strlen(token);
  aead.encrypt(ct, (const uint8_t*)token, n);
  aead.computeTag(tag, 16);
  aead.clear();
  std::ostringstream o;
  o << sid_hex << "." << seq << "." << hex(ct, n) << hex(tag, 16);
  return o.str();
}

api_seal::Table fresh() {
  api_seal::Table t = {};
  return t;
}

// ── canonical and gates ────────────────────────────────────────────────────
void test_canonical_and_gates() {
  char out[320];
  const size_t n = device_signature::build_seal_canonical(NONCE, CLIENT_PUB, DEVICE_PUB, DEVICE_ID,
                                                          out, sizeof(out));
  check(n > 0, "the seal canonical builds");
  check_eq(std::string(out, n), CANONICAL, "the seal canonical: prefix|v1|seal|device_id|nonce|client|device");
  check(device_signature::seal_pub_hex_ok(CLIENT_PUB), "a 64-hex key passes the gate");
  check(!device_signature::seal_pub_hex_ok("7B4E909BBE7FFE44C465A220037D608EE35897D31EF972F07F74892CB0F73F13"),
        "uppercase is refused");
  check(!device_signature::seal_pub_hex_ok(std::string(CLIENT_PUB).substr(0, 63).c_str()), "63 chars refused");
  check(!device_signature::seal_pub_hex_ok((std::string(CLIENT_PUB) + "0").c_str()), "65 chars refused");
  check(!device_signature::seal_pub_hex_ok(nullptr), "null refused");
  // A truncated buffer writes nothing: never a half canonical to sign.
  char small[64];
  check(device_signature::build_seal_canonical(NONCE, CLIENT_PUB, DEVICE_PUB, DEVICE_ID, small,
                                               sizeof(small)) == 0 && small[0] == '\0',
        "a canonical that does not fit is cleared, not cut");
  // The host build signs nothing (no Ed25519 here); the gate still refuses
  // bad input first, so a bad key never reaches the signer on the device.
  char sig[device_signature::SIG_HEX_CAP];
  check(!device_signature::sign_seal("ZZ", CLIENT_PUB, DEVICE_PUB, sig, sizeof(sig)),
        "a bad nonce is refused unsigned");
  std::printf("PASS canonical_and_gates\n");
}

// ── the handshake against the reference vectors ────────────────────────────
void test_open_session_matches_the_vectors() {
  api_seal::Table t = fresh();
  api_seal::Opened o;
  const api_seal::Open r = api_seal::open_session(t, DEVICE_ID, NONCE, CLIENT_PUB, fill_22, 1000, o);
  check(r == api_seal::Open::OK, "the handshake opens");
  check_eq(o.device_pub_hex, DEVICE_PUB, "the device's ephemeral public key (clamped 0x22 scalar)");
  check_eq(o.canonical, CANONICAL, "the canonical the identity key signs");
  check_eq(o.sid_hex, SID, "the session id is the transcript's first 8 bytes");
  check(t.slot[0].live && t.slot[0].last_seq == 0 && t.slot[0].opened_ms == 1000, "slot 0 holds it");
  check_eq(hex(t.slot[0].key, 32), KEY, "the session key: HKDF(salt=transcript, ikm=shared, info)");
  check_eq(hex(t.slot[0].sid, 8), SID, "the stored sid");

  // The client side reaches the same key from its own scalar (the stub's
  // X25519 is OpenSSL's): the agreement is real, not a mock that agrees.
  uint8_t cpriv[32], dpub[32], shared[32];
  unhex(CLIENT_PRIV, cpriv);
  unhex(DEVICE_PUB, dpub);
  check(Curve25519::eval(shared, cpriv, dpub), "client X25519");
  check_eq(hex(shared, 32), SHARED, "the shared secret both sides reach");

  // The answer's JSON: the enroll card's names, then the seal's.
  api_seal::Answer a = {};
  a.device_id = DEVICE_ID;
  a.pubkey_hex = "ea4a6c63e29c520abef5507b132ec5f9954776aebebe7b92421eea691446d22c";
  a.fingerprint_hex = "015c29625b4dea08";
  a.nonce = NONCE;
  a.sid_hex = o.sid_hex;
  a.device_pub_hex = o.device_pub_hex;
  a.sig_hex = "ab";
  a.ttl_s = api_seal::TTL_MS / 1000;
  const size_t need = api_seal::answer(a, nullptr, 0);
  check(need > 0, "the answer measures");
  std::string json(need + 1, '\0');
  check(api_seal::answer(a, &json[0], need + 1) == need, "the answer writes its measured length");
  json.resize(need);
  check_eq(json,
           std::string("{\"device_id\":\"canary_wap_a1\",\"pubkey_hex\":\"ea4a6c63e29c520abef5507b132ec5f99"
                       "54776aebebe7b92421eea691446d22c\",\"fingerprint_hex\":\"015c29625b4dea08\",\"alg\":"
                       "\"ed25519\",\"v\":1,\"nonce\":\"") + NONCE + "\",\"seal\":{\"v\":1,\"sid\":\"" + SID +
               "\",\"device_pub_hex\":\"" + DEVICE_PUB + "\",\"sig_hex\":\"ab\",\"ttl_s\":900}}",
           "the answer's field names and order");
  char tiny[16];
  check(api_seal::answer(a, tiny, sizeof(tiny)) == need && tiny[0] == '\0',
        "a buffer too small writes nothing and says what it needed");
  std::printf("PASS open_session_matches_the_vectors\n");
}

void test_open_session_refusals() {
  api_seal::Table t = fresh();
  api_seal::Opened o;
  check(api_seal::open_session(t, DEVICE_ID, "short", CLIENT_PUB, fill_22, 0, o) == api_seal::Open::BAD_NONCE,
        "a short nonce");
  check(api_seal::open_session(t, DEVICE_ID, NONCE, "zz", fill_22, 0, o) == api_seal::Open::BAD_CLIENT_PUB,
        "a bad client key");
  check(api_seal::open_session(t, DEVICE_ID, NONCE, CLIENT_PUB, nullptr, 0, o) == api_seal::Open::NO_RANDOM,
        "no random source");
  // The all-zero point: X25519 answers all zero, which is refused, so no
  // session key is ever derived from a peer who chose the result.
  const std::string zero(64, '0');
  check(api_seal::open_session(t, DEVICE_ID, NONCE, zero.c_str(), fill_22, 0, o) == api_seal::Open::NO_SHARED,
        "a low-order client key opens nothing");
  for (size_t i = 0; i < api_seal::SLOTS; ++i) check(!t.slot[i].live, "no refusal leaves a session");
  check(o.device_pub_hex[0] == '\0' && o.canonical[0] == '\0', "a refusal hands nothing out");
  std::printf("PASS open_session_refusals\n");
}

// ── sealed headers: the vectors, once each, bound, no replay ───────────────
void test_unseal_the_reference_headers() {
  api_seal::Table t = fresh();
  api_seal::Opened o;
  check(api_seal::open_session(t, DEVICE_ID, NONCE, CLIENT_PUB, fill_22, 1000, o) == api_seal::Open::OK, "open");
  uint8_t key[32];
  unhex(KEY, key);
  // This suite's client seal reproduces the Python vectors byte for byte...
  check_eq(seal(key, SID, 1, "POST", "/api/identify", TOKEN), SEALED_IDENTIFY_1, "the client seal (identify, seq 1)");
  check_eq(seal(key, SID, 2, "GET", "/api/ota/status", TOKEN), SEALED_STATUS_2, "the client seal (status, seq 2)");
  // ...and the device opens them, in order, once each.
  char tok[api_seal::TOKEN_CAP];
  check(api_seal::unseal(t, "POST", "/api/identify", SEALED_IDENTIFY_1, 2000, tok, sizeof(tok)) == api_seal::Unseal::OK,
        "the identify seal opens");
  check_eq(tok, TOKEN, "and yields the token");
  check(t.slot[0].last_seq == 1, "the counter moved to 1");
  check(api_seal::unseal(t, "POST", "/api/identify", SEALED_IDENTIFY_1, 2000, tok, sizeof(tok)) == api_seal::Unseal::REPLAY,
        "the same header again is a replay");
  check(tok[0] == '\0', "a refusal yields no token");
  // The query is not part of the path the seal binds.
  check(api_seal::unseal(t, "GET", "/api/ota/status?x=1", SEALED_STATUS_2, 2000, tok, sizeof(tok)) == api_seal::Unseal::OK,
        "the status seal opens (query ignored)");
  check_eq(tok, TOKEN, "the token again");
  check(t.slot[0].last_seq == 2, "the counter moved to 2");
  // Seq 3 on the wrong method or path: the associated data refuses it.
  const std::string s3 = seal(key, SID, 3, "POST", "/api/ota/install", TOKEN);
  check(api_seal::unseal(t, "GET", "/api/ota/install", s3.c_str(), 2000, tok, sizeof(tok)) == api_seal::Unseal::BAD_TAG,
        "another method is refused");
  check(api_seal::unseal(t, "POST", "/api/ota/check", s3.c_str(), 2000, tok, sizeof(tok)) == api_seal::Unseal::BAD_TAG,
        "another path is refused");
  check(t.slot[0].last_seq == 2, "a refusal does not move the counter");
  // A tampered byte of ciphertext, and of the tag.
  std::string bad = s3;
  bad[bad.size() - 40] = bad[bad.size() - 40] == 'a' ? 'b' : 'a';
  check(api_seal::unseal(t, "POST", "/api/ota/install", bad.c_str(), 2000, tok, sizeof(tok)) == api_seal::Unseal::BAD_TAG,
        "a changed ciphertext byte is refused");
  bad = s3;
  bad.back() = bad.back() == 'a' ? 'b' : 'a';
  check(api_seal::unseal(t, "POST", "/api/ota/install", bad.c_str(), 2000, tok, sizeof(tok)) == api_seal::Unseal::BAD_TAG,
        "a changed tag byte is refused");
  // The right one, then a lower counter than the last accepted.
  check(api_seal::unseal(t, "POST", "/api/ota/install", s3.c_str(), 2000, tok, sizeof(tok)) == api_seal::Unseal::OK, "seq 3");
  const std::string s2again = seal(key, SID, 2, "POST", "/api/ota/install", TOKEN);
  check(api_seal::unseal(t, "POST", "/api/ota/install", s2again.c_str(), 2000, tok, sizeof(tok)) == api_seal::Unseal::REPLAY,
        "a lower counter is a replay");
  // A gap is fine (a request the client never saw answered still counted).
  const std::string s9 = seal(key, SID, 9, "GET", "/api/status", TOKEN);
  check(api_seal::unseal(t, "GET", "/api/status", s9.c_str(), 2000, tok, sizeof(tok)) == api_seal::Unseal::OK, "seq 9 after 3");
  std::printf("PASS unseal_the_reference_headers\n");
}

void test_unseal_words() {
  api_seal::Table t = fresh();
  char tok[api_seal::TOKEN_CAP];
  uint8_t key[32];
  unhex(KEY, key);
  // An unknown session, before any handshake.
  check(api_seal::unseal(t, "POST", "/api/identify", SEALED_IDENTIFY_1, 0, tok, sizeof(tok)) == api_seal::Unseal::NO_SESSION,
        "no session");
  check_eq(api_seal::reason(api_seal::Unseal::NO_SESSION), "no_session", "its word");
  // Malformed values.
  const char* bad[] = {
      "", "c47ccac8932f3cb2", "c47ccac8932f3cb2.1", "c47ccac8932f3cb2.1.", "c47ccac8932f3cb2.0.00",
      "c47ccac8932f3cb2.01.0000000000000000000000000000000000",
      "c47ccac8932f3cb2.1.000000000000000000000000000000000",      // odd
      "c47ccac8932f3cb2.1.0000000000000000000000000000000",        // under a tag
      "c47ccac8932f3cb2.4294967296.00000000000000000000000000000000",  // past u32
      "C47CCAC8932F3CB2.1.00000000000000000000000000000000",      // uppercase sid
      "c47ccac8932f3cb2.1.0000000000000000000000000000000g",      // not hex
  };
  for (const char* b : bad) {
    check(api_seal::unseal(t, "POST", "/api/identify", b, 0, tok, sizeof(tok)) == api_seal::Unseal::BAD_FORMAT,
          "a malformed value is bad_format");
  }
  // Over the plaintext cap: TOKEN_CAP + TAG_LEN bytes of hex and two more.
  const std::string too_long = std::string(SID) + ".1." + std::string(2 * (api_seal::TOKEN_CAP + api_seal::TAG_LEN) + 2, '0');
  check(api_seal::unseal(t, "POST", "/api/identify", too_long.c_str(), 0, tok, sizeof(tok)) == api_seal::Unseal::BAD_FORMAT,
        "over the cap is bad_format");
  // Expiry: a session opened at t=1000 ms is dead TTL_MS + 1 later, and the
  // first header to name it then drops it (a later one finds no session).
  api_seal::Opened o;
  check(api_seal::open_session(t, DEVICE_ID, NONCE, CLIENT_PUB, fill_22, 1000, o) == api_seal::Open::OK, "open");
  check(api_seal::unseal(t, "POST", "/api/identify", SEALED_IDENTIFY_1, 1000 + api_seal::TTL_MS, tok, sizeof(tok)) == api_seal::Unseal::OK,
        "alive at exactly TTL");
  const std::string s2 = seal(key, SID, 2, "POST", "/api/identify", TOKEN);
  check(api_seal::unseal(t, "POST", "/api/identify", s2.c_str(), 1001 + api_seal::TTL_MS, tok, sizeof(tok)) == api_seal::Unseal::EXPIRED,
        "expired one ms past TTL");
  check(!t.slot[0].live && api_seal::all_zero(t.slot[0].key, 32), "an expired session is dropped and its key wiped");
  check(api_seal::unseal(t, "POST", "/api/identify", s2.c_str(), 1001 + api_seal::TTL_MS, tok, sizeof(tok)) == api_seal::Unseal::NO_SESSION,
        "and then unknown");
  // millis() wrap: opened near the top of the clock, still alive after it.
  check(api_seal::open_session(t, DEVICE_ID, NONCE, CLIENT_PUB, fill_22, 0xFFFFFF00u, o) == api_seal::Open::OK, "open near wrap");
  const std::string s1 = seal(key, SID, 1, "POST", "/api/identify", TOKEN);
  check(api_seal::unseal(t, "POST", "/api/identify", s1.c_str(), 0x00000100u, tok, sizeof(tok)) == api_seal::Unseal::OK,
        "alive across the wrap");
  std::printf("PASS unseal_words\n");
}

void test_table_holds_four_and_evicts_the_oldest() {
  api_seal::Table t = fresh();
  api_seal::Opened o[5];
  char nonces[5][33];
  for (int i = 0; i < 5; ++i) {
    std::snprintf(nonces[i], sizeof(nonces[i]), "%032x", 0x1000 + i);
    check(api_seal::open_session(t, DEVICE_ID, nonces[i], CLIENT_PUB, fill_counted, 1000 + i, o[i]) == api_seal::Open::OK,
          "open");
  }
  uint8_t sid0[8], sid1[8], sid4[8];
  unhex(o[0].sid_hex, sid0);
  unhex(o[1].sid_hex, sid1);
  unhex(o[4].sid_hex, sid4);
  check(api_seal::find(t, sid0) == nullptr, "the first (oldest) session was evicted by the fifth");
  check(api_seal::find(t, sid1) != nullptr, "the second still stands");
  check(api_seal::find(t, sid4) != nullptr, "the fifth stands");
  check(strcmp(o[0].sid_hex, o[1].sid_hex) != 0, "every handshake has its own sid");
  // An expired slot is reused before the oldest live one is.
  api_seal::Table u = fresh();
  for (int i = 0; i < 4; ++i) {
    check(api_seal::open_session(u, DEVICE_ID, nonces[i], CLIENT_PUB, fill_counted, i == 2 ? 0 : 500000, o[i]) == api_seal::Open::OK, "open");
  }
  uint8_t sid2[8];
  unhex(o[2].sid_hex, sid2);
  check(api_seal::open_session(u, DEVICE_ID, nonces[4], CLIENT_PUB, fill_counted, 500000 + api_seal::TTL_MS, o[4]) == api_seal::Open::OK, "a fifth");
  check(api_seal::find(u, sid2) == nullptr, "the expired slot was the one taken");
  unhex(o[0].sid_hex, sid0);
  check(api_seal::find(u, sid0) != nullptr, "the live ones stand");
  std::printf("PASS table_holds_four_and_evicts_the_oldest\n");
}

// ── the REAL gates over a sealed header ────────────────────────────────────
void test_api_auth_gates_open_a_sealed_header() {
  api_seal::g_table = fresh();
  g_auth_table = {};
  api_seal::Opened o;
  check(api_seal::open_session(api_seal::g_table, DEVICE_ID, NONCE, CLIENT_PUB, fill_22, 0, o) == api_seal::Open::OK, "open");
  uint8_t key[32];
  unhex(KEY, key);

  // A sealed token, the device's own: admitted, nothing sent.
  httpd_req_t r;
  g_method = "POST";
  g_uri = "/api/identify";
  r.req_hdrs["Authorization"] = std::string("Sealed ") + SEALED_IDENTIFY_1;
  check(api_auth_check_bearer(&r, TOKEN), "api_auth_check_bearer admits the sealed token");
  check(r.sends == 0, "and sends nothing");
  // The same header again: the seal says replay, the gate says 401 with
  // that word, and one strike is recorded, as for any wrong credential.
  httpd_req_t r2;
  r2.req_hdrs["Authorization"] = std::string("Sealed ") + SEALED_IDENTIFY_1;
  check(!api_auth_check_bearer(&r2, TOKEN), "a replayed seal is refused");
  check_eq(r2.status, "401 Unauthorized", "as 401");
  check_eq(r2.resp, "{\"error\":\"unauthorized\",\"seal\":\"replay\"}", "with the seal's word");
  check(canary::net::auth_backoff::locked(g_auth_table, auth_request_source(&r2), millis()),
        "and one strike holds the address");
  g_auth_table = {};
  // A sealed token that is NOT this device's: the seal opens, the compare
  // fails, 403 as for a wrong plain bearer.
  const std::string other = seal(key, SID, 5, "POST", "/api/identify", "cv_notThisDevicesTokenAtAll00000");
  httpd_req_t r3;
  r3.req_hdrs["Authorization"] = "Sealed " + other;
  check(!api_auth_check_bearer(&r3, TOKEN), "a sealed wrong token is refused");
  check_eq(r3.status, "403 Forbidden", "as 403");
  g_auth_table = {};
  // An unknown session: 401 no_session, so the client opens a new one.
  httpd_req_t r4;
  r4.req_hdrs["Authorization"] = "Sealed 0000000000000000.1." + std::string(70 + 32, '0');
  check(!api_auth_check_bearer(&r4, TOKEN), "an unknown session is refused");
  check_eq(r4.resp, "{\"error\":\"unauthorized\",\"seal\":\"no_session\"}", "with no_session");
  g_auth_table = {};
  // A plain bearer still works, and a wrong scheme still gets its hint.
  httpd_req_t r5;
  r5.req_hdrs["Authorization"] = std::string("Bearer ") + TOKEN;
  check(api_auth_check_bearer(&r5, TOKEN), "a plain Bearer is still admitted");
  httpd_req_t r6;
  r6.req_hdrs["Authorization"] = std::string("Basic abc");
  check(!api_auth_check_bearer(&r6, TOKEN), "another scheme is refused");
  check_eq(r6.resp, "{\"error\":\"unauthorized\",\"hint\":\"Use 'Bearer <token>' format\"}", "with the old hint");
  g_auth_table = {};
  // api_auth_bearer_valid (the raw compare under every accounted grant)
  // takes the sealed token too, and a sealed wrong token is false.
  g_method = "GET";
  g_uri = "/api/ota/status";
  // (The counter is past the seq-2 vector by now — that one is opened in
  // test_unseal_the_reference_headers — so these are sealed fresh.)
  const std::string status6 = seal(key, SID, 6, "GET", "/api/ota/status", TOKEN);
  httpd_req_t r7;
  r7.req_hdrs["Authorization"] = "Sealed " + status6;
  check(api_auth_bearer_valid(&r7, TOKEN), "api_auth_bearer_valid admits the sealed token");
  const std::string wrong = seal(key, SID, 7, "GET", "/api/ota/status", "cv_notThisDevicesTokenAtAll00000");
  httpd_req_t r8;
  r8.req_hdrs["Authorization"] = "Sealed " + wrong;
  check(!api_auth_bearer_valid(&r8, TOKEN), "a sealed wrong token is false");
  // The longest sealed value fits the gate's buffer: TOKEN_CAP bytes sealed.
  const std::string longest = seal(key, SID, 8, "GET", "/api/ota/status", std::string(api_seal::TOKEN_CAP, 'x').c_str());
  check(7 + longest.size() < API_AUTH_HEADER_CAP, "the longest Sealed header fits API_AUTH_HEADER_CAP");
  httpd_req_t r9;
  r9.req_hdrs["Authorization"] = "Sealed " + longest;
  check(!api_auth_check_bearer(&r9, TOKEN), "it opens (and is the wrong token: 403, not 401)");
  check_eq(r9.status, "403 Forbidden", "403");
  std::printf("PASS api_auth_gates_open_a_sealed_header\n");
}

// ── the sketch's text ──────────────────────────────────────────────────────
std::string slurp(const char* path) {
  std::ifstream f(path);
  check(f.good(), path);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

std::string squash(std::string s) {
  std::string out;
  bool space = false;
  for (char c : s) {
    if (c == ' ' || c == '\n' || c == '\t') {
      if (!space) out += ' ';
      space = true;
    } else {
      out += c;
      space = false;
    }
  }
  return out;
}

void test_sketch_wiring() {
  const std::string ino = squash(slurp(CANARY_WAP_INO));
  check(ino.find("#include \"seal_api.h\"") != std::string::npos, "canary_wap.ino includes seal_api.h");
  check(ino.find("{ .uri = \"/api/device/seal\", .method = HTTP_GET, .handler = seal_api::handle_seal }") != std::string::npos,
        "canary_wap.ino registers GET /api/device/seal on seal_api::handle_seal");
  const std::string api = squash(slurp(SEAL_API_H));
  check(api.find("api_seal::open_session(api_seal::g_table, device_signature::device_id(), nonce, client_pub, fill_random, now, opened)") != std::string::npos,
        "seal_api.h opens the session in the shared table with the sketch's random source");
  check(api.find("device_signature::sign_seal(nonce, client_pub, opened.device_pub_hex, sig_hex, sizeof(sig_hex))") != std::string::npos,
        "seal_api.h signs the handshake with the identity key");
  check(api.find("bool api_seal_open(httpd_req_t* req, const char* value, char* token_out, size_t token_cap, const char** reason) { const api_seal::Unseal u = api_seal::unseal(api_seal::g_table, seal_api::method_name(req->method), req->uri, value, millis(), token_out, token_cap);") != std::string::npos,
        "seal_api.h defines api_seal_open over api_seal::unseal, the request's method and URI");
  check(api.find("static void fill_random(void* buf, size_t len) { esp_fill_random(buf, len); }") != std::string::npos,
        "the random source is the hardware RNG");
  const std::string auth = squash(slurp(API_AUTH_H));
  check(auth.find("extern bool api_seal_open(httpd_req_t* req, const char* value, char* token_out, size_t token_cap, const char** reason) __attribute__((weak));") != std::string::npos,
        "api_auth.h declares the weak hook");
  size_t readers = 0;
  for (size_t at = auth.find("api_auth_presented_token(req, auth_buf, token, sizeof(token)"); at != std::string::npos;
       at = auth.find("api_auth_presented_token(req, auth_buf, token, sizeof(token)", at + 1)) {
    ++readers;
  }
  check(readers == 2, "both gates (api_auth_check_bearer, api_auth_bearer_valid) read the credential through the one reader");
  check(auth.find("if (api_seal_open && api_seal_open(req, value, token, cap, &why)) return true;") != std::string::npos,
        "the reader opens a Sealed header through the hook, when a build has one");
  std::printf("PASS sketch_wiring\n");
}

}  // namespace

int main() {
  test_canonical_and_gates();
  test_open_session_matches_the_vectors();
  test_open_session_refusals();
  test_unseal_the_reference_headers();
  test_unseal_words();
  test_table_holds_four_and_evicts_the_oldest();
  test_api_auth_gates_open_a_sealed_header();
  test_sketch_wiring();
  std::printf("\nALL api-seal (canary-wap) tests PASSED (%d checks)\n", g_checks);
  return 0;
}
