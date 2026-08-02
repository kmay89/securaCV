# Host-side unit tests for the Canary Companion — the Night Watch bedside clock
# and the Pocket Canary (docs/design/canary_companion.md).
#
# Everything here builds with plain g++: no Arduino, no LVGL, no panel driver,
# no I²C. That is the same posture the Tin Can took and for the same reason —
# the decisions that are expensive to get wrong do not need the hardware:
#
#   * a pet that can die, or that scores absence against a child
#   * an "ask" that quietly becomes a notification
#   * a night screen that goes dark for trouble as readily as for wellness
#   * a wake-on-raise that fires when a sleeper rolls over
#   * a voice that claims it buzzed on a board with no motor fitted
#   * a settings mis-tap that wipes a device
#
# Every one of those is decidable here, in CI, and every one has a test named
# after the mistake rather than after the function.
#
# Run:
#   make            # build + run
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror

INC_COMPANION = -I ../include

BIN = test_companion_cores
SRC = test_companion_cores.cpp

all: run

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) $(INC_COMPANION) -o $@ $(SRC)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all run clean
