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

$(DEVSIG_BIN): $(DEVSIG_SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(DEVSIG_SOURCES)

run: $(BIN) $(DEVSIG_BIN)
	./$(BIN)
	./$(DEVSIG_BIN)

clean:
	rm -f $(BIN) $(DEVSIG_BIN)
