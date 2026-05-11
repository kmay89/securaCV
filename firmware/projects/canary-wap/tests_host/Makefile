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

.PHONY: all run clean
all: run

$(BIN): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
