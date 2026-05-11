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

$(OPERA_SEC_BIN): $(OPERA_SEC_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(OPERA_SEC_SRC)

$(DEVSIG_BIN): $(DEVSIG_SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(DEVSIG_SOURCES)

run: $(BIN) $(CHIRP_INV_BIN) $(CHIRP_SEC_BIN) $(BEACON_ORIG_BIN) $(OPERA_SEC_BIN) $(DEVSIG_BIN)
	./$(BIN)
	./$(CHIRP_INV_BIN)
	./$(CHIRP_SEC_BIN)
	./$(BEACON_ORIG_BIN)
	./$(OPERA_SEC_BIN)
	./$(DEVSIG_BIN)

clean:
	rm -f $(BIN) $(CHIRP_INV_BIN) $(CHIRP_SEC_BIN) $(BEACON_ORIG_BIN) $(OPERA_SEC_BIN) $(DEVSIG_BIN)
