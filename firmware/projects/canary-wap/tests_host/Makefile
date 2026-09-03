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

# v0.3: Beacon origination two-pubkey co-signing invariants. Verifies the
# validation pipeline in beacon_channel.cpp::handle_alert_frame rejects
# single-signature frames, revoked pubkeys, origin==cosigner, wrong scope,
# and wrong magic.
BEACON_ORIG_BIN = test_beacon_origination
BEACON_ORIG_SRC = test_beacon_origination.cpp

# v0.4: Beacon solo origination (BOOT-button) invariants. Mirrors §6.2 of
# spec/beacon_channel_v0.md — solo frames require BCN_FLAG_SOLO_ORIGIN,
# certainty=Observed, originator_fp==cosigner_fp, and both signature slots
# carrying the same valid signature. Also checks that the dual-pubkey
# path still rejects collapsed signers without the flag.
BEACON_SOLO_BIN = test_beacon_solo_origination
BEACON_SOLO_SRC = test_beacon_solo_origination.cpp

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

$(BIN): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES)

$(CHIRP_INV_BIN): $(CHIRP_INV_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(CHIRP_INV_SRC)

$(CHIRP_SEC_BIN): $(CHIRP_SEC_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(CHIRP_SEC_SRC)

$(BEACON_ORIG_BIN): $(BEACON_ORIG_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(BEACON_ORIG_SRC)

$(BEACON_SOLO_BIN): $(BEACON_SOLO_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(BEACON_SOLO_SRC)

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
	$(CXX) $(CXXFLAGS) -I stubs/audio -I $(SRC_DIR) -o $@ $(AUDIO_SOURCES)

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

# Help QR: the verdict → Help Desk URL composer (help_qr_logic.h) — the
# probe-namespace bridge, worst-first precedence, and overflow refusal.
HELPQR_BIN = test_help_qr_logic
HELPQR_SRC = test_help_qr_logic.cpp

$(HELPQR_BIN): $(HELPQR_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(HELPQR_SRC)

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

run: $(BIN) $(CHIRP_INV_BIN) $(CHIRP_SEC_BIN) $(BEACON_ORIG_BIN) $(BEACON_SOLO_BIN) $(BEACON_TAMPER_BIN) $(OPERA_SEC_BIN) $(DEVSIG_BIN) $(CAPTIVE_DNS_BIN) $(CAPTIVE_PROBE_BIN) $(DEVPSEUDO_BIN) $(DEVPSEUDO_COMMON_BIN) $(GPS_COARSEN_BIN) $(GNSS_TIME_BIN) $(AUDIO_BIN) $(CSI_FEAT_BIN) $(WCA_BIN) $(FLEET_BEACON_BIN) $(EVDRIVE_BIN) $(EVVERIFY_BIN) $(HELPQR_BIN) $(CSI_SUBC_BIN) $(CSI_COMPAT_BINS) $(CSI_TRAFFIC_BIN) $(TAMPER_BIN)
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
	./$(AUDIO_BIN)
	./$(CSI_FEAT_BIN)
	./$(WCA_BIN)
	./$(FLEET_BEACON_BIN)
	./$(EVDRIVE_BIN)
	./$(EVVERIFY_BIN)
	./$(HELPQR_BIN)
	./$(CSI_SUBC_BIN)
	for b in $(CSI_COMPAT_BINS); do ./$$b || exit 1; done
	./$(CSI_TRAFFIC_BIN)
	./$(TAMPER_BIN)

clean:
	rm -f $(EVVERIFY_BIN) $(EVDRIVE_BIN) $(FLEET_BEACON_BIN) $(BIN) $(CHIRP_INV_BIN) $(CHIRP_SEC_BIN) $(BEACON_ORIG_BIN) $(BEACON_SOLO_BIN) $(BEACON_TAMPER_BIN) $(OPERA_SEC_BIN) $(DEVSIG_BIN) $(CAPTIVE_DNS_BIN) $(CAPTIVE_PROBE_BIN) $(DEVPSEUDO_BIN) $(DEVPSEUDO_COMMON_BIN) $(GPS_COARSEN_BIN) $(GNSS_TIME_BIN) $(AUDIO_BIN) $(CSI_FEAT_BIN) $(WCA_BIN) $(HELPQR_BIN) $(CSI_SUBC_BIN) $(CSI_COMPAT_BINS) $(CSI_TRAFFIC_BIN) $(TAMPER_BIN)
