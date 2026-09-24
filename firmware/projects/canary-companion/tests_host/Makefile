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

# $(call run_marked,<suite>) runs ./<suite> and holds it to the check
# firmware.yml's old inline steps made: a non-zero exit fails, and so does
# output without the suite's "ALL ... PASSED" line, so a suite that returns 0
# before its last check cannot pass silently. It echoes only the suite's
# command line, like a plain `./suite` line.
run_marked = @echo "./$(1)"; out=$$(./$(1)); s=$$?; printf '%s\n' "$$out"; \
  [ $$s -eq 0 ] || { echo "$(1): exit $$s" >&2; exit 1; }; \
  printf '%s\n' "$$out" | grep -q "ALL .* PASSED" || \
  { echo "$(1): no 'ALL .* PASSED' line in its output" >&2; exit 1; }

INC_COMPANION = -I ../include

BIN = test_companion_cores
SRC = test_companion_cores.cpp

all: run

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) $(INC_COMPANION) -o $@ $(SRC)

run: $(BIN)
	$(call run_marked,$(BIN))

clean:
	rm -f $(BIN)

.PHONY: all run clean
