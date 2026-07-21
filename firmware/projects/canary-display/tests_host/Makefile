# Host-side unit tests for canary-display modules that have no ESP32/NimBLE
# dependency — the pure wire-format + fleet-model logic behind the direct BLE
# "fleet link" (fleet_beacon.h on the WAP builds the identical layout these
# parse). The canonical include tree (include/) supplies the canary/... paths.
#
# Run:
#   make            # build + run both
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
INC       = -I ../include

# Fleet-link presence beacon parser (canary/net/beacon_parse.h): fp4 hex
# encode, status decode (flags/battery/health/LE chain), and rejection of
# short/oversize/wrong-company/wrong-type/wrong-version blobs.
BEACON_PARSE_BIN = test_beacon_parse
BEACON_PARSE_SRC = test_beacon_parse.cpp

# FleetModel::on_beacon ingest: suffix-match vs pseudo-create, seen_via_ble +
# liveness, stored battery/health/chain, badge-untouched, tamper 60 s dedupe.
# Links the model's non-template piece (classify_event/sev_name).
MODEL_BIN     = test_fleet_beacon_model
MODEL_SOURCES = test_fleet_beacon_model.cpp ../src/fleet/fleet_model.cpp

# Canary Cards firmware builder (canary/fleet/fleet_cards.h): the type-aware
# widget layer the glass renders for a canary-sense witness. Pinned to the
# schema (docs/standard/CANARY_CARDS.md) and to parity with the JS reference
# renderer (canary-local/assets/canary-cards.js). Pure logic, no LVGL.
CARDS_BIN     = test_fleet_cards
CARDS_SOURCES = test_fleet_cards.cpp ../src/fleet/fleet_cards.cpp

.PHONY: all run clean
all: run

$(BEACON_PARSE_BIN): $(BEACON_PARSE_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(BEACON_PARSE_SRC)

$(MODEL_BIN): $(MODEL_SOURCES)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(MODEL_SOURCES)

$(CARDS_BIN): $(CARDS_SOURCES)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(CARDS_SOURCES)

run: $(BEACON_PARSE_BIN) $(MODEL_BIN) $(CARDS_BIN)
	./$(BEACON_PARSE_BIN)
	./$(MODEL_BIN)
	./$(CARDS_BIN)

clean:
	rm -f $(BEACON_PARSE_BIN) $(MODEL_BIN) $(CARDS_BIN)
