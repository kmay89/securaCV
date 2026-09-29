# Host-side unit tests for the Tin Can — the kids' wrist Canary
# (docs/design/canary_tincan_kids_watch.md).
#
# Everything here builds with plain g++: no Arduino, no LVGL, no radio, no
# crypto library. That is deliberate. The board does not exist on anyone's desk
# yet, and the parts of this design that are easy to get catastrophically wrong
# — nonce uniqueness across both directions of a string, a send counter that
# rewinds after a reboot, a revocation that quietly un-revokes, a Ring that
# claims it was delivered when it wasn't — are all decidable without hardware.
# So they are decided here, in CI, before a single wrist wears one.
#
# Run:
#   make            # build + run both suites
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror

# $(call run_marked,<suite>) runs ./<suite> and holds it to the check
# firmware.yml's old inline steps made: a non-zero exit fails, and so does
# output without the suite's "ALL ... PASSED" line, so a suite that returns 0
# before its last check cannot pass silently. Its output passes through
# tee as the suite writes it, instead of being held until it exits, and
# its exit status comes back through a file, because make's /bin/sh has
# no pipefail. It echoes only the suite's command line, like
# a plain `./suite` line.
run_marked = @echo "./$(1)"; t=$$(mktemp) || exit 1; \
  { ./$(1); echo $$? > "$$t.rc"; } | tee "$$t"; s=$$(cat "$$t.rc"); \
  grep -q "ALL .* PASSED" "$$t"; m=$$?; rm -f "$$t" "$$t.rc"; \
  [ "$$s" = 0 ] || { echo "$(1): exit $$s" >&2; exit 1; }; \
  [ $$m -eq 0 ] || { echo "$(1): no 'ALL .* PASSED' line in its output" >&2; exit 1; }

# The generic transport/session layer lives in firmware/common/link — it is
# deliberately NOT kid-specific, because the same machinery (many nodes, one
# radio, no broker, no cloud) is what an industrial deployment needs.
INC_LINK   = -I ../../../common/link
INC_TINCAN = -I ../include

# Canary Link core: frame layout + discrimination against the fleet presence
# beacon, the dir||ctr nonce construction, the anti-replay window, the
# send-counter lease that survives an unclean reboot, session role assignment
# and terminal revocation, node liveness/route selection, relay dedupe + ttl.
LINK_BIN = test_link_core
LINK_SRC = test_link_core.cpp

# Tin Can payload cores: the knock codec (capture, debounce, quantize, wire,
# playback), the string model (sender-side cooldown, both-holding, cut is
# terminal), the Ring's delivery honesty, the tie ceremony's parent gate and
# knot compare, the stamp table's wire contract, warmer/colder (household
# anchors only, game-scoped), and the step duel's daily reset.
TINCAN_BIN = test_tincan_cores
TINCAN_SRC = test_tincan_cores.cpp

all: run

$(LINK_BIN): $(LINK_SRC)
	$(CXX) $(CXXFLAGS) $(INC_LINK) -o $@ $(LINK_SRC)

$(TINCAN_BIN): $(TINCAN_SRC)
	$(CXX) $(CXXFLAGS) $(INC_TINCAN) -o $@ $(TINCAN_SRC)

run: $(LINK_BIN) $(TINCAN_BIN)
	$(call run_marked,$(LINK_BIN))
	$(call run_marked,$(TINCAN_BIN))

clean:
	rm -f $(LINK_BIN) $(TINCAN_BIN)

.PHONY: all run clean
