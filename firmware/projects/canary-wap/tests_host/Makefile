# Host-side unit tests for canary-wap modules that have no ESP32 dependency.
#
# These modules are written so their core logic compiles in a plain hosted
# C++ environment; the firmware build provides the ESP32 glue via Arduino.h.
#
# Run:
#   make           # build + run
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
SRC_DIR   = ../arduino/canary_wap

# $(call run_marked,<suite>[ <args>][,<marker>]) runs ./<suite> and holds it
# to the check firmware.yml's old inline steps made: a non-zero exit fails,
# and so does output without the suite's marker (default "ALL .* PASSED"), so
# a suite that returns 0 before its last check cannot pass silently. Its
# output passes through tee as the suite writes it, instead of being held
# until it exits, and its exit status comes back through a file, because
# make's /bin/sh has no pipefail. It echoes only the suite's command line,
# like the plain `./suite` lines.
run_marked = @echo "./$(1)"; t=$$(mktemp) || exit 1; \
  { ./$(1); echo $$? > "$$t.rc"; } | tee "$$t"; s=$$(cat "$$t.rc"); \
  grep -q "$(or $(2),ALL .* PASSED)" "$$t"; m=$$?; rm -f "$$t" "$$t.rc"; \
  [ "$$s" = 0 ] || { echo "$(firstword $(1)): exit $$s" >&2; exit 1; }; \
  [ $$m -eq 0 ] || \
  { echo "$(firstword $(1)): no '$(or $(2),ALL .* PASSED)' line in its output" >&2; exit 1; }

SOURCES   = test_mesh_coexistence.cpp \
            $(SRC_DIR)/mesh_channel_policy.cpp \
            $(SRC_DIR)/airtime_governor.cpp

BIN       = test_mesh_coexistence

# v0.2 (audit C16): host-side protocol-invariants test for Chirp v0.2.
# Self-contained — reimplements the canonical builders to validate byte
# layout and signer-distinguishing-canonical invariants without pulling in
# ESP-NOW / Arduino dependencies.
CHIRP_INV_BIN = test_chirp_protocol_invariants
CHIRP_INV_SRC = test_chirp_protocol_invariants.cpp

# v0.3 (audit C16 closure): per-finding regression tests for the Chirp v0.2
# security model (C2/C3 self-counting, C5 ACK dedup, C7 suppress dedup, C8
# priority storage, C9 Bloom dedup, C11 emoji size).
CHIRP_SEC_BIN = test_chirp_security
CHIRP_SEC_SRC = test_chirp_security.cpp

# v0.3: Beacon receive-path invariants. Verifies the validation pipeline in
# beacon_channel.cpp::handle_alert_frame rejects single-signature frames,
# revoked or long-unsupervised pubkeys, origin==cosigner, wrong scope, wrong
# magic, a header msg_type that disagrees with the signed canonical, a drill
# without its flag, an out-of-set template, a replayed frame (whatever its
# unsigned header nonce now says), and a frame
# outside the freshness window — and that CANCEL/UPDATE only act on the
# alarm they name, drills bank against their own rate bucket, and a replayed
# SELFTEST_OK cannot keep a dead neighbor supervised-healthy; and that the
# cosigner of a frame holds its alarm (its own fingerprint resolves to its own
# key) and so can cosign that alarm's CANCEL, checked against the real
# beacon_cancel_policy.h. Also pins that CAP-gateway trust confers no
# privilege — in the mirror, and by reading the real beacon_channel.cpp (its
# path is passed in, so the pin fails closed), which is also read to hold the
# receive path's signer resolution to the mirror's.
BEACON_ORIG_BIN = test_beacon_origination
BEACON_ORIG_SRC = test_beacon_origination.cpp

# v0.4: Beacon solo origination (BOOT-button) invariants. Mirrors §6.2 of
# spec/beacon_channel_v0.md — solo frames require BCN_FLAG_SOLO_ORIGIN,
# certainty=Observed, originator_fp==cosigner_fp, and both signature slots
# carrying the same valid signature. Also checks that the dual-pubkey
# path still rejects collapsed signers without the flag.
BEACON_SOLO_BIN = test_beacon_solo_origination
BEACON_SOLO_SRC = test_beacon_solo_origination.cpp

# Beacon CANCEL origination (beacon_cancel_policy.h): reason -> all-clear
# template, the cancel request body (malformed fields refused, not
# defaulted — beacon_api.h's adapter is read and pinned too), the canonical a
# CANCEL signs, the refusal order the REST layer
# names, the cosigner gate (sign a CANCEL only for the alarm this device
# holds), the emitted header following the signed canonical, the originator
# adopting its own frame, and the wire struct sizes. Includes the REAL
# Arduino-free beacon_wire.h + beacon_cancel_policy.h, not a mirror — and reads
# the real beacon_channel.cpp (beacon_source_scan.h; its path is passed in, so
# the pins fail closed) to pin the call sites that use those helpers.
BEACON_CANCEL_BIN = test_beacon_cancel_origination
BEACON_CANCEL_SRC = test_beacon_cancel_origination.cpp

# Beacon COSIGN envelope associated data (beacon_cosign_aad.h): the clear
# routing fields both ends bind into the ChaCha20-Poly1305 tag (layout, byte
# order, every field an attacker could flip changes the bytes, REQ/RESP
# domain separation), the all-zero X25519 shared-secret refusal, and
# secure_zero (the key wipe the optimizer cannot delete).
BEACON_AAD_BIN = test_beacon_cosign_aad
BEACON_AAD_SRC = test_beacon_cosign_aad.cpp

# v0.5: auto-revoke-on-tamper invariants for
# mesh_network.cpp::handle_tamper_alert → beacon_channel::on_peer_tampered.
# Uses libcrypto (OpenSSL) for SHA-256 since this test is host-side and
# doesn't link the firmware's mbedtls.
BEACON_TAMPER_BIN = test_beacon_tamper_revoke
BEACON_TAMPER_SRC = test_beacon_tamper_revoke.cpp

# v0.3: Opera mesh v0.2/v0.3 security invariants (O1 counter replay, O2 FE
# gating, O3 transactional rekey commit + timeout).
OPERA_SEC_BIN = test_mesh_opera_security
OPERA_SEC_SRC = test_mesh_opera_security.cpp

# device_signature has a pure-stdlib subset (canonical-string builders
# + b64url encoder) that we test on the host. The Ed25519 sign path is
# exercised separately on the HA side (Python's `cryptography` produces
# a sig over the same canonical string this test verifies the firmware
# emits byte-for-byte). The Arduino Ed25519 lib is intentionally NOT
# linked on the host — sign_chain/sign_event/sign_counts call into it
# but they're not invoked by this test; the symbol stays unresolved
# only at runtime, never at link time (we don't reference those
# functions from main()).
DEVSIG_SOURCES = test_device_signature.cpp \
                 $(SRC_DIR)/device_signature.cpp
DEVSIG_BIN     = test_device_signature

# Captive-portal DNS response builder (captive_dns.h). Pure header, no Arduino
# glue — the test includes it directly and asserts the response byte layout
# (A → redirect, AAAA/HTTPS → NODATA, malformed → drop).
CAPTIVE_DNS_BIN = test_captive_dns
CAPTIVE_DNS_SRC = test_captive_dns.cpp

# Captive-portal connectivity-probe policy (captive_probe.h). Pure header — the
# test includes it directly and asserts path classification + the per-platform
# response descriptor (Apple HTML page, Android 204, Windows NCSI body), the
# same logic handle_captive_probe() serves in the firmware.
CAPTIVE_PROBE_BIN = test_captive_probe
CAPTIVE_PROBE_SRC = test_captive_probe.cpp

# Privacy (F-03): salted, non-reversible device pseudonym that replaces the raw
# MAC in operator-facing diagnostics. The sketch's device_pseudonym.h is now a
# byte-identical staged copy of the canonical firmware/common/identity header
# (header-only, no .cpp), so this just compiles the header via the sketch path.
# Uses libcrypto (OpenSSL) for SHA-256 — the firmware compiles the same derive()
# with mbedtls.
DEVPSEUDO_SOURCES = test_device_pseudonym.cpp
DEVPSEUDO_BIN     = test_device_pseudonym

# Privacy (F-03): the same canonical device_pseudonym header (firmware/common/
# identity) via its real path — also adopted by the canary-vision tree. Pins the
# byte construction independently of the sketch copy. Links libcrypto for host SHA-256.
DEVPSEUDO_COMMON_BIN = test_device_pseudonym_common
DEVPSEUDO_COMMON_SRC = test_device_pseudonym_common.cpp

# Privacy (F-03): shared gps_coarsen_deg() header (firmware/common/gnss). Pure
# header, no Arduino glue and no crypto — asserts the exact operator-facing rounding.
GPS_COARSEN_BIN = test_gps_coarsen
GPS_COARSEN_SRC = test_gps_coarsen.cpp

# GPS-derived system clock: NMEA UTC date/time -> validated Unix epoch
# (firmware/common/gnss/gnss_time.h). Pure header, no Arduino glue — asserts
# known epoch values and the calendar-validity guardrails that keep a
# corrupt-but-checksum-valid sentence from reaching settimeofday().
GNSS_TIME_BIN = test_gnss_time
GNSS_TIME_SRC = test_gnss_time.cpp

# Acoustic pipeline: compiles the REAL vendored securacv_audio.cpp against
# stub Arduino/ESP-IDF headers (stubs/audio/) and drives synthetic alarm
# cadences through a scripted i2s_read. Covers T3 detection, knock, the
# deferred hard mute, and threshold validation.
AUDIO_BIN     = test_audio_cadence
AUDIO_SOURCES = test_audio_cadence.cpp \
                $(SRC_DIR)/securacv_audio.cpp

# CSI feature extractor: compiles the REAL staged csi_features.cpp against
# a stub Arduino.h (stubs/csi/) and feeds synthetic frames with known
# physics — static channel + random CFO must read empty, motion must read
# as motion, a 0.25 Hz breathing envelope must land in the right bin.
CSI_FEAT_BIN     = test_csi_features
CSI_FEAT_SOURCES = test_csi_features.cpp \
                   $(SRC_DIR)/csi_features.cpp

# wifi.channel_activity: ambient, unattributed channel-activity burst detector.
# Compiles the REAL staged module against stub chokepoint/settings symbols and
# pins the privacy contract — emits only CSI_CATEGORY_AMBIENT (never persisted),
# a coarse P0 allow-list (no identity field), and a fixed literal + bounded
# intensity regardless of input. See spec/canary_free_signals_v0.md A/E/F.
WCA_BIN     = test_wifi_channel_activity_privacy
WCA_SOURCES = test_wifi_channel_activity_privacy.cpp \
              $(SRC_DIR)/wifi_channel_activity.cpp

.PHONY: all run clean
all: run


# Fleet-link presence beacon wire format (fleet_beacon.h). Pure header, no
# NimBLE/Arduino glue — the test includes it directly and round-trips the
# 9-byte manufacturer payload (type/version/flags, 0xFF-unknown battery/health,
# little-endian chain_lo, fp bytes) plus the full-blob parser. The display side
# parses the identical layout (canary-display/include/canary/net/beacon_parse.h).
FLEET_BEACON_BIN = test_fleet_beacon
FLEET_BEACON_SRC = test_fleet_beacon.cpp


# USB evidence drive core (share-mode state machine + FAT16 staging volume).
# See docs/design/usb_evidence_drive.md.
EVDRIVE_BIN = test_evidence_drive_logic
EVDRIVE_SOURCES = test_evidence_drive_logic.cpp \
                  $(SRC_DIR)/evidence_drive_logic.cpp


# Drop-file update verification, end-to-end with real Ed25519 (OpenSSL).
# Links the OTA engine's pure subset (SECURACV_OTA_HOST_BUILD).
EVVERIFY_BIN = test_evidence_update_verify
EVVERIFY_SOURCES = test_evidence_update_verify.cpp \
                   $(SRC_DIR)/evidence_update_verify.cpp \
                   $(SRC_DIR)/securacv_ota.cpp

# BLE OTA protocol-v2 header policy (ble_ota_policy.h): the 168-byte
# header's layout and strict parse, the domain-separated canonical message
# pinned against the fixture shared with firmware/scripts/test_ota_release.py,
# and the accept / refuse / break-glass decision matrix — product binding,
# the anti-rollback floor shared with the pull engine, the legacy v1 header
# admitted only when armed — with real Ed25519 (OpenSSL). Links the OTA
# engine's pure subset for securacv_ota_update_decision().
BLEOTA_BIN     = test_ble_ota_policy
BLEOTA_SOURCES = test_ble_ota_policy.cpp \
                 $(SRC_DIR)/securacv_ota.cpp

# GET /api/v1/witness page (witness_page.h, spec/witness_api_v1.md): the
# ?last=N parser, the RAM ring, coarse ten-minute anchoring, and a byte-exact
# render of the shared fixture spec/fixtures/witness_page_v1.json rebuilt
# from real Ed25519-signed records (OpenSSL) — the same fixture the iPhone
# app's XCTest decodes. Links libcrypto.
WITNESS_PAGE_BIN = test_witness_page
WITNESS_PAGE_SRC = test_witness_page.cpp
WITNESS_PAGE_FIXTURE = ../../../../spec/fixtures/witness_page_v1.json

$(BIN): $(SOURCES) $(SRC_DIR)/airtime_governor.h $(SRC_DIR)/mesh_network.cpp beacon_source_scan.h
	$(CXX) $(CXXFLAGS) -DMESH_NETWORK_CPP='"$(abspath $(SRC_DIR))/mesh_network.cpp"' -o $@ $(SOURCES)

$(CHIRP_INV_BIN): $(CHIRP_INV_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(CHIRP_INV_SRC)

$(CHIRP_SEC_BIN): $(CHIRP_SEC_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(CHIRP_SEC_SRC)

$(BEACON_ORIG_BIN): $(BEACON_ORIG_SRC) beacon_source_scan.h $(SRC_DIR)/beacon_wire.h $(SRC_DIR)/beacon_cancel_policy.h
	$(CXX) $(CXXFLAGS) -I $(SRC_DIR) -DBEACON_CHANNEL_CPP='"$(abspath $(SRC_DIR))/beacon_channel.cpp"' -o $@ $(BEACON_ORIG_SRC)

$(BEACON_SOLO_BIN): $(BEACON_SOLO_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(BEACON_SOLO_SRC)

$(BEACON_CANCEL_BIN): $(BEACON_CANCEL_SRC) $(SRC_DIR)/beacon_wire.h $(SRC_DIR)/beacon_cancel_policy.h beacon_source_scan.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -DBEACON_CHANNEL_CPP='"$(abspath $(SRC_DIR))/beacon_channel.cpp"' -DBEACON_API_H='"$(abspath $(SRC_DIR))/beacon_api.h"' -o $@ $(BEACON_CANCEL_SRC)

$(BEACON_AAD_BIN): $(BEACON_AAD_SRC) $(SRC_DIR)/beacon_wire.h $(SRC_DIR)/beacon_cosign_aad.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(BEACON_AAD_SRC)

# The two suites above join `run` and `clean` by prerequisite, so they add no
# hunk to those shared lists (other branches extend the same lines).
.PHONY: run-beacon-cancel clean-beacon-cancel
run-beacon-cancel: $(BEACON_CANCEL_BIN) $(BEACON_AAD_BIN)
	./$(BEACON_CANCEL_BIN)
	./$(BEACON_AAD_BIN)
clean-beacon-cancel:
	rm -f $(BEACON_CANCEL_BIN) $(BEACON_AAD_BIN)
run: run-beacon-cancel
clean: clean-beacon-cancel

# Full-body HTTP reads (http_body_reader.h): a body split across TCP reads is
# reassembled, an oversized one is refused before any read (never truncated),
# timeouts are bounded, nothing past content_len is read — and a source pin
# that every Beacon route in beacon_api.h reads its body through read_body().
HTTP_BODY_BIN = test_http_body_reader
HTTP_BODY_SRC = test_http_body_reader.cpp

$(HTTP_BODY_BIN): $(HTTP_BODY_SRC) $(SRC_DIR)/http_body_reader.h $(SRC_DIR)/beacon_api.h beacon_source_scan.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -DBEACON_API_H='"$(abspath $(SRC_DIR))/beacon_api.h"' -o $@ $(HTTP_BODY_SRC)

.PHONY: run-http-body clean-http-body
run-http-body: $(HTTP_BODY_BIN)
	./$(HTTP_BODY_BIN)
clean-http-body:
	rm -f $(HTTP_BODY_BIN)
run: run-http-body
clean: clean-http-body

$(BEACON_TAMPER_BIN): $(BEACON_TAMPER_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(BEACON_TAMPER_SRC) -lcrypto

$(OPERA_SEC_BIN): $(OPERA_SEC_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(OPERA_SEC_SRC)

$(DEVSIG_BIN): $(DEVSIG_SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(DEVSIG_SOURCES)

$(CAPTIVE_DNS_BIN): $(CAPTIVE_DNS_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(CAPTIVE_DNS_SRC)

$(CAPTIVE_PROBE_BIN): $(CAPTIVE_PROBE_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(CAPTIVE_PROBE_SRC)

$(DEVPSEUDO_BIN): $(DEVPSEUDO_SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(DEVPSEUDO_SOURCES) -lcrypto

$(DEVPSEUDO_COMMON_BIN): $(DEVPSEUDO_COMMON_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(DEVPSEUDO_COMMON_SRC) -lcrypto

$(GPS_COARSEN_BIN): $(GPS_COARSEN_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(GPS_COARSEN_SRC)

$(GNSS_TIME_BIN): $(GNSS_TIME_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(GNSS_TIME_SRC)

$(AUDIO_BIN): $(AUDIO_SOURCES)
	$(CXX) $(CXXFLAGS) -Werror -I stubs/audio -I $(SRC_DIR) -o $@ $(AUDIO_SOURCES)

$(CSI_FEAT_BIN): $(CSI_FEAT_SOURCES)
	$(CXX) $(CXXFLAGS) -I stubs/csi -I $(SRC_DIR) -o $@ $(CSI_FEAT_SOURCES)

$(WCA_BIN): $(WCA_SOURCES)
	$(CXX) $(CXXFLAGS) -I $(SRC_DIR) -o $@ $(WCA_SOURCES)

$(FLEET_BEACON_BIN): $(FLEET_BEACON_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(FLEET_BEACON_SRC)

$(EVDRIVE_BIN): $(EVDRIVE_SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(EVDRIVE_SOURCES)

$(EVVERIFY_BIN): $(EVVERIFY_SOURCES)
	$(CXX) $(CXXFLAGS) -DSECURACV_OTA_HOST_BUILD -I $(SRC_DIR) -o $@ $(EVVERIFY_SOURCES) -lcrypto

$(BLEOTA_BIN): $(BLEOTA_SOURCES)
	$(CXX) $(CXXFLAGS) -Werror -Wno-deprecated-declarations -DSECURACV_OTA_HOST_BUILD -I $(SRC_DIR) -o $@ $(BLEOTA_SOURCES) -lcrypto

$(WITNESS_PAGE_BIN): $(WITNESS_PAGE_SRC) $(SRC_DIR)/witness_page.h
	$(CXX) $(CXXFLAGS) -Werror -Wno-deprecated-declarations -I $(SRC_DIR) -o $@ $(WITNESS_PAGE_SRC) -lcrypto

# Help QR: the verdict → Help Desk URL composer (help_qr_logic.h) — the
# probe-namespace bridge, worst-first precedence, and overflow refusal.
HELPQR_BIN = test_help_qr_logic
HELPQR_SRC = test_help_qr_logic.cpp

$(HELPQR_BIN): $(HELPQR_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(HELPQR_SRC)

# Status tier: the dashboard's Good / Needs attention / Action required strip
# (status_tier_logic.h) — worst-first ranking, one reason code per tier, a
# missing card is not a fault, and every code has a COPY.tier row.
STATUSTIER_BIN = test_status_tier_logic
STATUSTIER_SRC = test_status_tier_logic.cpp

$(STATUSTIER_BIN): $(STATUSTIER_SRC) $(SRC_DIR)/status_tier_logic.h
	$(CXX) $(CXXFLAGS) -o $@ $(STATUSTIER_SRC)

# CSI L-LTF tone canonicalization (csi_subcarriers.h): the 52-tone map for
# FFT-ordered 20 MHz and linear 40 MHz L-LTF sections, HT/non-HT parity,
# first_word_invalid patching, and the zero-based order detector.
CSI_SUBC_BIN = test_csi_subcarriers
CSI_SUBC_SRC = test_csi_subcarriers.cpp

$(CSI_SUBC_BIN): $(CSI_SUBC_SRC)
	$(CXX) $(CXXFLAGS) -I $(SRC_DIR) -o $@ $(CSI_SUBC_SRC)

# CSI ESP-IDF config shim (csi_idf_compat.h): one source, four struct shapes
# (legacy IDF 4.4 / 5.x, HE IDF 5.1 / 5.5). A member rename upstream fails
# here, not on a customer's C6.
CSI_COMPAT_SRC  = test_csi_idf_compat.cpp
CSI_COMPAT_BINS = test_csi_idf_compat_legacy44 test_csi_idf_compat_legacy5x \
                  test_csi_idf_compat_he51 test_csi_idf_compat_he55

test_csi_idf_compat_legacy44: $(CSI_COMPAT_SRC)
	$(CXX) $(CXXFLAGS) -DCSI_TEST_SHAPE_LEGACY44 -I $(SRC_DIR) -o $@ $(CSI_COMPAT_SRC)
test_csi_idf_compat_legacy5x: $(CSI_COMPAT_SRC)
	$(CXX) $(CXXFLAGS) -DCSI_TEST_SHAPE_LEGACY5X -I $(SRC_DIR) -o $@ $(CSI_COMPAT_SRC)
test_csi_idf_compat_he51: $(CSI_COMPAT_SRC)
	$(CXX) $(CXXFLAGS) -DCSI_TEST_SHAPE_HE51 -I $(SRC_DIR) -o $@ $(CSI_COMPAT_SRC)
test_csi_idf_compat_he55: $(CSI_COMPAT_SRC)
	$(CXX) $(CXXFLAGS) -DCSI_TEST_SHAPE_HE55 -I $(SRC_DIR) -o $@ $(CSI_COMPAT_SRC)

# CSI router-echo frame supply (csi_traffic): start/stop policy through the
# host hooks — delay, retry, link/gateway changes, the enable gate.
CSI_TRAFFIC_BIN     = test_csi_traffic
CSI_TRAFFIC_SOURCES = test_csi_traffic.cpp \
                      $(SRC_DIR)/csi_traffic.cpp

$(CSI_TRAFFIC_BIN): $(CSI_TRAFFIC_SOURCES)
	$(CXX) $(CXXFLAGS) -DCSI_TEST_HOST_BUILD -I $(SRC_DIR) -o $@ $(CSI_TRAFFIC_SOURCES)

# system.integrity tamper watcher (tamper_events_module): the transition
# rules through the REAL chokepoint + bundler — clean boot silent, reset
# precedence (watchdog > brownout > panic), one boot story, SD adoption vs
# removal/error, recovery-not-a-tamper, pre-registration retry. Compiles the
# sketch's staged CSI core copies (check_csi_sync.sh pins them byte-identical
# to firmware/common/csi/src).
TAMPER_BIN     = test_tamper_events_logic
TAMPER_SOURCES = test_tamper_events_logic.cpp \
                 $(SRC_DIR)/csi_event.cpp \
                 $(SRC_DIR)/csi_module.cpp \
                 $(SRC_DIR)/csi_bundler.cpp \
                 $(SRC_DIR)/tamper_events_module.cpp

$(TAMPER_BIN): $(TAMPER_SOURCES)
	$(CXX) $(CXXFLAGS) -DCSI_TEST_HOST_BUILD -Wno-unused-parameter -I $(SRC_DIR) -o $@ $(TAMPER_SOURCES)

run: $(BIN) $(CHIRP_INV_BIN) $(CHIRP_SEC_BIN) $(BEACON_ORIG_BIN) $(BEACON_SOLO_BIN) $(BEACON_TAMPER_BIN) $(OPERA_SEC_BIN) $(DEVSIG_BIN) $(CAPTIVE_DNS_BIN) $(CAPTIVE_PROBE_BIN) $(DEVPSEUDO_BIN) $(DEVPSEUDO_COMMON_BIN) $(GPS_COARSEN_BIN) $(GNSS_TIME_BIN) $(AUDIO_BIN) $(CSI_FEAT_BIN) $(WCA_BIN) $(FLEET_BEACON_BIN) $(EVDRIVE_BIN) $(EVVERIFY_BIN) $(BLEOTA_BIN) $(HELPQR_BIN) $(STATUSTIER_BIN) $(CSI_SUBC_BIN) $(CSI_COMPAT_BINS) $(CSI_TRAFFIC_BIN) $(TAMPER_BIN) $(WITNESS_PAGE_BIN)
	./$(BIN)
	./$(CHIRP_INV_BIN)
	./$(CHIRP_SEC_BIN)
	./$(BEACON_ORIG_BIN)
	./$(BEACON_SOLO_BIN)
	./$(BEACON_TAMPER_BIN)
	./$(OPERA_SEC_BIN)
	./$(DEVSIG_BIN)
	./$(CAPTIVE_DNS_BIN)
	./$(CAPTIVE_PROBE_BIN)
	./$(DEVPSEUDO_BIN)
	./$(DEVPSEUDO_COMMON_BIN)
	./$(GPS_COARSEN_BIN)
	./$(GNSS_TIME_BIN)
	$(call run_marked,$(AUDIO_BIN),test_audio_cadence: all tests passed)
	./$(CSI_FEAT_BIN)
	./$(WCA_BIN)
	./$(FLEET_BEACON_BIN)
	./$(EVDRIVE_BIN)
	./$(EVVERIFY_BIN)
	$(call run_marked,$(BLEOTA_BIN))
	./$(HELPQR_BIN)
	./$(STATUSTIER_BIN)
	./$(CSI_SUBC_BIN)
	for b in $(CSI_COMPAT_BINS); do ./$$b || exit 1; done
	./$(CSI_TRAFFIC_BIN)
	./$(TAMPER_BIN)
	$(call run_marked,$(WITNESS_PAGE_BIN) $(WITNESS_PAGE_FIXTURE))

clean:
	rm -f $(WITNESS_PAGE_BIN) $(EVVERIFY_BIN) $(BLEOTA_BIN) $(EVDRIVE_BIN) $(FLEET_BEACON_BIN) $(BIN) $(CHIRP_INV_BIN) $(CHIRP_SEC_BIN) $(BEACON_ORIG_BIN) $(BEACON_SOLO_BIN) $(BEACON_TAMPER_BIN) $(OPERA_SEC_BIN) $(DEVSIG_BIN) $(CAPTIVE_DNS_BIN) $(CAPTIVE_PROBE_BIN) $(DEVPSEUDO_BIN) $(DEVPSEUDO_COMMON_BIN) $(GPS_COARSEN_BIN) $(GNSS_TIME_BIN) $(AUDIO_BIN) $(CSI_FEAT_BIN) $(WCA_BIN) $(HELPQR_BIN) $(STATUSTIER_BIN) $(CSI_SUBC_BIN) $(CSI_COMPAT_BINS) $(CSI_TRAFFIC_BIN) $(TAMPER_BIN)

# F14: canary-wap Opera pairing framing (mesh_pair_frame.h). Pairing frames
# are [MessageType 8..12][exact Pair*Payload] and must be classified before
# handle_received_message()'s 102-byte signed-header minimum, which used to
# drop all of them. Pins exact-size accept, off-by-one reject, and that a
# signed Opera (version 0), Chirp (0xC4) or Beacon (0xB1) frame is never
# taken for a pairing frame. Pure header, no Arduino glue. Hooked into
# `run` and `clean` by prerequisite so it adds no hunk to their lists.
PAIR_FRAME_BIN = test_mesh_pair_frame
PAIR_FRAME_SRC = test_mesh_pair_frame.cpp

$(PAIR_FRAME_BIN): $(PAIR_FRAME_SRC) $(SRC_DIR)/mesh_pair_frame.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(PAIR_FRAME_SRC)

.PHONY: run-pair-frame clean-pair-frame
run-pair-frame: $(PAIR_FRAME_BIN)
	./$(PAIR_FRAME_BIN)
clean-pair-frame:
	rm -f $(PAIR_FRAME_BIN)

run: run-pair-frame
clean: clean-pair-frame

# F33 part 2 (crypto review — maintainer to confirm): canary-wap Opera pairing
# key agreement (mesh_pair_crypto.h). The header compiles here as the sketch
# compiles it, against REAL X25519 / HKDF-SHA256 / SHA-256 (OpenSSL, behind the
# rweather and mbedTLS names in stubs/pair_crypto) — no mock of the key
# exchange: two independently generated keypairs must reach the same session
# key and 6-digit code, and the pre-fix Ed25519 keys must not. Also reads the
# real mesh_network.cpp (beacon_source_scan.h; its path is passed in, so the
# pins fail closed) to pin that the pairing handlers use the header.
PAIR_CRYPTO_BIN = test_mesh_pair_crypto
PAIR_CRYPTO_SRC = test_mesh_pair_crypto.cpp

$(PAIR_CRYPTO_BIN): $(PAIR_CRYPTO_SRC) $(SRC_DIR)/mesh_pair_crypto.h $(SRC_DIR)/mesh_network.cpp beacon_source_scan.h stubs/pair_crypto/Curve25519.h stubs/pair_crypto/mbedtls/md.h stubs/pair_crypto/mbedtls/hkdf.h
	$(CXX) $(CXXFLAGS) -Werror -I stubs/pair_crypto -I $(SRC_DIR) -DMESH_NETWORK_CPP='"$(abspath $(SRC_DIR))/mesh_network.cpp"' -o $@ $(PAIR_CRYPTO_SRC) -lcrypto

.PHONY: run-pair-crypto clean-pair-crypto
run-pair-crypto: $(PAIR_CRYPTO_BIN)
	./$(PAIR_CRYPTO_BIN)
clean-pair-crypto:
	rm -f $(PAIR_CRYPTO_BIN)

run: run-pair-crypto
clean: clean-pair-crypto

# F33 part 6: the spec §5.6 REVOCATION_GRACE_MS deny-list. mesh_revocation is
# the PlatformIO tree's module, staged byte-identical (check_mesh_sync.sh);
# its behavior suite runs here against the STAGED copy, and
# test_mesh_revocation_wap pins what mesh_network.cpp does with it (records on
# remove, refuses in the pairing handlers and add_peer, restores / expires /
# persists, FE-gated) by reading the real file (beacon_source_scan.h).
REVOC_WAP_BIN    = test_mesh_revocation_wap
REVOC_WAP_SRC    = test_mesh_revocation_wap.cpp
REVOC_STAGED_BIN = test_mesh_revocation_staged
REVOC_STAGED_SRC = ../../../canary/lib/securacv_mesh/test_mesh_revocation.cpp

$(REVOC_WAP_BIN): $(REVOC_WAP_SRC) $(SRC_DIR)/mesh_revocation.h $(SRC_DIR)/mesh_revocation.cpp $(SRC_DIR)/mesh_network.cpp beacon_source_scan.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -DMESH_NETWORK_CPP='"$(abspath $(SRC_DIR))/mesh_network.cpp"' -o $@ $(REVOC_WAP_SRC) $(SRC_DIR)/mesh_revocation.cpp

$(REVOC_STAGED_BIN): $(REVOC_STAGED_SRC) $(SRC_DIR)/mesh_revocation.h $(SRC_DIR)/mesh_revocation.cpp
	$(CXX) $(CXXFLAGS) -Werror -DCSI_TEST_HOST_BUILD -I $(SRC_DIR) -o $@ $(REVOC_STAGED_SRC) $(SRC_DIR)/mesh_revocation.cpp

.PHONY: run-revocation clean-revocation
run-revocation: $(REVOC_WAP_BIN) $(REVOC_STAGED_BIN)
	./$(REVOC_WAP_BIN)
	./$(REVOC_STAGED_BIN)
clean-revocation:
	rm -f $(REVOC_WAP_BIN) $(REVOC_STAGED_BIN)

run: run-revocation
clean: clean-revocation

# The CSI active probe under the airtime governor (the follow-up to sweep F4,
# #1696), end to end — the REAL csi_probe scheduler (the sketch's staged copy,
# CSI_TEST_HOST_BUILD), the REAL airtime_governor and the gate body the WAP
# installs (probe_airtime.h), against an independent send ledger: the window
# never reads below the ledger, the probe stops at its 1.60 % ceiling and the
# mesh heartbeat keeps its room. Also reads the real csi_integration.cpp
# (beacon_source_scan.h; its path is passed in, so the pin fails closed) to
# pin that probe_pump installs exactly probe_airtime::reserve_probe_frame.
PROBE_AIR_BIN     = test_csi_probe_airtime
PROBE_AIR_SOURCES = test_csi_probe_airtime.cpp \
                    $(SRC_DIR)/csi_probe.cpp \
                    $(SRC_DIR)/airtime_governor.cpp

$(PROBE_AIR_BIN): $(PROBE_AIR_SOURCES) $(SRC_DIR)/probe_airtime.h $(SRC_DIR)/csi_probe.h $(SRC_DIR)/airtime_governor.h $(SRC_DIR)/csi_integration.cpp beacon_source_scan.h
	$(CXX) $(CXXFLAGS) -Werror -DCSI_TEST_HOST_BUILD -I $(SRC_DIR) -DCSI_INTEGRATION_CPP='"$(abspath $(SRC_DIR))/csi_integration.cpp"' -o $@ $(PROBE_AIR_SOURCES)

.PHONY: run-probe-airtime clean-probe-airtime
run-probe-airtime: $(PROBE_AIR_BIN)
	./$(PROBE_AIR_BIN)
clean-probe-airtime:
	rm -f $(PROBE_AIR_BIN)

run: run-probe-airtime
clean: clean-probe-airtime

# Roadmap §5 step 4, host half: the second CSI feature extractor —
# amplitude-centroid wander → v[28], frame-to-frame jitter → v[29] — behind
# CSI_WANDER_JITTER, which is 0 in every shipped build.
# test_csi_wander_jitter pins the header's exact math and edges (pure
# header, no Arduino glue). test_csi_features_wj recompiles the REAL staged
# csi_features.cpp and the physics suite with the flag ON, where the
# synthetic-channel pins live; test_csi_features above stays the flag-off
# build and asserts v[28..31] read zero. Hooked into `run` and `clean` by
# prerequisite so it adds no hunk to their lists.
CSI_WJ_BIN      = test_csi_wander_jitter
CSI_WJ_SRC      = test_csi_wander_jitter.cpp
CSI_FEAT_WJ_BIN = test_csi_features_wj

$(CSI_WJ_BIN): $(CSI_WJ_SRC) $(SRC_DIR)/csi_wander_jitter.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(CSI_WJ_SRC)

$(CSI_FEAT_WJ_BIN): $(CSI_FEAT_SOURCES) $(SRC_DIR)/csi_wander_jitter.h
	$(CXX) $(CXXFLAGS) -Werror -DCSI_WANDER_JITTER=1 -I stubs/csi -I $(SRC_DIR) -o $@ $(CSI_FEAT_SOURCES)

# The flag-off build includes the header too (isqrt_u32 lives there).
$(CSI_FEAT_BIN): $(SRC_DIR)/csi_wander_jitter.h

.PHONY: run-wander-jitter clean-wander-jitter
run-wander-jitter: $(CSI_WJ_BIN) $(CSI_FEAT_WJ_BIN)
	./$(CSI_WJ_BIN)
	./$(CSI_FEAT_WJ_BIN)
clean-wander-jitter:
	rm -f $(CSI_WJ_BIN) $(CSI_FEAT_WJ_BIN)

run: run-wander-jitter
clean: clean-wander-jitter

# Sweep CI3, one host-test list: the fourteen suites below were built only by
# their own inline g++ steps in firmware.yml's "Mesh + Scout Host Tests" job,
# so `make` here never ran them and the two lists could drift. Each rule keeps
# its step's flags (-Werror; the witness store's OpenSSL deprecation waiver
# and -lcrypto) and its "ALL ... PASSED" check (run_marked, above).
# scripts/tests/test_host_test_lists.py fails a suite that is back on a
# workflow's list only. Pure sketch headers, no Arduino glue. Hooked into
# `run` and `clean` by prerequisite so it adds no hunk to their lists.

# The first-run wizard's pure decisions (setup-window keep-alive, scan-cache
# freshness, AP-only join suppression, AP teardown grace), so the
# phone-visible provisioning behavior is pinned on the host.
PROV_BIN = test_provisioning_logic
PROV_SRC = test_provisioning_logic.cpp

# The pre-flight health check's verdicts (Bluetooth SKIP vs FAIL vs ABSENT
# across boot window / safe mode / DEV builds, Wi-Fi link-down vs radio-off),
# so the "almost everything failing" false negatives can't come back.
SELFTEST_BIN = test_selftest_logic
SELFTEST_SRC = test_selftest_logic.cpp

# The Device-tab runtime config clamps. Most important: the event-time
# coarsening bucket can only be widened past its compile-time floor, never
# narrowed (Invariant III), and the log threshold can never rise above
# WARNING (ERROR/CRITICAL always stored).
CONFIG_BIN = test_config_logic
CONFIG_SRC = test_config_logic.cpp

# Which failed-auth shapes feed the brute-force lockout. A credential-less
# request must NOT count: counting it let one open dashboard tab 429-lock the
# operator's correct token.
AUTH_BIN = test_auth_logic
AUTH_SRC = test_auth_logic.cpp

# The BLE pairing channel ships ENABLED by default (disabled read as a dead
# Bluetooth panel). Pins that the radio is on out of the box AND that PIN
# confirmation stays required, so "on by default" never becomes "pairs
# silently".
BTDEF_BIN = test_bt_defaults
BTDEF_SRC = test_bt_defaults.cpp

# The SD mount worker's decisions: the loop's periodic SD check never attempts
# a mount in SAFE MODE (the old unconditional remount re-ran a blocking
# SD.begin from loop() and crash-looped safe mode via the 8 s task watchdog),
# never piles onto an in-flight attempt, and the caller's bounded wait is
# wrap-safe.
SDMOUNT_BIN = test_sd_mount_logic
SDMOUNT_SRC = test_sd_mount_logic.cpp

# The camera preview's reported numbers: stream uptime freezes at the recorded
# end instead of collapsing to 0 (which zeroed the derived throughput), avg
# kbps comes from real byte totals with no divide-by-zero, and the MJPEG
# worker's frame pacing clamps to a >=20 ms floor (the yield that keeps the
# WDT-subscribed IDLE tasks fed under the priority-3 stream task).
PEEK_BIN = test_peek_stream_logic
PEEK_SRC = test_peek_stream_logic.cpp

# Sealed-snapshot vault decisions and on-disk format (vault_logic.h): the
# fail-closed capture decision table (no key / no SD / no camera / QR busy /
# worker busy / cooldown, incl. millis wrap), the byte-exact 64-byte .svlt
# header against a golden hex fixture shared verbatim with
# tools/test_unseal_snapshot.py, and the ring filename parser that doubles as
# the download/delete traversal gate.
VAULT_BIN = test_vault_logic
VAULT_SRC = test_vault_logic.cpp

# The /WITNESS/records.jsonl durable tier (witness_store.h, the sketch's
# staged copy; firmware/tests_host builds the same suite against the
# canonical header): the byte-exact NDJSON line against a golden fixture,
# tail recovery (newest complete line wins; torn power-cut lines skipped; junk
# lines can't shadow good ones), malformed-field and seq-overflow rejection,
# the SD-wins reconciliation decision, and the seq-binding tamper check
# (OpenSSL recompute of the chain hash; SHA256_Init & co. are deprecated in
# OpenSSL 3, hence the waiver under -Werror).
WITSTORE_BIN = test_witness_store_logic
WITSTORE_SRC = test_witness_store_logic.cpp

# The beacon-audit chain-head SD recovery guard (beacon_audit_recover.h): only
# adopt the newest COMPLETE line's head when the tail chain-links (each
# entry's prev == the previous entry's head, or genesis for a first record).
# Torn tails, corrupt or field-less lines, and unverifiable single fragments
# are refused, so a stale-NVS boot cannot silently redirect the append-only
# chain.
BCNAUDIT_BIN = test_beacon_audit_recover
BCNAUDIT_SRC = test_beacon_audit_recover.cpp

# Camera power management decisions (camera_gate_logic.h): the peek-surface
# gate precedence (broken camera > thermal > battery policy, each with honest
# user-facing copy) and the idle-standby decision (never while
# streaming/QR/sealing; immediate on policy deny; wrap-safe idle timer
# otherwise).
CAMGATE_BIN = test_camera_gate_logic
CAMGATE_SRC = test_camera_gate_logic.cpp

# The /HEALTH per-boot jsonl format (health_store_logic.h): JSON escaping of
# peer-influenced strings (quote/backslash/control bytes, atomic truncation),
# the byte-exact line format, and the boot filename scheme.
HEALTH_BIN = test_health_store_logic
HEALTH_SRC = test_health_store_logic.cpp

# Round-two power-policy gates (power_gate_logic.h): advisory feature run/skip
# (CSI drain) and routine MQTT heartbeat cadence stretch by power mode
# (life-safety and event publishes bypass; overflow saturates rather than
# wraps).
PWRGATE_BIN = test_power_gate_logic
PWRGATE_SRC = test_power_gate_logic.cpp

# Who owns the shared canary.local catch-all when several Canaries share a
# LAN: the fingerprint-staggered claim window, the detect-own-echo probe
# classification, and the IP tie-break whose antisymmetry guarantees exactly
# one device keeps a double claim (both keeping = session-cookie-killing
# hostname flapping; both withdrawing = canary.local goes dark).
CATCHALL_BIN = test_catchall_logic
CATCHALL_SRC = test_catchall_logic.cpp

SKETCH_LOGIC_BINS = $(PROV_BIN) $(SELFTEST_BIN) $(CONFIG_BIN) $(AUTH_BIN) \
                    $(BTDEF_BIN) $(SDMOUNT_BIN) $(PEEK_BIN) $(VAULT_BIN) \
                    $(WITSTORE_BIN) $(BCNAUDIT_BIN) $(CAMGATE_BIN) \
                    $(HEALTH_BIN) $(PWRGATE_BIN) $(CATCHALL_BIN)

$(PROV_BIN): $(PROV_SRC) $(SRC_DIR)/provisioning_logic.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(PROV_SRC)

$(SELFTEST_BIN): $(SELFTEST_SRC) $(SRC_DIR)/selftest_logic.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(SELFTEST_SRC)

$(CONFIG_BIN): $(CONFIG_SRC) $(SRC_DIR)/config_logic.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(CONFIG_SRC)

$(AUTH_BIN): $(AUTH_SRC) $(SRC_DIR)/auth_logic.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(AUTH_SRC)

$(BTDEF_BIN): $(BTDEF_SRC) $(SRC_DIR)/bt_defaults.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(BTDEF_SRC)

$(SDMOUNT_BIN): $(SDMOUNT_SRC) $(SRC_DIR)/sd_mount_logic.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(SDMOUNT_SRC)

$(PEEK_BIN): $(PEEK_SRC) $(SRC_DIR)/peek_stream_logic.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(PEEK_SRC)

$(VAULT_BIN): $(VAULT_SRC) $(SRC_DIR)/vault_logic.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(VAULT_SRC)

$(WITSTORE_BIN): $(WITSTORE_SRC) $(SRC_DIR)/witness_store.h
	$(CXX) $(CXXFLAGS) -Werror -Wno-deprecated-declarations -I $(SRC_DIR) -o $@ $(WITSTORE_SRC) -lcrypto

$(BCNAUDIT_BIN): $(BCNAUDIT_SRC) $(SRC_DIR)/beacon_audit_recover.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(BCNAUDIT_SRC)

$(CAMGATE_BIN): $(CAMGATE_SRC) $(SRC_DIR)/camera_gate_logic.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(CAMGATE_SRC)

$(HEALTH_BIN): $(HEALTH_SRC) $(SRC_DIR)/health_store_logic.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(HEALTH_SRC)

$(PWRGATE_BIN): $(PWRGATE_SRC) $(SRC_DIR)/power_gate_logic.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(PWRGATE_SRC)

$(CATCHALL_BIN): $(CATCHALL_SRC) $(SRC_DIR)/catchall_logic.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -o $@ $(CATCHALL_SRC)

.PHONY: run-sketch-logic clean-sketch-logic
run-sketch-logic: $(SKETCH_LOGIC_BINS)
	$(call run_marked,$(PROV_BIN))
	$(call run_marked,$(SELFTEST_BIN))
	$(call run_marked,$(CONFIG_BIN))
	$(call run_marked,$(AUTH_BIN))
	$(call run_marked,$(BTDEF_BIN))
	$(call run_marked,$(SDMOUNT_BIN))
	$(call run_marked,$(PEEK_BIN))
	$(call run_marked,$(VAULT_BIN))
	$(call run_marked,$(WITSTORE_BIN))
	$(call run_marked,$(BCNAUDIT_BIN))
	$(call run_marked,$(CAMGATE_BIN))
	$(call run_marked,$(HEALTH_BIN))
	$(call run_marked,$(PWRGATE_BIN))
	$(call run_marked,$(CATCHALL_BIN))
clean-sketch-logic:
	rm -f $(SKETCH_LOGIC_BINS)

run: run-sketch-logic
clean: clean-sketch-logic

# NvsManager's cross-task session lock (nvs_store.h, sweep F53). The sketch's
# one settings handle is opened by five tasks (loop, httpd, NimBLE host,
# Bluetooth bring-up, QR scan); nvs_store.h holds a recursive FreeRTOS mutex
# from begin() to the matching end(), counting nested sessions with the
# staged nvs_session_depth.h (canonical in firmware/common/storage/,
# check_csi_sync.sh). test_nvs_store_lock compiles the REAL nvs_store.h over
# stubs/nvs_store (Serial, String, a typed Preferences that refuses a double
# begin and counts closed-handle access) and the canary suite's fake
# recursive mutex, and runs the canary's scenarios on the WAP's code plus its
# nvs_store:: helpers and legacy wrappers; it also reads canary_wap.ino for
# the watchdog static_assert. Built twice: the _nolock binary is the first
# instance() meeting a failed mutex creation. test_nvs_session_balance reads
# every sketch source and fails on a block that opens a session and does not
# close it (the vault's pre-F53 shape). Hooked into `run` and `clean` by
# prerequisite so it adds no hunk to their lists.
NVSLOCK_BIN        = test_nvs_store_lock
NVSLOCK_NULL_BIN   = test_nvs_store_lock_nolock
NVSBAL_BIN         = test_nvs_session_balance
NVSLOCK_SRC        = test_nvs_store_lock.cpp
NVSBAL_SRC         = test_nvs_session_balance.cpp
NVSLOCK_FREERTOS   = ../../../tests_host/stubs/nvs_manager
NVSLOCK_DEPS       = $(NVSLOCK_SRC) $(SRC_DIR)/nvs_store.h $(SRC_DIR)/nvs_session_depth.h \
                     $(SRC_DIR)/canary_wap.ino $(wildcard stubs/nvs_store/*.h) \
                     $(wildcard $(NVSLOCK_FREERTOS)/freertos/*.h)
NVSLOCK_FLAGS      = -Werror -I stubs/nvs_store -I $(NVSLOCK_FREERTOS) -I $(SRC_DIR) \
                     -DCANARY_WAP_INO='"$(abspath $(SRC_DIR))/canary_wap.ino"'

$(NVSLOCK_BIN): $(NVSLOCK_DEPS)
	$(CXX) $(CXXFLAGS) $(NVSLOCK_FLAGS) -o $@ $(NVSLOCK_SRC)

$(NVSLOCK_NULL_BIN): $(NVSLOCK_DEPS)
	$(CXX) $(CXXFLAGS) $(NVSLOCK_FLAGS) -DNVS_STORE_TEST_NULL_LOCK -o $@ $(NVSLOCK_SRC)

# The scan reads the directory at run time, so it re-runs on every `make`.
$(NVSBAL_BIN): $(NVSBAL_SRC)
	$(CXX) $(CXXFLAGS) -Werror -DCANARY_WAP_SKETCH_DIR='"$(abspath $(SRC_DIR))"' -o $@ $(NVSBAL_SRC)

.PHONY: run-nvs-lock clean-nvs-lock
run-nvs-lock: $(NVSLOCK_BIN) $(NVSLOCK_NULL_BIN) $(NVSBAL_BIN)
	./$(NVSLOCK_BIN)
	./$(NVSLOCK_NULL_BIN)
	./$(NVSBAL_BIN)
clean-nvs-lock:
	rm -f $(NVSLOCK_BIN) $(NVSLOCK_NULL_BIN) $(NVSBAL_BIN)

run: run-nvs-lock
clean: clean-nvs-lock

# Sweep HA20: the canary-wap publishes its MQTT fingerprint and health key in
# lowercase hex (the sketch's mqtt_identity.h), like every other build. The
# suite derives the repo's Ed25519 test key and its fingerprint with OpenSSL,
# spells both strings with the encoder, builds a signed events body through
# the real device_signature.cpp and csi_event_wire.h (OpenSSL's deterministic
# Ed25519 as the signer) and byte-compares it with the lowercase body Home
# Assistant's test_fingerprint_case.py runs. It also reads canary_wap.ino and
# csi_mqtt.cpp (beacon_source_scan.h; both paths passed in, so the pins fail
# closed): both csi_mqtt::init calls and the device_signature::init call take
# the encoder's strings, and csi_mqtt.cpp publishes no other fp or key. Hooked
# into `run` and `clean` by prerequisite so it adds no hunk to their lists.
MQTTID_BIN  = test_mqtt_identity
MQTTID_SRC  = test_mqtt_identity.cpp
MQTTID_DEPS = $(MQTTID_SRC) beacon_source_scan.h $(SRC_DIR)/mqtt_identity.h \
              $(SRC_DIR)/device_signature.h $(SRC_DIR)/device_signature.cpp \
              $(SRC_DIR)/csi_event_wire.h

# The pins read both files at run time, so they re-run on every `make`.
$(MQTTID_BIN): $(MQTTID_DEPS)
	$(CXX) $(CXXFLAGS) -Werror -DCSI_TEST_HOST_BUILD -I $(SRC_DIR) \
	  -DCANARY_WAP_INO='"$(abspath $(SRC_DIR))/canary_wap.ino"' \
	  -DCSI_MQTT_CPP='"$(abspath $(SRC_DIR))/csi_mqtt.cpp"' \
	  -o $@ $(MQTTID_SRC) $(SRC_DIR)/device_signature.cpp -lcrypto

.PHONY: run-mqtt-identity clean-mqtt-identity
run-mqtt-identity: $(MQTTID_BIN)
	$(call run_marked,$(MQTTID_BIN))
clean-mqtt-identity:
	rm -f $(MQTTID_BIN)

run: run-mqtt-identity
clean: clean-mqtt-identity
