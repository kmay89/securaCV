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
# widget layer the glass renders for canary-sense + canary-pool witnesses.
# Pinned to the schema (docs/standard/CANARY_CARDS.md) and to parity with the
# JS reference renderer (canary-local/assets/canary-cards.js). Pure logic, no
# LVGL. Links fleet_model.cpp too so the pool section can drift-lock its
# threshold events against the real classify_event().
CARDS_BIN     = test_fleet_cards
CARDS_SOURCES = test_fleet_cards.cpp ../src/fleet/fleet_cards.cpp ../src/fleet/fleet_model.cpp

# Mode registry (canary/mode/mode_registry.h): boot-gear resolution (NVS token
# + legacy devmode migration), the capability gate, and the per-mode policy
# invariants (fail-safe to Fleet; every non-fleet gear has an exit + banner).
MODE_BIN = test_mode_registry
MODE_SRC = test_mode_registry.cpp

# Demo storyline (canary/mode/demo_script.h): links the REAL fleet_model.cpp so
# every scripted beat's severity is drift-locked to classify_event(), plus the
# loop-stepping (fires-once-per-lap, wrap-safe) invariants.
DEMO_BIN     = test_demo_script
DEMO_SOURCES = test_demo_script.cpp ../src/fleet/fleet_model.cpp

# Arcade QA core (canary/mode/arcade_logic.h): the seeded every-zone-once
# shuffle (factory-replayable), in-cell target placement, and the pass/fail
# verdict (dead zone / slow zone / stray tap all fail the panel).
ARCADE_BIN = test_arcade_logic
ARCADE_SRC = test_arcade_logic.cpp

# Mic decision core (canary/io/mic_logic.h — the 4.3C): the listening gate
# (off by default, unset pins hard-block, indicator == driver bit) and the
# alarm-cadence detector (smoke T3 / CO T4 need two on-grammar cycles;
# doorbells/speech/stale streaks never alarm).
MIC_BIN = test_mic_logic
MIC_SRC = test_mic_logic.cpp

# Canary Voice acoustic core (canary/hal/voice_score.h): the pure score +
# audio math behind the sound engine — palette completeness, pentatonic
# membership, the frozen IEC alarm grammar, the rise/fall contours, envelope
# de-click, glissando/warble bounds, and the volume model (monotone, night
# silencing, the Tier-1 always-audible floor).
VOICE_BIN = test_voice_score
VOICE_SRC = test_voice_score.cpp

# Acoustic clearance (canary/hal/voice_score.h): no signature reproduces a
# regulated life-safety cadence (ISO 8201 T3 fire / T4 CO) — a status chirp
# must never be confusable with a real alarm — plus the originality invariants
# that back docs/hardware/display_sound_clearance.md.
CLEAR_BIN = test_voice_clearance
CLEAR_SRC = test_voice_clearance.cpp

# PCF8563/PCF85063A RTC core (canary/io/rtc_pcf.h): epoch<->civil inversion vs
# libc, the 7-byte BCD register round-trip, VL/OS "unreliable" rejection, and
# the per-chip seconds-register base (0x02 vs 0x04) — the guard against a
# mis-based read feeding shifted garbage into a signed timestamp. The second TU
# compiles the core WITH RTC_CHIP_PCF85063 to prove the base flips to 0x04.
RTC_BIN = test_rtc_pcf
RTC_SRC = test_rtc_pcf.cpp
RTC85063_BIN = test_rtc_pcf85063
RTC85063_SRC = test_rtc_pcf85063.cpp

# Bedside models (canary/care/lantern.h, canary/care/ambient_life.h,
# canary/io/boot_button.h): the Nightstand 7 wave's pure logic — the honest
# night light (window timeout, the attention veto that takes the glass back
# and does NOT resume, the opt-in auto schedule), the rationed organic
# check-ins (cadence bounds, the idle/lit gate, per-device determinism), and
# the BOOT-button short/double/long grammar.
BEDSIDE_BIN = test_bedside_models
BEDSIDE_SRC = test_bedside_models.cpp

# Nightlight companion visits (canary/care/nightlight.h): the allowed gate
# (a visit can't begin over an alarm and ends the moment one arrives), the
# hour->theater mapping, night = still + sparse (Moonwatch only), the
# clockless fallback, determinism, and the no-severity-words caption rule.
NIGHTLIGHT_BIN = test_nightlight
NIGHTLIGHT_SRC = test_nightlight.cpp

# Gravity-settled orientation (canary/io/orientation.h): settled-only
# commits (shakes/carries have no opinion), flat/diagonal keep the last
# orientation, dwell + cooldown, all four orientations.
ORIENT_BIN = test_orientation
ORIENT_SRC = test_orientation.cpp

# Display settings geometry (canary/glass_settings.h): the 7"/dash orientation
# helpers (logical dims, the touch un-rotation round-tripped against the
# forward render rotation) and the rendered brightness scrim (clamp + the
# monotone opacity curve). Pure inline logic — no LVGL, no board.
DISPSET_BIN = test_display_settings
DISPSET_SRC = test_display_settings.cpp

.PHONY: all run clean
all: run

$(BEACON_PARSE_BIN): $(BEACON_PARSE_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(BEACON_PARSE_SRC)

$(MODEL_BIN): $(MODEL_SOURCES)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(MODEL_SOURCES)

$(CARDS_BIN): $(CARDS_SOURCES)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(CARDS_SOURCES)

$(MODE_BIN): $(MODE_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(MODE_SRC)

$(DEMO_BIN): $(DEMO_SOURCES)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(DEMO_SOURCES)

$(ARCADE_BIN): $(ARCADE_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(ARCADE_SRC)

$(MIC_BIN): $(MIC_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(MIC_SRC)

$(VOICE_BIN): $(VOICE_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(VOICE_SRC)

$(CLEAR_BIN): $(CLEAR_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(CLEAR_SRC)

$(RTC_BIN): $(RTC_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(RTC_SRC)

$(RTC85063_BIN): $(RTC85063_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(RTC85063_SRC)

$(BEDSIDE_BIN): $(BEDSIDE_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(BEDSIDE_SRC)

$(NIGHTLIGHT_BIN): $(NIGHTLIGHT_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(NIGHTLIGHT_SRC)

$(ORIENT_BIN): $(ORIENT_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(ORIENT_SRC)

$(DISPSET_BIN): $(DISPSET_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(DISPSET_SRC)

# Clock-face ring (canary/ui/clock_styles.h): the shared 12-hour conversion
# (0 -> 12 AM, 12 -> 12 PM), wrap-safe style stepping, the segment-geometry
# table, and the glyph-range guard on every caption. Pure header, no LVGL.
CLOCKSTYLE_BIN = test_clock_styles
CLOCKSTYLE_SRC = test_clock_styles.cpp

$(CLOCKSTYLE_BIN): $(CLOCKSTYLE_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(CLOCKSTYLE_SRC)

# The first-meet story (canary/care/greeting.h): the beat arc and its dwell,
# the first-meet-only rule, the abort when trouble arrives mid-story, and the
# copy itself -- including the mechanical check that no beat ever words an
# alarm (the Voice rule) or leaves plain ASCII.
GREET_BIN = test_greeting
GREET_SRC = test_greeting.cpp

$(GREET_BIN): $(GREET_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(GREET_SRC)

# The motion engine's pure half (canary/ui/motion_core.h): the capability
# model against the real board rows, tier derivation, easing curves, the
# frame-time governor's degrade/recover hysteresis, and the weather-scene
# particle math (determinism, bounds, budgets). Pure header, no LVGL.
MOTION_BIN = test_motion
MOTION_SRC = test_motion.cpp

$(MOTION_BIN): $(MOTION_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(MOTION_SRC)

run: $(BEACON_PARSE_BIN) $(MODEL_BIN) $(CARDS_BIN) $(MODE_BIN) $(DEMO_BIN) $(ARCADE_BIN) $(MIC_BIN) $(VOICE_BIN) $(CLEAR_BIN) $(RTC_BIN) $(RTC85063_BIN) $(BEDSIDE_BIN) $(NIGHTLIGHT_BIN) $(ORIENT_BIN) $(DISPSET_BIN) $(CLOCKSTYLE_BIN) $(GREET_BIN) $(MOTION_BIN)
	./$(BEACON_PARSE_BIN)
	./$(MODEL_BIN)
	./$(CARDS_BIN)
	./$(MODE_BIN)
	./$(DEMO_BIN)
	./$(ARCADE_BIN)
	./$(MIC_BIN)
	./$(VOICE_BIN)
	./$(CLEAR_BIN)
	./$(RTC_BIN)
	./$(RTC85063_BIN)
	./$(BEDSIDE_BIN)
	./$(NIGHTLIGHT_BIN)
	./$(ORIENT_BIN)
	./$(DISPSET_BIN)
	./$(CLOCKSTYLE_BIN)
	./$(GREET_BIN)
	./$(MOTION_BIN)

clean:
	rm -f $(BEACON_PARSE_BIN) $(MODEL_BIN) $(CARDS_BIN) $(MODE_BIN) $(DEMO_BIN) $(ARCADE_BIN) $(MIC_BIN) $(VOICE_BIN) $(CLEAR_BIN) $(RTC_BIN) $(RTC85063_BIN) $(BEDSIDE_BIN) $(NIGHTLIGHT_BIN) $(ORIENT_BIN) $(DISPSET_BIN) $(CLOCKSTYLE_BIN) $(GREET_BIN) $(MOTION_BIN)
