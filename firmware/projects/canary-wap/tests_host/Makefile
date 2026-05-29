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

.PHONY: all run clean
all: run

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

run: $(BIN) $(CHIRP_INV_BIN) $(CHIRP_SEC_BIN) $(BEACON_ORIG_BIN) $(BEACON_SOLO_BIN) $(BEACON_TAMPER_BIN) $(OPERA_SEC_BIN) $(DEVSIG_BIN) $(CAPTIVE_DNS_BIN) $(CAPTIVE_PROBE_BIN)
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

clean:
	rm -f $(BIN) $(CHIRP_INV_BIN) $(CHIRP_SEC_BIN) $(BEACON_ORIG_BIN) $(BEACON_SOLO_BIN) $(BEACON_TAMPER_BIN) $(OPERA_SEC_BIN) $(DEVSIG_BIN) $(CAPTIVE_DNS_BIN) $(CAPTIVE_PROBE_BIN)
