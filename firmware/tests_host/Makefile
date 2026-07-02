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

.PHONY: all run clean
all: run

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(MR60_BIN): $(MR60_TEST_SRC) $(MR60_SRC) $(MR60_HDRS) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I $(MR60_DIR) $(MR60_TEST_SRC) $(MR60_SRC) -o $@

$(MR60_VITALS_BIN): $(MR60_TEST_SRC) $(MR60_SRC) $(MR60_HDRS) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -DCANARY_SENSE_VITALS=1 -I $(MR60_DIR) $(MR60_TEST_SRC) $(MR60_SRC) -o $@

run: $(MR60_BIN) $(MR60_VITALS_BIN)
	./$(MR60_BIN)
	./$(MR60_VITALS_BIN)

clean:
	rm -rf $(BUILD_DIR)
