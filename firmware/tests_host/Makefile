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

# USB onboarding trust model (common/usb/usb_onboard_logic.h): the consent
# state machine (types only after an announced arm + physical confirm), the
# help-URL builder/sanitizer, and the keystroke-plan allow-list that makes the
# HID keyboard provably incapable of emitting anything but the help link.
# Pure header, no Arduino/crypto deps.
USBONB_HDR = ../common/usb/usb_onboard_logic.h
USBONB_BIN = $(BUILD_DIR)/test_usb_onboard_logic

# Serial test-console security policy (common/health/test_console.h): proves no
# command leaks secrets, production images expose only read-only diagnostics,
# mutating commands need a physical confirm — plus the BLE bring-up ladder that
# turns "Bluetooth doesn't work" into the specific rung that failed.
TESTCON_HDR = ../common/health/test_console.h
TESTCON_BIN = $(BUILD_DIR)/test_test_console

# Boot crash-loop / safe-mode policy (common/health/boot_policy.h): the pure
# decision half of "a bad image must not brick trust" — proves a good image
# never trips safe mode, an unconfirmed image defers to A/B rollback, a confirmed
# crash-loop degrades to safe mode after exactly N boots and stays there, and
# that safe mode is always escapable (never a permanent brick). Pure header.
BOOTPOL_HDR = ../common/health/boot_policy.h
BOOTPOL_BIN = $(BUILD_DIR)/test_boot_policy

# Fleet-wide Wi-Fi join/retry rules (common/network/wifi_join_policy.h): which
# failures a human can fix, how long to back off, and the rule that a link
# which has NEVER associated must never be rebooted. That last one is the
# 4-inch display's "setup loop" — boot join times out, board reboots, identical
# join fails again, forever — reproduced as a unit test so it cannot come back.
WIFIJOIN_HDR = ../common/network/wifi_join_policy.h
WIFIJOIN_BIN = $(BUILD_DIR)/test_wifi_join_policy

# Shared headless setup portal (common/network): the pure timing choreography
# (AP linger, quiet background retry, stuck-phone hint) plus the canonical
# provision_core byte-math (A-only captive DNS, probe classification, JSON
# escaping, the unambiguous password alphabet) that the sense/vision portal
# links. The display's byte-identical provision_core copy is pinned separately
# by firmware/scripts/check_provision_core_sync.sh.
PORTAL_HDRS = ../common/network/setup_portal_logic.h \
              ../common/network/provision_core.h
PORTAL_BIN  = $(BUILD_DIR)/test_setup_portal_logic

# Power-event lineage + outage log (common/power/power_events.h): the pure
# decision half of "harden against brownouts / flickers / outages and keep a
# correct log of when the power went out." Proves the whole classification table
# (cold boot vs clean reboot vs brownout vs restored outage vs fault), the honest
# lower-bound outage arithmetic, and the durable ring + monotonic counters the
# firmware persists to NVS. Pure header, no Arduino deps.
POWEREVT_HDR = ../common/power/power_events.h
POWEREVT_BIN = $(BUILD_DIR)/test_power_events

# Console scene engine (common/ui/*.h): proves the drunken-bishop randomart is a
# faithful, deterministic, bounded walk, and — the anti-fragile guarantee — that
# the ASCII tier never emits an escape or non-7-bit byte and every framed line
# aligns to the same width. Pure headers, no Arduino deps.
SCENE_HDRS = ../common/ui/randomart.h ../common/ui/console_theme.h ../common/ui/console_scenes.h ../common/ui/console_wake.h
SCENE_BIN = $(BUILD_DIR)/test_console_scene

# Fleet view (common/ui/fleet_view.h): the console face of the self-* roadmap's
# Fleet map — "which other Canaries can this one hear." Proves the ASCII tier
# stays escape-free / 7-bit / width-aligned (same contract as the trust card),
# that presence is rendered honestly as UNSIGNED with worded (not color-only)
# attention states, and pins a golden render. Pure header over the console
# engine + the fleet-link wire constants; no Arduino deps.
FLEETVIEW_HDRS = ../common/ui/fleet_view.h ../common/ui/console_theme.h \
                 ../common/fleet_link/fleet_beacon.h ../common/fleet_link/fleet_roster.h
FLEETVIEW_BIN = $(BUILD_DIR)/test_fleet_view

# Fleet self-report (common/fleet_selfreport/fleet_selfreport.h): the ONE shared
# builder for the GET /api/fleet body every networked Canary answers — the
# "parity by architecture" core (write the wire shape once, all boards serve it
# identically). Pins the DISCOVERY.md contract shape, JSON-escaping of
# attacker-influenced device names (same discipline as the web XSS fix), the
# bounded/NUL-terminated writer, and the open/append/close hub composition.
# Pure header, no Arduino deps.
FLEETSR_HDR = ../common/fleet_selfreport/fleet_selfreport.h
FLEETSR_BIN = $(BUILD_DIR)/test_fleet_selfreport

# Device self-manifest (common/attest/self_manifest.h): the machine-readable
# JSON the device emits on 'j' and the website parses over WebSerial. Pins the
# schema + every key, proves string escaping, and that the bounded writer never
# overflows or emits truncated JSON. Pure header, no Arduino deps.
MANIFEST_HDR = ../common/attest/self_manifest.h
MANIFEST_BIN = $(BUILD_DIR)/test_self_manifest

# Birth-day decision (common/identity/birth_day.h): when a device may record
# the day its key was born, and when that day may be CALLED a birth day rather
# than the day it was first dated. Every test is a way the certificate could
# misstate the device's age. Pure header, no Arduino deps.
BIRTHDAY_HDR = ../common/identity/birth_day.h
BIRTHDAY_BIN = $(BUILD_DIR)/test_birth_day

# Coarse optical feature classifiers (canary-vision optical_features.h): pins
# the invariant-safe coarsening of the person-detection model's box geometry —
# aspect ratio -> posture, area fraction -> proximity, count -> occupancy
# bucket. Proves only ordinals/names are emitted (no coordinates/area/distance).
# Pure header over canary/types.h, no Arduino deps.
OPTFEAT_INC = ../projects/canary-vision/include
OPTFEAT_HDR = $(OPTFEAT_INC)/canary/vision/optical_features.h $(OPTFEAT_INC)/canary/types.h
OPTFEAT_BIN = $(BUILD_DIR)/test_optical_features

# Canary Vision detection pipeline: this is the exact allocation-free header
# compiled by both vision_mgr.cpp on the ESP32 and the browser wasm core.
VISION_INC = ../projects/canary-vision/include
VISION_CFG = ../configs/canary-vision/default
VISION_PIPE_HDR = $(VISION_INC)/canary/vision/detection_pipeline.h \
                  $(VISION_INC)/canary/vision/optical_features.h \
                  $(VISION_INC)/canary/detect_config.h \
                  $(VISION_INC)/canary/types.h
VISION_PIPE_BIN = $(BUILD_DIR)/test_vision_detection_pipeline

# Sentinel multi-sensor fusion brain (common/fusion/sentinel_fusion.*): the
# board-agnostic evidence-fusion engine compiled by projects/canary-sentinel on
# device and here on the host. Pins the anti-evasion scoring (independence
# bonus, denied-channel suspicion, silent-body anomaly) and the debounced
# decision FSM. Built with -DSENTINEL_FUSION_TEST_HOST so the reference
# default_standard_config() is available to the suite. Pure C++, no Arduino.
FUSION_DIR  = ../common/fusion
FUSION_SRC  = $(FUSION_DIR)/sentinel_fusion.cpp
FUSION_HDR  = $(FUSION_DIR)/sentinel_fusion.h $(FUSION_DIR)/sentinel_channels.h
FUSION_BIN  = $(BUILD_DIR)/test_sentinel_fusion

# Preset -> FusionConfig -> engine pipeline (projects/canary-sentinel's
# sentinel_config.h over configs/canary-sentinel/<preset>). Built once per
# preset: the preset dir is placed on the include path so <config.h> resolves
# to it, and its name is passed for the banner. Two presets cover both the
# full stack (door) and the reduced Lite tier (mailbox-lite, radar/CSI off).
SENT_PROJ_INC = ../projects/canary-sentinel/include
SENT_CFG_DIR  = ../configs/canary-sentinel
SENT_PRESET_HDR = $(SENT_PROJ_INC)/canary/sentinel_config.h \
                  $(FUSION_DIR)/sentinel_fusion.h $(FUSION_DIR)/sentinel_channels.h
PRESET_DOOR_BIN = $(BUILD_DIR)/test_sentinel_preset_door
PRESET_LITE_BIN = $(BUILD_DIR)/test_sentinel_preset_mailbox_lite

# Fleet-figure art tier (common/core/fleet_figures_art.h over
# fleet_figures.h): every figure the firmware lookups can resolve has
# generated, in-bounds, non-degenerate triangle art at the matching rev — so
# the roll call can draw a witness's real picture and a device that names
# itself never draws nothing (or worse, stale art). Pure headers, no Arduino.
FIGART_HDRS = ../common/core/fleet_figures.h ../common/core/fleet_figures_art.h
FIGART_BIN  = $(BUILD_DIR)/test_fleet_figures_art

# Mini-calendar arithmetic (canary-display ui/calendar_math.h): leap rules,
# Sakamoto weekdays against independently-known dates, grid geometry, and
# the 6-row layout budget. A bedside calendar that misplaces a weekday is a
# quiet lie in the most trusted spot in the house. Pure header.
CALMATH_HDR = ../projects/canary-display/include/canary/ui/calendar_math.h
CALMATH_BIN = $(BUILD_DIR)/test_calendar_math

# Standalone-weather request/blob contract (canary-display net/wx_core.h +
# the combined-location codec in glass_settings.h): the URL is the privacy
# disclosure — coarse 0.1-degree coordinate and field names, nothing
# identifier-shaped — and the fetched values leave through the hub blob's
# exact JSON shape so every face behaves identically either way. Pure headers.
WXCORE_HDRS = ../projects/canary-display/include/canary/net/wx_core.h \
              ../projects/canary-display/include/canary/glass_settings.h
WXCORE_BIN = $(BUILD_DIR)/test_wx_core

# Serial tuning console (common/console/tuning_console.h): the line-based
# knob console the canary-sense bench speaks over USB serial. Proves the
# command vocabulary (help/cfg/set/reset/stream/raw), clamping through the
# knob table, strict number parsing, overflow discipline, CRLF tolerance,
# and the single-line [cfg] snapshot the flasher parses. Pure header.
TUNECON_HDR = ../common/console/tuning_console.h
TUNECON_BIN = $(BUILD_DIR)/test_tuning_console

# Nightstand look engine (common/color/*): the gamma LUT, integer HSV→RGB,
# warmth, palette sampling, breath easing, and the scene→beacon/wash engine
# with its honest severity override. Pure integer C++ (no float, no Arduino),
# so the whole color pipeline is proven on the host before it ever reaches the
# FPU-less C6.
LOOK_DIR  = ../common/color
LOOK_SRC  = $(LOOK_DIR)/color_engine.cpp $(LOOK_DIR)/look_engine.cpp
LOOK_HDRS = $(LOOK_DIR)/color_engine.h $(LOOK_DIR)/look_engine.h
LOOK_BIN  = $(BUILD_DIR)/test_look_engine

# The plumage engine (common/color/plumage.*): the birdsong light-language the
# hallway nightlight speaks — syllable envelopes, per-device voice, the phrase
# scheduler, and the rising-band render. Its most important assertions are the
# honesty ones: under Warn/Alert and under safe_dark its output must be
# BIT-IDENTICAL to the look engine's, because plumage delegates those paths
# rather than reimplementing them.
PLUMAGE_SRC  = $(LOOK_SRC) $(LOOK_DIR)/plumage.cpp
PLUMAGE_HDRS = $(LOOK_HDRS) $(LOOK_DIR)/plumage.h
PLUMAGE_BIN  = $(BUILD_DIR)/test_plumage

# The story engine (common/story/*): the beat-sheet performance engine and the
# scripts it plays. Its most important assertions are the ones that keep a
# character from becoming a liability — attention preempts the story and it
# never resumes, night is sacred, no cutscene traps the user, and the WRITING
# is checked against its own rules (plain ASCII, short enough to read at a
# glance, and containing no diagnostic vocabulary — that last one is the guard
# rail on "ambient only, never diagnosis").
# Header-only (see the note in story.h): splash.cpp plays a scene and compiles
# into ~20 PlatformIO envs, several of which must name every shared .cpp by
# hand — a pure model with no TU to link cannot be forgotten by one of them.
STORY_DIR  = ../common/story
STORY_HDRS = $(STORY_DIR)/story.h $(STORY_DIR)/story_scripts.h
STORY_BIN  = $(BUILD_DIR)/test_story

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

$(USBONB_BIN): test_usb_onboard_logic.cpp $(USBONB_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_usb_onboard_logic.cpp -o $@

$(TESTCON_BIN): test_test_console.cpp $(TESTCON_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_test_console.cpp -o $@

$(BOOTPOL_BIN): test_boot_policy.cpp $(BOOTPOL_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_boot_policy.cpp -o $@

$(WIFIJOIN_BIN): test_wifi_join_policy.cpp $(WIFIJOIN_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ test_wifi_join_policy.cpp

$(PORTAL_BIN): test_setup_portal_logic.cpp $(PORTAL_HDRS) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_setup_portal_logic.cpp -o $@

$(POWEREVT_BIN): test_power_events.cpp $(POWEREVT_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_power_events.cpp -o $@

$(SCENE_BIN): test_console_scene.cpp $(SCENE_HDRS) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_console_scene.cpp -o $@

$(FLEETVIEW_BIN): test_fleet_view.cpp $(FLEETVIEW_HDRS) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_fleet_view.cpp -o $@

$(FLEETSR_BIN): test_fleet_selfreport.cpp $(FLEETSR_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_fleet_selfreport.cpp -o $@

$(MANIFEST_BIN): test_self_manifest.cpp $(MANIFEST_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_self_manifest.cpp -o $@

$(BIRTHDAY_BIN): test_birth_day.cpp $(BIRTHDAY_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_birth_day.cpp -o $@

$(OPTFEAT_BIN): test_optical_features.cpp $(OPTFEAT_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I $(OPTFEAT_INC) test_optical_features.cpp -o $@

$(VISION_PIPE_BIN): test_vision_detection_pipeline.cpp $(VISION_PIPE_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I $(VISION_INC) -I $(VISION_CFG) test_vision_detection_pipeline.cpp -o $@

$(FUSION_BIN): test_sentinel_fusion.cpp $(FUSION_SRC) $(FUSION_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -DSENTINEL_FUSION_TEST_HOST -I $(FUSION_DIR) test_sentinel_fusion.cpp $(FUSION_SRC) -o $@

$(PRESET_DOOR_BIN): test_sentinel_presets.cpp $(FUSION_SRC) $(SENT_PRESET_HDR) $(SENT_CFG_DIR)/door/config.h Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) '-DSENTINEL_PRESET_NAME="door"' -I $(SENT_PROJ_INC) -I ../common -I $(SENT_CFG_DIR)/door test_sentinel_presets.cpp $(FUSION_SRC) -o $@

$(PRESET_LITE_BIN): test_sentinel_presets.cpp $(FUSION_SRC) $(SENT_PRESET_HDR) $(SENT_CFG_DIR)/mailbox-lite/config.h Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) '-DSENTINEL_PRESET_NAME="mailbox-lite"' -I $(SENT_PROJ_INC) -I ../common -I $(SENT_CFG_DIR)/mailbox-lite test_sentinel_presets.cpp $(FUSION_SRC) -o $@

$(LOOK_BIN): test_look_engine.cpp $(LOOK_SRC) $(LOOK_HDRS) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_look_engine.cpp $(LOOK_SRC) -o $@

$(STORY_BIN): test_story.cpp $(STORY_HDRS) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_story.cpp -o $@

$(PLUMAGE_BIN): test_plumage.cpp $(PLUMAGE_SRC) $(PLUMAGE_HDRS) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_plumage.cpp $(PLUMAGE_SRC) -o $@

$(TUNECON_BIN): test_tuning_console.cpp $(TUNECON_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_tuning_console.cpp -o $@

$(FIGART_BIN): test_fleet_figures_art.cpp $(FIGART_HDRS) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../common test_fleet_figures_art.cpp -o $@

$(CALMATH_BIN): test_calendar_math.cpp $(CALMATH_HDR) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../projects/canary-display/include test_calendar_math.cpp -o $@

$(WXCORE_BIN): test_wx_core.cpp $(WXCORE_HDRS) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I ../projects/canary-display/include test_wx_core.cpp -o $@

run: $(MR60_BIN) $(MR60_VITALS_BIN) $(DEVSIG_BIN) $(WITCHAIN_BIN) $(WITSTORE_BIN) $(USBONB_BIN) $(TESTCON_BIN) $(BOOTPOL_BIN) $(WIFIJOIN_BIN) $(PORTAL_BIN) $(POWEREVT_BIN) $(SCENE_BIN) $(FLEETVIEW_BIN) $(FLEETSR_BIN) $(MANIFEST_BIN) $(BIRTHDAY_BIN) $(OPTFEAT_BIN) $(VISION_PIPE_BIN) $(FUSION_BIN) $(PRESET_DOOR_BIN) $(PRESET_LITE_BIN) $(LOOK_BIN) $(PLUMAGE_BIN) $(STORY_BIN) $(TUNECON_BIN) $(FIGART_BIN) $(CALMATH_BIN) $(WXCORE_BIN)
	./$(MR60_BIN)
	./$(MR60_VITALS_BIN)
	./$(DEVSIG_BIN)
	./$(WITCHAIN_BIN)
	./$(WITSTORE_BIN)
	./$(USBONB_BIN)
	./$(TESTCON_BIN)
	./$(BOOTPOL_BIN)
	./$(WIFIJOIN_BIN)
	./$(PORTAL_BIN)
	./$(POWEREVT_BIN)
	./$(SCENE_BIN)
	./$(FLEETVIEW_BIN)
	./$(FLEETSR_BIN)
	./$(MANIFEST_BIN)
	./$(BIRTHDAY_BIN)
	./$(OPTFEAT_BIN)
	./$(VISION_PIPE_BIN)
	./$(FUSION_BIN)
	./$(PRESET_DOOR_BIN)
	./$(PRESET_LITE_BIN)
	./$(LOOK_BIN)
	./$(PLUMAGE_BIN)
	./$(STORY_BIN)
	./$(TUNECON_BIN)
	./$(FIGART_BIN)
	./$(CALMATH_BIN)
	./$(WXCORE_BIN)

clean:
	rm -rf $(BUILD_DIR)
