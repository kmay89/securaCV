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

# The SD event log's reboot refill (csi_event_log::load_into_ring): the REAL
# csi_event_log.cpp and the staged CSI chokepoint (csi_event_inject) over a
# RAM card (stubs/sd_fake) — not before arm_load() (the id floor restored),
# tail window (a boundary-aligned window keeps its first line), refused
# lines, owner-file cards, no card writes, the watchdog fed on the walk, no
# read after a live row, once per boot. The staged sources are byte-identical to
# firmware/common/csi/src (check_csi_sync.sh).
EVLOAD_BIN = test_csi_event_log_load
EVLOAD_SRC = test_csi_event_log_load.cpp
EVLOAD_LIB = $(SRC_DIR)/csi_event_log.cpp $(SRC_DIR)/csi_event.cpp \
             $(SRC_DIR)/csi_module.cpp $(SRC_DIR)/csi_bundler.cpp

$(EVLOAD_BIN): $(EVLOAD_SRC) $(EVLOAD_LIB) $(SRC_DIR)/csi_event_log.h $(SRC_DIR)/csi_event.h \
               $(SRC_DIR)/csi_event_log_line.h $(SRC_DIR)/csi_event_id_floor.h $(wildcard stubs/sd_fake/*.h)
	$(CXX) $(CXXFLAGS) -Werror -Wno-unused-variable -Wno-stringop-truncation -DCSI_TEST_HOST_BUILD \
	  -I stubs/sd_fake -I $(SRC_DIR) -o $@ $(EVLOAD_SRC) $(EVLOAD_LIB)

.PHONY: run-evlog-load clean-evlog-load
run-evlog-load: $(EVLOAD_BIN)
	$(call run_marked,$(EVLOAD_BIN))
clean-evlog-load:
	rm -f $(EVLOAD_BIN)
run: run-evlog-load
clean: clean-evlog-load

# A dismissal survives that refill: csi_event_log::queue_dismissal /
# flush_dismissals write the dismissed row as one more line, load_into_ring
# restores the event dismissed, a dismissal before the egress logs the
# original does not lose it, and a dismissal the log cannot take yet (no
# open log, a log at MAX_BYTES) waits instead of being dropped.
# Same REAL sources and RAM card as the load test above.
EVDISMISS_BIN = test_csi_event_log_dismiss
EVDISMISS_SRC = test_csi_event_log_dismiss.cpp

$(EVDISMISS_BIN): $(EVDISMISS_SRC) $(EVLOAD_LIB) $(SRC_DIR)/csi_event_log.h $(SRC_DIR)/csi_event.h \
                  $(SRC_DIR)/csi_event_log_line.h $(SRC_DIR)/csi_event_id_floor.h $(wildcard stubs/sd_fake/*.h)
	$(CXX) $(CXXFLAGS) -Werror -Wno-unused-variable -Wno-stringop-truncation -DCSI_TEST_HOST_BUILD \
	  -I stubs/sd_fake -I $(SRC_DIR) -o $@ $(EVDISMISS_SRC) $(EVLOAD_LIB)

.PHONY: run-evlog-dismiss clean-evlog-dismiss
run-evlog-dismiss: $(EVDISMISS_BIN)
	$(call run_marked,$(EVDISMISS_BIN))
clean-evlog-dismiss:
	rm -f $(EVDISMISS_BIN)
run: run-evlog-dismiss
clean: clean-evlog-dismiss

# The committed-event egress (backlog F78): the REAL csi_event_egress.cpp,
# csi_event_log.cpp over the RAM card and the staged CSI chokepoint, with
# the staged csi_event_backfill.h planner, against a model of Home
# Assistant's replay gate. A row committed between a reconnect and the
# loop's drain (a direct row, a closed bundle) waits behind the backlog;
# a commit inside the backfill walk publishes nothing and moves no
# watermark; reboots, no broker or a changed one, dismissals, send, append
# and body-build failures, forged card lines, the RAM hold (its failure
# paths, ambient rows, a card not open yet or closed for a moment), torn
# tails and failed rewrites, and the queue bound. stubs/egress: a FreeRTOS
# queue, Preferences, a clock the test moves.
EGRESS_BIN = test_wap_event_egress
EGRESS_SRC = test_wap_event_egress.cpp
EGRESS_LIB = $(SRC_DIR)/csi_event_egress.cpp $(EVLOAD_LIB)

$(EGRESS_BIN): $(EGRESS_SRC) $(EGRESS_LIB) $(SRC_DIR)/csi_event_egress.h $(SRC_DIR)/csi_event_log.h \
               $(SRC_DIR)/csi_event.h $(SRC_DIR)/csi_event_backfill.h $(SRC_DIR)/csi_event_log_line.h \
               $(SRC_DIR)/csi_event_id_floor.h $(SRC_DIR)/csi_mqtt.h $(SRC_DIR)/csi_module_settings_nvs.h \
               $(wildcard $(SRC_DIR)/loop_snapshot.h $(SRC_DIR)/loop_command_ring.h) \
               $(wildcard stubs/egress/*.h stubs/egress/freertos/*.h stubs/sd_fake/*.h)
	$(CXX) $(CXXFLAGS) -Werror -Wno-comment -Wno-stringop-truncation -DCSI_TEST_HOST_BUILD \
	  -I stubs/egress -I stubs/sd_fake -I $(SRC_DIR) -o $@ $(EGRESS_SRC) $(EGRESS_LIB)

.PHONY: run-wap-egress clean-wap-egress
run-wap-egress: $(EGRESS_BIN)
	$(call run_marked,$(EGRESS_BIN))
clean-wap-egress:
	rm -f $(EGRESS_BIN)
run: run-wap-egress
clean: clean-wap-egress

# GET /api/diagnostics's body (sweep F149): the REAL wap_diagnostics.h the
# handler builds it with. Every key, the egress counters as an object (null
# before the first pump), and the widest body inside the route's kJsonMax
# buffer; sys_monitor.h is read (its path passed in, so the read fails closed)
# for the longest degradation-level name. check_wap_event_egress.py rule 12
# holds the handler to the builder and the buffer.
DIAG_BIN = test_wap_diagnostics
DIAG_SRC = test_wap_diagnostics.cpp

$(DIAG_BIN): $(DIAG_SRC) $(SRC_DIR)/wap_diagnostics.h $(SRC_DIR)/csi_event_egress.h \
             $(SRC_DIR)/csi_event_backfill.h $(SRC_DIR)/sys_monitor.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) \
	  -DSYS_MONITOR_H='"$(abspath $(SRC_DIR))/sys_monitor.h"' -o $@ $(DIAG_SRC)

.PHONY: run-wap-diagnostics clean-wap-diagnostics
run-wap-diagnostics: $(DIAG_BIN)
	$(call run_marked,$(DIAG_BIN))
clean-wap-diagnostics:
	rm -f $(DIAG_BIN)

run: run-wap-diagnostics
clean: clean-wap-diagnostics

# The CSI modules' boot init (sweep F93): the REAL csi_settings_nvs.cpp (the
# sketch's csi_module_settings_* readers, by the shared
# csi_module_settings_nvs.h rule, and its boot init through one read-only
# handle), the staged CSI library and modules over a
# typed NVS that outlives a modeled reboot (stubs/module_boot), booted in
# csi_integration::init()'s order: a saved preset, threshold, pet mode and
# anomaly cooldown apply from the boot's first windows, each module's
# init() runs once per boot and commits nothing, and a settings change
# still applies at once. It reads csi_integration.cpp's register_v1_modules()
# to keep its module list the device's.
MODBOOT_BIN = test_wap_module_boot
MODBOOT_SRC = test_wap_module_boot.cpp
MODBOOT_LIB = $(SRC_DIR)/csi_settings_nvs.cpp $(SRC_DIR)/csi_tune_lab.cpp \
              $(SRC_DIR)/csi_event.cpp $(SRC_DIR)/csi_bundler.cpp \
              $(SRC_DIR)/csi_module.cpp $(SRC_DIR)/core_presence.cpp $(SRC_DIR)/core_breathing.cpp \
              $(SRC_DIR)/core_activity_ribbon.cpp $(SRC_DIR)/meta_daily_summary.cpp \
              $(SRC_DIR)/meta_quiet_hours.cpp $(SRC_DIR)/anomaly_baseline.cpp \
              $(SRC_DIR)/wifi_channel_activity.cpp $(SRC_DIR)/core_multilink_fusion.cpp \
              $(SRC_DIR)/meta_empty_room_baseline.cpp $(SRC_DIR)/ble_events_module.cpp \
              $(SRC_DIR)/tamper_events_module.cpp

$(MODBOOT_BIN): $(MODBOOT_SRC) $(MODBOOT_LIB) $(SRC_DIR)/csi_module.h $(SRC_DIR)/csi_module_settings_nvs.h \
                $(SRC_DIR)/csi_settings_nvs.h $(SRC_DIR)/csi_tune_lab.h \
                $(SRC_DIR)/csi_event.h $(SRC_DIR)/csi_bundler.h $(SRC_DIR)/csi_integration.cpp \
                $(SRC_DIR)/acoustic_events_module.cpp $(SRC_DIR)/vault_events_module.cpp $(SRC_DIR)/ble_scout.cpp \
                $(wildcard stubs/module_boot/*.h)
	$(CXX) $(CXXFLAGS) -Werror -Wno-stringop-truncation -DCSI_TEST_HOST_BUILD \
	  -DWAP_SKETCH_DIR='"$(abspath $(SRC_DIR))"' \
	  -I stubs/module_boot -I $(SRC_DIR) -o $@ $(MODBOOT_SRC) $(MODBOOT_LIB)

.PHONY: run-wap-module-boot clean-wap-module-boot
run-wap-module-boot: $(MODBOOT_BIN)
	$(call run_marked,$(MODBOOT_BIN))
clean-wap-module-boot:
	rm -f $(MODBOOT_BIN)
run: run-wap-module-boot
clean: clean-wap-module-boot

# The Tuning Lab and its Quiet Hours (sweeps F123, F128): the REAL
# csi_tune_lab.cpp (the knobs' table and the POST's store-and-apply) and
# csi_settings_nvs.cpp (the one Quiet Hours default, its reader, the
# dashboard's store and the apply to the chokepoint), with the staged CSI
# library and modules over the same fake NVS (stubs/module_boot). The Lab
# declares the device's own Quiet Hours default, which the dashboard starts
# from too; a Quiet Hours knob or bundle import applies to the chokepoint at
# once; every knob applies to its own group; the dashboard and the Lab write
# the rows the reader reads, under their stored names. It reads
# csi_integration.cpp to hold its handlers thin around that code
# (mutation-checked).
TUNELAB_BIN = test_wap_tune_lab
TUNELAB_SRC = test_wap_tune_lab.cpp
TUNELAB_LIB = $(SRC_DIR)/csi_tune_lab.cpp $(SRC_DIR)/csi_settings_nvs.cpp \
              $(SRC_DIR)/csi_event.cpp $(SRC_DIR)/csi_bundler.cpp $(SRC_DIR)/csi_module.cpp \
              $(SRC_DIR)/core_presence.cpp $(SRC_DIR)/core_breathing.cpp \
              $(SRC_DIR)/anomaly_baseline.cpp $(SRC_DIR)/meta_quiet_hours.cpp

$(TUNELAB_BIN): $(TUNELAB_SRC) $(TUNELAB_LIB) $(SRC_DIR)/csi_tune_lab.h $(SRC_DIR)/csi_settings_nvs.h \
                $(SRC_DIR)/csi_module_settings_nvs.h $(SRC_DIR)/csi_module.h $(SRC_DIR)/csi_event.h \
                $(SRC_DIR)/csi_integration.cpp $(SRC_DIR)/csi_dashboard_html.h \
                $(wildcard stubs/module_boot/*.h)
	$(CXX) $(CXXFLAGS) -Werror -Wno-stringop-truncation -DCSI_TEST_HOST_BUILD \
	  -DWAP_SKETCH_DIR='"$(abspath $(SRC_DIR))"' \
	  -I stubs/module_boot -I $(SRC_DIR) -o $@ $(TUNELAB_SRC) $(TUNELAB_LIB)

.PHONY: run-wap-tune-lab clean-wap-tune-lab
run-wap-tune-lab: $(TUNELAB_BIN)
	$(call run_marked,$(TUNELAB_BIN))
clean-wap-tune-lab:
	rm -f $(TUNELAB_BIN)
run: run-wap-tune-lab
clean: clean-wap-tune-lab

# meta.daily_summary (sweep F121): canary_wap.ino's clock feed,
# update_csi_clock_offset() cut verbatim (firmware/tests_host/
# cut_functions.awk) with the GPS_CLOCK_FLOOR line it guards on, over the
# staged CSI library and modules booted as csi_integration::init() boots
# them (the sketch's real csi_settings_nvs.cpp, over stubs/module_boot). A
# synced day crossing 23:55 commits exactly one row, a reboot after it
# commits no second, an unsynced clock commits none, a first clock inside
# the window waits for the next day, Quiet Hours over 23:55 hold the row,
# and the row carries what docs/csi_modules.md says. Hooked into `run` and
# `clean` by prerequisite so it adds no hunk to their lists.
DAILY_BIN = test_wap_daily_summary
DAILY_SRC = test_wap_daily_summary.cpp
DAILY_INC = test_wap_daily_summary_clock.inc
DAILY_INO = $(SRC_DIR)/canary_wap.ino
DAILY_CUT = ../../../tests_host/cut_functions.awk

$(DAILY_INC): $(DAILY_INO) $(DAILY_CUT) Makefile
	@test "$$(grep -cE '^static const time_t GPS_CLOCK_FLOOR = ' $(DAILY_INO))" = 1 || { \
	  echo "canary_wap.ino: no single GPS_CLOCK_FLOOR line for update_csi_clock_offset's guard" >&2; exit 1; }
	@grep -nE '^static const time_t GPS_CLOCK_FLOOR = ' $(DAILY_INO) | \
	  sed -E 's|^([0-9]+):(.*)$$|#line \1 "$(DAILY_INO)"\n\2|' > $@.tmp
	awk -v 'sigs=static void update_csi_clock_offset(' -f $(DAILY_CUT) $(DAILY_INO) >> $@.tmp && mv $@.tmp $@

$(DAILY_BIN): $(DAILY_SRC) $(DAILY_INC) $(MODBOOT_LIB) $(SRC_DIR)/csi_settings_nvs.h \
              $(SRC_DIR)/csi_module_settings_nvs.h $(SRC_DIR)/meta_daily_summary.h \
              $(SRC_DIR)/csi_event.h $(SRC_DIR)/csi_event_wire.h $(SRC_DIR)/tz_rule.h \
              $(wildcard stubs/module_boot/*.h)
	$(CXX) $(CXXFLAGS) -Werror -Wno-stringop-truncation -DCSI_TEST_HOST_BUILD \
	  -I stubs/module_boot -I $(SRC_DIR) -I . -o $@ $(DAILY_SRC) $(MODBOOT_LIB)

.PHONY: run-wap-daily-summary clean-wap-daily-summary
run-wap-daily-summary: $(DAILY_BIN)
	$(call run_marked,$(DAILY_BIN))
clean-wap-daily-summary:
	rm -f $(DAILY_BIN) $(DAILY_INC)
run: run-wap-daily-summary
clean: clean-wap-daily-summary

# A first boot after an NVS erase and the "securacv" namespace (sweep F201):
# setup_wizard.h's init(), canary_wap.ino's nvs_load_key() / nvs_store_key()
# (with its NVS_KEY_PRIV line) over the real nvs_store.h, and power_monitor.h's
# load_nvs_state(), each cut verbatim (firmware/tests_host/cut_functions.awk),
# over a fake NVS that models namespaces and counts Preferences' error line
# (stubs/first_boot; Arduino.h from stubs/nvs_store, the recursive mutex from
# the canary suite's stubs/nvs_manager). Run in setup()'s order, which the
# suite's source pins hold: a first boot logs no line, a provisioned boot
# reads as before, an NVS fault keeps its lines. Hooked into `run` and
# `clean` by prerequisite so it adds no hunk to their lists.
FIRSTBOOT_BIN    = test_wap_first_boot_nvs
FIRSTBOOT_SRC    = test_wap_first_boot_nvs.cpp
FIRSTBOOT_WIZ    = test_wap_first_boot_nvs_wizard.inc
FIRSTBOOT_KEY    = test_wap_first_boot_nvs_key.inc
FIRSTBOOT_PWR    = test_wap_first_boot_nvs_power.inc
FIRSTBOOT_INCS   = $(FIRSTBOOT_WIZ) $(FIRSTBOOT_KEY) $(FIRSTBOOT_PWR)
FIRSTBOOT_CUT    = ../../../tests_host/cut_functions.awk

$(FIRSTBOOT_WIZ): $(SRC_DIR)/setup_wizard.h $(FIRSTBOOT_CUT) Makefile
	awk -v 'sigs=inline bool init() {' -f $(FIRSTBOOT_CUT) $(SRC_DIR)/setup_wizard.h > $@.tmp && mv $@.tmp $@

$(FIRSTBOOT_PWR): $(SRC_DIR)/power_monitor.h $(FIRSTBOOT_CUT) Makefile
	awk -v 'sigs=inline void load_nvs_state() {' -f $(FIRSTBOOT_CUT) $(SRC_DIR)/power_monitor.h > $@.tmp && mv $@.tmp $@

$(FIRSTBOOT_KEY): $(SRC_DIR)/canary_wap.ino $(FIRSTBOOT_CUT) Makefile
	@test "$$(grep -cE '^static const char\* NVS_KEY_PRIV +=' $(SRC_DIR)/canary_wap.ino)" = 1 || { \
	  echo "canary_wap.ino: no single NVS_KEY_PRIV line for nvs_load_key()" >&2; exit 1; }
	@grep -nE '^static const char\* NVS_KEY_PRIV +=' $(SRC_DIR)/canary_wap.ino | \
	  sed -E 's|^([0-9]+):(.*)$$|#line \1 "$(SRC_DIR)/canary_wap.ino"\n\2|' > $@.tmp
	awk -v 'sigs=static bool nvs_load_key(|static bool nvs_store_key(' -f $(FIRSTBOOT_CUT) \
	  $(SRC_DIR)/canary_wap.ino >> $@.tmp && mv $@.tmp $@

$(FIRSTBOOT_BIN): $(FIRSTBOOT_SRC) $(FIRSTBOOT_INCS) $(SRC_DIR)/nvs_store.h $(SRC_DIR)/nvs_session_depth.h \
                  $(SRC_DIR)/csi_module_settings_nvs.h $(wildcard stubs/first_boot/*.h) \
                  stubs/nvs_store/Arduino.h $(wildcard $(NVSLOCK_FREERTOS)/freertos/*.h)
	$(CXX) $(CXXFLAGS) -Werror -I stubs/first_boot -I stubs/nvs_store -I $(NVSLOCK_FREERTOS) \
	  -I $(SRC_DIR) -I . -DWAP_SKETCH_DIR='"$(abspath $(SRC_DIR))"' -o $@ $(FIRSTBOOT_SRC)

.PHONY: run-wap-first-boot-nvs clean-wap-first-boot-nvs
run-wap-first-boot-nvs: $(FIRSTBOOT_BIN)
	$(call run_marked,$(FIRSTBOOT_BIN))
clean-wap-first-boot-nvs:
	rm -f $(FIRSTBOOT_BIN) $(FIRSTBOOT_INCS)
run: run-wap-first-boot-nvs
clean: clean-wap-first-boot-nvs

# The differential-privacy budget (dp.h / dp.cpp) is enforced and fails
# closed: all-or-nothing reservations, value-independent refusals, exact
# exhaustion, a refill only on the 4 h uptime clock (never on a manual
# rotation, never on a reboot: the spend persisted with its epoch, over a
# fake ledger_store), no release completing across a refill, no overspend
# under racing threads, and the calibrated noise scale. The REAL dp.cpp over
# stubs/dp; rf_presence.cpp and rf_presence_api.h are read for source pins
# (their paths are passed in, so the pins fail closed).
DPBUDGET_BIN = test_dp_budget
DPBUDGET_SRC = test_dp_budget.cpp

$(DPBUDGET_BIN): $(DPBUDGET_SRC) $(SRC_DIR)/dp.cpp $(SRC_DIR)/dp.h $(SRC_DIR)/health_log.h \
                 $(SRC_DIR)/rf_presence.cpp $(SRC_DIR)/rf_presence_api.h beacon_source_scan.h \
                 $(wildcard stubs/dp/*.h)
	$(CXX) $(CXXFLAGS) -Werror -pthread -I stubs/dp -I $(SRC_DIR) \
	  -DRF_PRESENCE_CPP='"$(abspath $(SRC_DIR))/rf_presence.cpp"' \
	  -DRF_PRESENCE_API_H='"$(abspath $(SRC_DIR))/rf_presence_api.h"' \
	  -o $@ $(DPBUDGET_SRC) $(SRC_DIR)/dp.cpp

.PHONY: run-dp-budget clean-dp-budget
run-dp-budget: $(DPBUDGET_BIN)
	$(call run_marked,$(DPBUDGET_BIN))
clean-dp-budget:
	rm -f $(DPBUDGET_BIN)
run: run-dp-budget
clean: clean-dp-budget

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

$(PAIR_FRAME_BIN): $(PAIR_FRAME_SRC) $(SRC_DIR)/mesh_pair_frame.h $(SRC_DIR)/mesh_wire.h
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

# v0.4 (spec §4.5 — awaiting crypto review, not bench-verified): the Opera
# mesh wire registry. mesh_wire.h is the PlatformIO tree's header, staged
# byte-identical (check_mesh_sync.sh); its own suite runs here against the
# STAGED copy, and test_mesh_wire_wap pins what mesh_network.{h,cpp} do with
# it (every MessageType value an initializer from the registry, the version
# gate before the type read, the header field order on send, the pairing
# classifier's types) by reading the real files (beacon_source_scan.h).
WIRE_WAP_BIN    = test_mesh_wire_wap
WIRE_WAP_SRC    = test_mesh_wire_wap.cpp
WIRE_STAGED_BIN = test_mesh_wire_staged
WIRE_STAGED_SRC = ../../../canary/lib/securacv_mesh/test_mesh_wire.cpp

$(WIRE_WAP_BIN): $(WIRE_WAP_SRC) $(SRC_DIR)/mesh_wire.h $(SRC_DIR)/mesh_pair_frame.h $(SRC_DIR)/mesh_network.h $(SRC_DIR)/mesh_network.cpp beacon_source_scan.h
	$(CXX) $(CXXFLAGS) -Werror -I $(SRC_DIR) -DMESH_NETWORK_CPP='"$(abspath $(SRC_DIR))/mesh_network.cpp"' -DMESH_NETWORK_H='"$(abspath $(SRC_DIR))/mesh_network.h"' -o $@ $(WIRE_WAP_SRC)

# The sketch stages the registry alone, not the PIO envelope that takes its
# version from it, so the suite's envelope include (its macro-clash guard's
# second half) is off here; the PIO job compiles it with the envelope.
$(WIRE_STAGED_BIN): $(WIRE_STAGED_SRC) $(SRC_DIR)/mesh_wire.h
	$(CXX) $(CXXFLAGS) -Werror -DMESH_WIRE_STAGED_ONLY -I $(SRC_DIR) -o $@ $(WIRE_STAGED_SRC)

.PHONY: run-wire clean-wire
run-wire: $(WIRE_WAP_BIN) $(WIRE_STAGED_BIN)
	./$(WIRE_WAP_BIN)
	./$(WIRE_STAGED_BIN)
clean-wire:
	rm -f $(WIRE_WAP_BIN) $(WIRE_STAGED_BIN)

run: run-wire
clean: clean-wire

# The v0.4 review's three pre-existing receive-path gaps (host-tested only,
# not bench-verified, awaiting crypto review): the replay counter convention
# (first counter 1, strict greater-than, no rx==0 exemption — the same lines
# pinned in the PIO mesh_session.cpp), no frame moving a member's address
# (spec §8.3: a frame from any address but the signer's bound one drops
# before verify; it was re-bound after verify, and before that ahead of
# it), and every struct-payload handler refusing any length but its
# struct's (the PIO 6-byte TAMPER_ALERT, mesh_alert.h, used to be read as
# the 56-byte struct).
# Reads the real mesh_network.{h,cpp} (beacon_source_scan.h; paths passed in,
# so the pins fail closed) and includes the PIO mesh_alert.h for its length.
RX_GATES_BIN = test_mesh_rx_gates_wap
RX_GATES_SRC = test_mesh_rx_gates_wap.cpp
PIO_MESH_SRC = ../../../canary/lib/securacv_mesh/src

$(RX_GATES_BIN): $(RX_GATES_SRC) $(SRC_DIR)/mesh_network.h $(SRC_DIR)/mesh_network.cpp $(PIO_MESH_SRC)/mesh_session.cpp $(PIO_MESH_SRC)/mesh_alert.h beacon_source_scan.h
	$(CXX) $(CXXFLAGS) -Werror -I $(PIO_MESH_SRC) -DMESH_NETWORK_CPP='"$(abspath $(SRC_DIR))/mesh_network.cpp"' -DMESH_NETWORK_H='"$(abspath $(SRC_DIR))/mesh_network.h"' -DMESH_SESSION_CPP='"$(abspath $(PIO_MESH_SRC))/mesh_session.cpp"' -o $@ $(RX_GATES_SRC)

.PHONY: run-rx-gates clean-rx-gates
run-rx-gates: $(RX_GATES_BIN)
	$(call run_marked,$(RX_GATES_BIN))
clean-rx-gates:
	rm -f $(RX_GATES_BIN)

run: run-rx-gates
clean: clean-rx-gates

# Which address canary-wap sends a member's frames to, and what may change
# it (spec §8.3), run against the REAL mesh_network.cpp: the file is
# #included and compiled on the host against stubs/mesh_net (an ESP-NOW peer
# list and send log, an in-memory NVS, OpenSSL's Ed25519 / SHA-256 /
# ChaCha20-Poly1305) and stubs/pair_crypto (OpenSSL's X25519), with the
# real airtime governor, channel policy, deny-list, wire codecs and
# mesh_crypto linked. mesh_net_sim.h runs several devices on that one image
# and moves frames between them as bytes.
MESH_ADDR_BIN = test_mesh_address_wap
MESH_ADDR_SRC = test_mesh_address_wap.cpp
MESH_ADDR_LINK = $(SRC_DIR)/airtime_governor.cpp $(SRC_DIR)/mesh_channel_policy.cpp \
                 $(SRC_DIR)/mesh_revocation.cpp $(SRC_DIR)/mesh_beacon.cpp \
                 $(SRC_DIR)/mesh_channel_hop.cpp $(SRC_DIR)/mesh_hub_election.cpp \
                 $(SRC_DIR)/mesh_crypto.cpp

$(MESH_ADDR_BIN): $(MESH_ADDR_SRC) mesh_net_sim.h $(SRC_DIR)/csi_module_settings_nvs.h $(wildcard stubs/mesh_net/*.h stubs/mesh_net/freertos/*.h stubs/mesh_net/mbedtls/*.h) $(SRC_DIR)/loop_command_ring.h $(SRC_DIR)/mesh_network.h $(SRC_DIR)/mesh_network.cpp $(MESH_ADDR_LINK)
	$(CXX) $(CXXFLAGS) -Werror -I stubs/mesh_net -I stubs/pair_crypto -I $(SRC_DIR) -DMESH_NETWORK_CPP='"$(abspath $(SRC_DIR))/mesh_network.cpp"' -o $@ $(MESH_ADDR_SRC) $(MESH_ADDR_LINK) -lcrypto

.PHONY: run-mesh-address clean-mesh-address
run-mesh-address: $(MESH_ADDR_BIN)
	$(call run_marked,$(MESH_ADDR_BIN))
clean-mesh-address:
	rm -f $(MESH_ADDR_BIN)

run: run-mesh-address
clean: clean-mesh-address

# Whether a canary-wap opera comes up, stays reachable, and says so when a
# pairing fails (sweep F71, F73, F74, F75, F76), on the same harness and
# link as test_mesh_address_wap: send counters that survive a reboot, a
# refused partner reported as a failure, the DISCOVER's broadcast
# registration, the owners confirming in either order, and an unheard opera
# announcing itself.
MESH_LIVE_BIN = test_mesh_liveness_wap
MESH_LIVE_SRC = test_mesh_liveness_wap.cpp

$(MESH_LIVE_BIN): $(MESH_LIVE_SRC) mesh_net_sim.h $(SRC_DIR)/csi_module_settings_nvs.h $(wildcard stubs/mesh_net/*.h stubs/mesh_net/freertos/*.h stubs/mesh_net/mbedtls/*.h) $(SRC_DIR)/loop_command_ring.h $(SRC_DIR)/mesh_network.h $(SRC_DIR)/mesh_network.cpp $(MESH_ADDR_LINK)
	$(CXX) $(CXXFLAGS) -Werror -I stubs/mesh_net -I stubs/pair_crypto -I $(SRC_DIR) -DMESH_NETWORK_CPP='"$(abspath $(SRC_DIR))/mesh_network.cpp"' -o $@ $(MESH_LIVE_SRC) $(MESH_ADDR_LINK) -lcrypto

.PHONY: run-mesh-liveness clean-mesh-liveness
run-mesh-liveness: $(MESH_LIVE_BIN)
	$(call run_marked,$(MESH_LIVE_BIN))
clean-mesh-liveness:
	rm -f $(MESH_LIVE_BIN)

run: run-mesh-liveness
clean: clean-mesh-liveness

# The owner's mesh commands run on the loop task (sweep F96), on the same
# harness and link as test_mesh_address_wap: the REAL mesh_network.cpp's
# submit() and update()'s drain, a removal, every command, a disabled mesh
# turning back on, a command the loop task does not reach (withdrawn), a full
# ring, the post order, and a pairing driven over REST end to end — with no
# NVS write or ESP-NOW call made from the handler's side.
MESH_CMD_BIN = test_mesh_commands_wap
MESH_CMD_SRC = test_mesh_commands_wap.cpp

$(MESH_CMD_BIN): $(MESH_CMD_SRC) mesh_net_sim.h $(SRC_DIR)/csi_module_settings_nvs.h $(wildcard stubs/mesh_net/*.h stubs/mesh_net/freertos/*.h stubs/mesh_net/mbedtls/*.h) $(SRC_DIR)/mesh_network.h $(SRC_DIR)/mesh_network.cpp $(SRC_DIR)/loop_command_ring.h $(SRC_DIR)/http_status_line.h $(MESH_ADDR_LINK)
	$(CXX) $(CXXFLAGS) -Werror -I stubs/mesh_net -I stubs/pair_crypto -I $(SRC_DIR) -DMESH_NETWORK_CPP='"$(abspath $(SRC_DIR))/mesh_network.cpp"' -o $@ $(MESH_CMD_SRC) $(MESH_ADDR_LINK) -lcrypto

.PHONY: run-mesh-commands clean-mesh-commands
run-mesh-commands: $(MESH_CMD_BIN)
	$(call run_marked,$(MESH_CMD_BIN))
clean-mesh-commands:
	rm -f $(MESH_CMD_BIN)

run: run-mesh-commands
clean: clean-mesh-commands

# The owner's Chirp commands run on the loop task (sweep F111): the REAL
# chirp_channel.cpp, #included and compiled on the host against
# stubs/mesh_net (an ESP-NOW send log, an in-memory NVS, OpenSSL's Ed25519
# and SHA-256) with the real airtime governor, and the test's own wall clock.
# The handler's side submits, the loop task's update() drains: enable on a
# disabled channel, every command, a refused send's reason, a command the
# loop task never reaches (withdrawn), a full ring (busy) and the post
# order, with no NVS write or ESP-NOW call made from the handler's side.
# And the status routes' view (sweep F138): what GET /api/chirp, /nearby and
# /recent read is what the loop task last published (loop_snapshot.h), from
# real presence, witness and confirmation frames, the prune and each command.
CHIRP_CMD_BIN = test_chirp_commands_wap
CHIRP_CMD_SRC = test_chirp_commands_wap.cpp

$(CHIRP_CMD_BIN): $(CHIRP_CMD_SRC) $(wildcard stubs/mesh_net/*.h stubs/mesh_net/freertos/*.h stubs/mesh_net/mbedtls/*.h) $(SRC_DIR)/chirp_channel.cpp $(SRC_DIR)/mesh_network.h $(SRC_DIR)/loop_command_ring.h $(SRC_DIR)/loop_snapshot.h $(SRC_DIR)/http_status_line.h $(SRC_DIR)/airtime_governor.cpp
	$(CXX) $(CXXFLAGS) -Werror -I stubs/mesh_net -I $(SRC_DIR) -DCHIRP_CHANNEL_CPP='"$(abspath $(SRC_DIR))/chirp_channel.cpp"' -o $@ $(CHIRP_CMD_SRC) $(SRC_DIR)/airtime_governor.cpp -lcrypto

.PHONY: run-chirp-commands clean-chirp-commands
run-chirp-commands: $(CHIRP_CMD_BIN)
	$(call run_marked,$(CHIRP_CMD_BIN))
clean-chirp-commands:
	rm -f $(CHIRP_CMD_BIN)

run: run-chirp-commands
clean: clean-chirp-commands

# The owner's Bluetooth commands run on the loop task (sweep F111): the REAL
# bluetooth_channel.cpp, #included and compiled on the host against
# stubs/bt (a NimBLE-Arduino 2.x stand-in that records each radio, bond and
# passkey call with the task that made it, and an in-memory main NVS). A PIN
# confirm, reject and cancel answer the pending pairing once, on the loop
# task, the timeout's pass included; every command's radio, bond and NVS
# calls are the loop task's; settings apply only the fields named; no
# command brings the stack up (init() is the handler's or the worker's);
# withdrawn, busy and post order through the real submit() and update().
# And a settings POST's "enabled" turns Bluetooth off and on the way
# POST /disable and /enable do (sweep F144); the NimBLE host task's
# callbacks change nothing until update() applies their events on the loop
# task, the pending pairing's copy has one owner, and a full event queue
# keeps a link's room and fails a pairing closed (sweep F143). And the
# FULL profile's wiring (sweep F171): the channel's init() and Opera's
# (ble_opera.h) in either order through one server-callback dispatcher
# (ble_server_dispatch.h), so a passkey waits for the owner's confirm.
BT_CMD_BIN = test_bluetooth_commands_wap
BT_CMD_SRC = test_bluetooth_commands_wap.cpp

BT_CMD_DEPS = $(BT_CMD_SRC) $(wildcard stubs/bt/*.h stubs/bt/freertos/*.h) $(SRC_DIR)/bluetooth_channel.cpp $(SRC_DIR)/bluetooth_channel.h $(SRC_DIR)/loop_command_ring.h $(SRC_DIR)/loop_event_queue.h $(SRC_DIR)/http_status_line.h $(SRC_DIR)/ble_standard_profiles.h $(SRC_DIR)/bt_defaults.h $(SRC_DIR)/ble_heap_guard.h $(SRC_DIR)/ble_server_dispatch.h $(SRC_DIR)/ble_opera.h $(SRC_DIR)/ble_config.h $(SRC_DIR)/fleet_beacon.h

$(BT_CMD_BIN): $(BT_CMD_DEPS)
	$(CXX) $(CXXFLAGS) -Werror -pthread -I stubs/bt -I $(SRC_DIR) -DBLUETOOTH_CHANNEL_CPP='"$(abspath $(SRC_DIR))/bluetooth_channel.cpp"' -o $@ $(BT_CMD_SRC)

# `make tsan-bt-commands` runs the same suite under -fsanitize=thread (not
# part of `run`: it needs a sanitizer runtime). Its threaded test plays the
# NimBLE host task, the loop task and the HTTP server's on three threads
# (sweep F143): a callback that wrote the channel's state is a race there.
.PHONY: run-bt-commands clean-bt-commands tsan-bt-commands
run-bt-commands: $(BT_CMD_BIN)
	$(call run_marked,$(BT_CMD_BIN))
tsan-bt-commands: $(BT_CMD_DEPS)
	$(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread -I stubs/bt -I $(SRC_DIR) -DBLUETOOTH_CHANNEL_CPP='"$(abspath $(SRC_DIR))/bluetooth_channel.cpp"' -o $(BT_CMD_BIN)_tsan $(BT_CMD_SRC)
	./$(BT_CMD_BIN)_tsan
clean-bt-commands:
	rm -f $(BT_CMD_BIN) $(BT_CMD_BIN)_tsan

run: run-bt-commands
clean: clean-bt-commands

# The MQTT bridge re-inits on the loop task (sweep F106): the REAL
# csi_mqtt.cpp over stubs/mqtt (an in-memory NVS, a request/response httpd,
# and a fake esp_mqtt client the test defines, which records the task of
# every call and notices a client destroyed under a publish). The config
# POST, the test handler and a QR provisioning only request a re-init;
# loop() runs one per batch of requests; a publish in flight keeps its
# client; the waits are bounded; the auto-update switch set from the httpd
# task is published by loop(). And the loop task's publishes are bounded
# (sweep F112): every client carries csi_mqtt.h's network timeout, over a
# stalled link a busy pass waits out one timeout, not esp_mqtt's 10 s, and a
# pass never waits behind the CONNECTED burst esp_mqtt sends under its lock.
# Also the health publish's event-id space warning (sweep F82), against a
# stubbed allocator, and the egress counters' retained topic (sweep F149),
# against a stubbed csi_event_egress::stats().
MQTT_REINIT_BIN = test_mqtt_reinit
MQTT_REINIT_SRC = test_mqtt_reinit.cpp

$(MQTT_REINIT_BIN): $(MQTT_REINIT_SRC) $(SRC_DIR)/csi_mqtt.cpp $(SRC_DIR)/csi_mqtt.h $(SRC_DIR)/csi_module_settings_nvs.h \
                  $(SRC_DIR)/csi_event_egress.h \
                  $(wildcard stubs/mqtt/*.h stubs/mqtt/freertos/*.h)
	$(CXX) $(CXXFLAGS) -Werror -Wno-comment -Wno-ignored-qualifiers -Wno-unused-function -Wno-stringop-truncation \
	  -I stubs/mqtt -I $(SRC_DIR) -o $@ $(MQTT_REINIT_SRC) $(SRC_DIR)/csi_mqtt.cpp

.PHONY: run-mqtt-reinit clean-mqtt-reinit
run-mqtt-reinit: $(MQTT_REINIT_BIN)
	$(call run_marked,$(MQTT_REINIT_BIN))
clean-mqtt-reinit:
	rm -f $(MQTT_REINIT_BIN)

run: run-mqtt-reinit
clean: clean-mqtt-reinit

# The hand-over from a requesting task to the loop task (sweep F96):
# the REAL loop_command_ring.h. Commands run in post order across the ticket
# wrap, a full ring refuses, each waiter collects its own result once, a
# command not started by the timeout is withdrawn and never runs, one already
# started is waited for, and with real threads (six requesters, one loop
# thread) every command runs once, on the loop thread, and with a one-tick
# timeout no withdrawn command runs and no slot is lost while withdrawals
# race the drain. `make tsan-loop-ring` builds and runs the same suite under
# -fsanitize=thread (not part of `run`: it needs a sanitizer runtime).
LOOP_RING_BIN = test_loop_command_ring
LOOP_RING_SRC = test_loop_command_ring.cpp

$(LOOP_RING_BIN): $(LOOP_RING_SRC) $(SRC_DIR)/loop_command_ring.h
	$(CXX) $(CXXFLAGS) -Werror -pthread -I $(SRC_DIR) -o $@ $(LOOP_RING_SRC)

.PHONY: run-loop-ring clean-loop-ring tsan-loop-ring
run-loop-ring: $(LOOP_RING_BIN)
	$(call run_marked,$(LOOP_RING_BIN))
tsan-loop-ring: $(LOOP_RING_SRC) $(SRC_DIR)/loop_command_ring.h
	$(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread -I $(SRC_DIR) -o $(LOOP_RING_BIN)_tsan $(LOOP_RING_SRC)
	./$(LOOP_RING_BIN)_tsan
clean-loop-ring:
	rm -f $(LOOP_RING_BIN) $(LOOP_RING_BIN)_tsan

run: run-loop-ring
clean: clean-loop-ring

# What the loop task publishes for other tasks to read whole (sweep F110):
# the REAL loop_snapshot.h, which the mesh status routes read through. A
# Value read is the last publish, whole, and the same bytes take no lock; a
# Log read is one moment's records in storage order, and starts over when
# the loop task changes the log between two of its copies (driven
# deterministically by a lock that runs the loop task's step there), or ends
# with one copy under one hold; with real threads (one writer, four
# readers) no read is torn. `make tsan-loop-snapshot` runs it under
# -fsanitize=thread (not part of `run`: it needs a sanitizer runtime).
LOOP_SNAP_BIN = test_loop_snapshot
LOOP_SNAP_SRC = test_loop_snapshot.cpp

$(LOOP_SNAP_BIN): $(LOOP_SNAP_SRC) $(SRC_DIR)/loop_snapshot.h
	$(CXX) $(CXXFLAGS) -Werror -pthread -I $(SRC_DIR) -o $@ $(LOOP_SNAP_SRC)

.PHONY: run-loop-snapshot clean-loop-snapshot tsan-loop-snapshot
run-loop-snapshot: $(LOOP_SNAP_BIN)
	$(call run_marked,$(LOOP_SNAP_BIN))
tsan-loop-snapshot: $(LOOP_SNAP_SRC) $(SRC_DIR)/loop_snapshot.h
	$(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread -I $(SRC_DIR) -o $(LOOP_SNAP_BIN)_tsan $(LOOP_SNAP_SRC)
	./$(LOOP_SNAP_BIN)_tsan
clean-loop-snapshot:
	rm -f $(LOOP_SNAP_BIN) $(LOOP_SNAP_BIN)_tsan

run: run-loop-snapshot
clean: clean-loop-snapshot

# What another task reports, handed to the loop task to apply (sweep F143):
# the REAL loop_event_queue.h, which the NimBLE host task's Bluetooth
# callbacks post through. Post order is apply order across the wrap; a full
# queue refuses and counts; a post under a lower limit (a scan result)
# leaves the reserve to the rest (a link up or down, a passkey); consume()
# applies what waited when it started, with no lock held; with real threads
# (three producers, one consumer) every event is applied once or counted,
# in each producer's order. `make tsan-loop-events` runs it under
# -fsanitize=thread (not part of `run`: it needs a sanitizer runtime).
LOOP_EV_BIN = test_loop_event_queue
LOOP_EV_SRC = test_loop_event_queue.cpp

$(LOOP_EV_BIN): $(LOOP_EV_SRC) $(SRC_DIR)/loop_event_queue.h
	$(CXX) $(CXXFLAGS) -Werror -pthread -I $(SRC_DIR) -o $@ $(LOOP_EV_SRC)

.PHONY: run-loop-events clean-loop-events tsan-loop-events
run-loop-events: $(LOOP_EV_BIN)
	$(call run_marked,$(LOOP_EV_BIN))
tsan-loop-events: $(LOOP_EV_SRC) $(SRC_DIR)/loop_event_queue.h
	$(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread -I $(SRC_DIR) -o $(LOOP_EV_BIN)_tsan $(LOOP_EV_SRC)
	./$(LOOP_EV_BIN)_tsan
clean-loop-events:
	rm -f $(LOOP_EV_BIN) $(LOOP_EV_BIN)_tsan

run: run-loop-events
clean: clean-loop-events

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
# recursive mutex, and runs the canary's scenarios on the WAP's code plus the
# RAII NvsMainSession guard (F60) and the nvs_store:: helpers; it also reads
# canary_wap.ino for the watchdog static_assert. Built twice: the _nolock
# binary is the first instance() meeting a failed mutex creation.
# test_nvs_session_balance reads every sketch source and fails on one that
# names NvsManager outside nvs_store.h — the rule that keeps every session
# behind the guard (its first edition balanced begin/end pairs textually;
# the guard's destructor made the compiler walk the paths instead). Hooked
# into `run` and `clean` by prerequisite so it adds no hunk to their lists.
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
