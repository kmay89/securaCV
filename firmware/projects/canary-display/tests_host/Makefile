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

# $(call run_marked,<suite>) runs ./<suite> and holds it to the check
# firmware.yml's old inline steps made: a non-zero exit fails, and so does
# output without the suite's "ALL ... PASSED" line, so a suite that returns 0
# before its last check cannot pass silently. It echoes only the suite's
# command line, like the plain `./suite` lines.
run_marked = @echo "./$(1)"; out=$$(./$(1)); s=$$?; printf '%s\n' "$$out"; \
  [ $$s -eq 0 ] || { echo "$(1): exit $$s" >&2; exit 1; }; \
  printf '%s\n' "$$out" | grep -q "ALL .* PASSED" || \
  { echo "$(1): no 'ALL .* PASSED' line in its output" >&2; exit 1; }

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
# forward render rotation, the LVGL 9 pointer feed inverted against LVGL's
# own indev rotation) and the rendered brightness scrim (clamp + the
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
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(MODE_SRC)

$(DEMO_BIN): $(DEMO_SOURCES)
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(DEMO_SOURCES)

$(ARCADE_BIN): $(ARCADE_SRC)
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(ARCADE_SRC)

$(MIC_BIN): $(MIC_SRC)
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(MIC_SRC)

$(VOICE_BIN): $(VOICE_SRC)
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(VOICE_SRC)

$(CLEAR_BIN): $(CLEAR_SRC)
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(CLEAR_SRC)

$(RTC_BIN): $(RTC_SRC)
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(RTC_SRC)

$(RTC85063_BIN): $(RTC85063_SRC)
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(RTC85063_SRC)

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

# The glass help QR's verdict (canary/ui/help_verdict.h): worst-first
# precedence (verify failure > hub down > quiet witness > bare Help Desk),
# the overflow-refusal compose, and the website #s- anchor charset contract.
# Pure header, no LVGL.
HELPV_BIN = test_help_verdict
HELPV_SRC = test_help_verdict.cpp

$(HELPV_BIN): $(HELPV_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(HELPV_SRC)

# Round Frame geometry engine (canary/ui/round_frame_core.h): the chord-at-
# latitude math every fitted watch label rides, equator-centered row stacks,
# the inscribed QR square, and the polar helpers. Pure integers, no LVGL.
ROUNDFRAME_BIN = test_round_frame_core
ROUNDFRAME_SRC = test_round_frame_core.cpp

$(ROUNDFRAME_BIN): $(ROUNDFRAME_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(ROUNDFRAME_SRC)

# The engine's C++11 contract — built with -std=gnu++11 ON PURPOSE (the
# Arduino core-2.0.17 rows compile the flat sketch at that standard, where
# a constexpr body is a single return statement). A cleanup that reintroduces
# loops into the header's constexpr functions passes every C++17 build and
# fails only in Arduino CI; this target fails first, locally, and says why.
ROUNDFRAME11_BIN = test_round_frame_cpp11
ROUNDFRAME11_SRC = test_round_frame_cpp11.cpp

$(ROUNDFRAME11_BIN): $(ROUNDFRAME11_SRC)
	$(CXX) -std=gnu++11 -O2 -Wall -Wextra $(INC) -o $@ $(ROUNDFRAME11_SRC)

# The first-boot Join scene's stack (canary/ui/onboard_layout.h): on every
# display env's panel (the ini -> each board's pins.h) with both type ladders
# (parsed from character.cpp), the title, QR card and caption lines never
# cross or leave the glass — F43, the dash's "or join ... password" line drawn
# inside the QR card — and what those rows say is never cut to an ellipsis
# for any network name or key the unit can mint (F45), measured with LVGL's
# own glyph metrics (montserrat_metrics.h, generated by
# firmware/scripts/gen_montserrat_metrics.py). Reads the tree through FW_DIR;
# built at gnu++11 like the Round Frame contract, since the core-2.0.17
# sketch compiles the header there.
ONBOARD_BIN = test_onboard_layout
ONBOARD_SRC = test_onboard_layout.cpp

$(ONBOARD_BIN): $(ONBOARD_SRC) ../include/canary/ui/onboard_layout.h montserrat_metrics.h
	$(CXX) -std=gnu++11 -O2 -Wall -Wextra $(INC) -I ../../../common \
	  '-DFW_DIR="$(abspath ../../..)"' -o $@ $(ONBOARD_SRC)

# The LAN write policy (canary/net/settings_policy.h): the /api/set keys the
# display refuses from the network for every caller — the standalone-weather
# opt-in (wx_direct, the glass's one opt-in outbound path) and its coarse
# location (wx_loc) — so a host on the home WiFi cannot flip zero-phone-home.
# Pins every egress/location key true, every ordinary knob false, unknown and
# near-miss keys false, the lookup's table-constant return, and the served
# table == the predicate. Pure header, no Arduino.
POLICY_BIN = test_settings_policy
POLICY_SRC = test_settings_policy.cpp

$(POLICY_BIN): $(POLICY_SRC)
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(POLICY_SRC)

run:$(BEACON_PARSE_BIN) $(MODEL_BIN) $(CARDS_BIN) $(MODE_BIN) $(DEMO_BIN) $(ARCADE_BIN) $(MIC_BIN) $(VOICE_BIN) $(CLEAR_BIN) $(RTC_BIN) $(RTC85063_BIN) $(BEDSIDE_BIN) $(NIGHTLIGHT_BIN) $(ORIENT_BIN) $(DISPSET_BIN) $(CLOCKSTYLE_BIN) $(GREET_BIN) $(HELPV_BIN) $(MOTION_BIN) $(ROUNDFRAME_BIN) $(ROUNDFRAME11_BIN) $(ONBOARD_BIN) $(POLICY_BIN)
	./$(BEACON_PARSE_BIN)
	./$(MODEL_BIN)
	./$(CARDS_BIN)
	$(call run_marked,$(MODE_BIN))
	$(call run_marked,$(DEMO_BIN))
	$(call run_marked,$(ARCADE_BIN))
	$(call run_marked,$(MIC_BIN))
	$(call run_marked,$(VOICE_BIN))
	$(call run_marked,$(CLEAR_BIN))
	$(call run_marked,$(RTC_BIN))
	$(call run_marked,$(RTC85063_BIN))
	./$(BEDSIDE_BIN)
	./$(NIGHTLIGHT_BIN)
	./$(ORIENT_BIN)
	./$(DISPSET_BIN)
	./$(CLOCKSTYLE_BIN)
	./$(GREET_BIN)
	./$(HELPV_BIN)
	./$(MOTION_BIN)
	./$(ROUNDFRAME_BIN)
	./$(ROUNDFRAME11_BIN)
	./$(ONBOARD_BIN)
	$(call run_marked,$(POLICY_BIN))

clean:
	rm -f $(BEACON_PARSE_BIN) $(MODEL_BIN) $(CARDS_BIN) $(MODE_BIN) $(DEMO_BIN) $(ARCADE_BIN) $(MIC_BIN) $(VOICE_BIN) $(CLEAR_BIN) $(RTC_BIN) $(RTC85063_BIN) $(BEDSIDE_BIN) $(NIGHTLIGHT_BIN) $(ORIENT_BIN) $(DISPSET_BIN) $(CLOCKSTYLE_BIN) $(GREET_BIN) $(HELPV_BIN) $(MOTION_BIN) $(ROUNDFRAME_BIN) $(ROUNDFRAME11_BIN) $(ONBOARD_BIN) $(POLICY_BIN)

# Sweep CI3, one host-test list: the five suites below were built only by
# their own inline g++ steps in firmware.yml's "Mesh + Scout Host Tests" job,
# so `make` here never ran them and the two lists could drift. Each rule keeps
# its step's -Werror and its "ALL ... PASSED" check (run_marked, above);
# scripts/tests/test_host_test_lists.py fails a suite that is back on a
# workflow's list only. Pure cores, no board. Hooked into `run` and `clean`
# by prerequisite so it adds no hunk to their lists.

# The RS485/Modbus master's framing and CRC-16/MODBUS (canary/io/modbus_rtu.h),
# so every byte on the 4.3B A/B terminal is host-verified without a board.
# Anchored on the cataloged CRC-16/MODBUS check value 0x4B37 plus build/parse
# roundtrips, exception decoding, and CRC/addr/func/capacity rejection cases.
MODBUS_BIN = test_modbus_rtu
MODBUS_SRC = test_modbus_rtu.cpp

# The CAN 2.0 frame model (canary/io/can_frame.h): id-width/DLC validity,
# SocketCAN-style acceptance filtering, and the ASCII log formatter. The TWAI
# transport itself is compile-checked by the canary-display-dash-can
# PlatformIO env.
CAN_BIN = test_can_frame
CAN_SRC = test_can_frame.cpp

# The isolated DI/DO promotion's two testable decisions
# (canary/io/field_io_logic.h): input debounce (reject contact bounce and bus
# glitches) and the bounded siren controller (drive only while a real unacked
# alert stands, capped so it can't blare forever). The runtime that reads the
# CH422G and reports UNSIGNED local contact events is compile-checked by the
# canary-display-dash-b env (HAS_ISOLATED_IO).
FIELDIO_BIN = test_field_io
FIELDIO_SRC = test_field_io.cpp

# Router-independent peer presence (canary/net/espnow_peer_logic.h): the
# decode that turns a raw ESP-NOW payload into a fleet-link beacon (fp4 +
# status) reuses the shared, host-tested beacon wire contract
# (canary/net/beacon_parse.h), so an ESP-NOW frame and a BLE advert from one
# canary resolve to one witness, and foreign traffic and noise are rejected
# before the fleet. The esp_now transport is compile-checked by the
# canary-display-dash-espnow env.
ESPNOW_BIN = test_espnow_peer
ESPNOW_SRC = test_espnow_peer.cpp

# The First Light pair demo's pure core (canary/pair/pair_demo.h):
# adopt-first-heard + the lock (the card can never silently switch cameras),
# the detection-edge rule mirrored from the sender's beacon payload, the
# fleet-aligned staleness read, the 5 s HoldGate open gesture, and the
# boxed-pair auto-open rule (unprovisioned + router-free band ONLY: a working
# bedside clock never swaps its face). The LVGL/NVS shell is compile-checked
# by the canary-display-nightlight-c3 env.
PAIRDEMO_BIN = test_pair_demo
PAIRDEMO_SRC = test_pair_demo.cpp

IO_CORE_BINS = $(MODBUS_BIN) $(CAN_BIN) $(FIELDIO_BIN) $(ESPNOW_BIN) $(PAIRDEMO_BIN)

$(MODBUS_BIN): $(MODBUS_SRC) ../include/canary/io/modbus_rtu.h
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(MODBUS_SRC)

$(CAN_BIN): $(CAN_SRC) ../include/canary/io/can_frame.h
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(CAN_SRC)

$(FIELDIO_BIN): $(FIELDIO_SRC) ../include/canary/io/field_io_logic.h
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(FIELDIO_SRC)

$(ESPNOW_BIN): $(ESPNOW_SRC) ../include/canary/net/espnow_peer_logic.h
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(ESPNOW_SRC)

$(PAIRDEMO_BIN): $(PAIRDEMO_SRC) ../include/canary/pair/pair_demo.h
	$(CXX) $(CXXFLAGS) -Werror $(INC) -o $@ $(PAIRDEMO_SRC)

.PHONY: run-io-cores clean-io-cores
run-io-cores: $(IO_CORE_BINS)
	$(call run_marked,$(MODBUS_BIN))
	$(call run_marked,$(CAN_BIN))
	$(call run_marked,$(FIELDIO_BIN))
	$(call run_marked,$(ESPNOW_BIN))
	$(call run_marked,$(PAIRDEMO_BIN))
clean-io-cores:
	rm -f $(IO_CORE_BINS)

run: run-io-cores
clean: clean-io-cores
