# Host-side unit tests for board-agnostic firmware/common modules that have no
# ESP32 dependency. These modules keep their core logic in plain hosted C++ so
# it can be exercised without the Arduino toolchain.
#
# Run:
#   make            # build + run every suite (presence-only and vitals builds)
#   make clean
#
# Mirrors the compiler contract the "Mesh + Scout Host Tests" CI job uses for
# the other common/ suites: g++ -std=c++17 with -Wall -Wextra -Werror.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror

MR60_DIR = ../common/sensors/mmwave_mr60
MR60_SRC = $(MR60_DIR)/mr60_uart.cpp \
           $(MR60_DIR)/mr60_presence.cpp \
           $(MR60_DIR)/mr60_vitals.cpp

# MR60BHA2 UART frame decoder + presence/vitals FSM integration. Built twice:
# once presence-only (vitals TU compiles to nothing), once with the vitals
# build flag so the vitals decode path and FSM are compile- and run-tested.
# Binaries live in an ignored build/ directory (never in the source tree, so
# CI always proves the sources compile) and depend on the module headers AND
# this Makefile, so flag changes rebuild too.
BUILD_DIR       = build
MR60_TEST_SRC   = test_mr60_uart.cpp
MR60_HDRS       = $(wildcard $(MR60_DIR)/*.h)
MR60_BIN        = $(BUILD_DIR)/test_mr60_uart
MR60_VITALS_BIN = $(BUILD_DIR)/test_mr60_uart_vitals

# Shared device_signature module (common/identity): canonical-message
# builders + b64url encoder compile hosted (the Ed25519 sign path is
# ARDUINO-gated). Locks the wire format the canary-sense witness signs
# and HA's signature.py verifies.
DEVSIG_DIR = ../common/identity
DEVSIG_SRC = $(DEVSIG_DIR)/device_signature.cpp
DEVSIG_BIN = $(BUILD_DIR)/test_device_signature_common

# Canonical witness-chain construction (common/witness/witness_chain.h):
# pins the 72-byte chain pre-image layout, domain strings, and
# python-hashlib golden hashes shared with tools/verify_witness_log.py.
# Uses OpenSSL's SHA-256 on the host (libcrypto is preinstalled on
# ubuntu-latest); SHA256_Init & co. are deprecated in OpenSSL 3 but fine
# for a test, hence the -Wno flag under -Werror.
WITCHAIN_HDR = ../common/witness/witness_chain.h
WITCHAIN_BIN = $(BUILD_DIR)/test_witness_chain_core

# Canonical witness-store pure logic (common/witness/witness_store.h):
# the /WITNESS/records.jsonl byte-exact line format + SD-wins boot
# reconciliation, consumed by the PIO canary tree directly and by the
# canary-wap sketch via a byte-identical staged copy (check_csi_sync.sh).
# Reuses the canary-wap host suite, compiled here against the CANONICAL
# header so the suite exercises the copy the PIO canary actually builds.
WITSTORE_HDR  = ../common/witness/witness_store.h
WITSTORE_TEST = ../projects/canary-wap/tests_host/test_witness_store_logic.cpp
WITSTORE_BIN  = $(BUILD_DIR)/test_witness_store_common

.PHONY: all run clean
all: run

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(MR60_BIN): $(MR60_TEST_SRC) $(MR60_SRC) $(MR60_HDRS) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I $(MR60_DIR) $(MR60_TEST_SRC) $(MR60_SRC) -o $@

$(MR60_VITALS_BIN): $(MR60_TEST_SRC) $(MR60_SRC) $(MR60_HDRS) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -DCANARY_SENSE_VITALS=1 -I $(MR60_DIR) $(MR60_TEST_SRC) $(MR60_SRC) -o $@

$(DEVSIG_BIN): test_device_signature_common.cpp $(DEVSIG_SRC) $(DEVSIG_DIR)/device_signature.h Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I $(DEVSIG_DIR) test_device_signature_common.cpp $(DEVSIG_SRC) -o $@

$(WITCHAIN_BIN): test_witness_chain_core.cpp $(WITCHAIN_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Wno-deprecated-declarations -I ../common test_witness_chain_core.cpp -lcrypto -o $@

$(WITSTORE_BIN): $(WITSTORE_TEST) $(WITSTORE_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Wno-deprecated-declarations -I ../common/witness $(WITSTORE_TEST) -lcrypto -o $@

run: $(MR60_BIN) $(MR60_VITALS_BIN) $(DEVSIG_BIN) $(WITCHAIN_BIN) $(WITSTORE_BIN)
	./$(MR60_BIN)
	./$(MR60_VITALS_BIN)
	./$(DEVSIG_BIN)
	./$(WITCHAIN_BIN)
	./$(WITSTORE_BIN)

clean:
	rm -rf $(BUILD_DIR)
