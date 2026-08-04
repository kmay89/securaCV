// tincan_bringup — day-one bench firmware for the Tin Can watch board.
//
//   Board:  Waveshare ESP32-S3-Touch-AMOLED-2.06 (waveshare-esp32s3-amoled206)
//   Design: docs/design/canary_tincan_kids_watch.md
//   Runbook: ../README.md  <- read this first, it has the failure ladder
//
// THIS IS A BENCH TOOL, NOT THE PRODUCT. It exists to answer, in order, the
// questions that block everything else — and to answer them in seconds rather
// than in an afternoon of bisecting a big app:
//
//   1. What is actually on the I2C bus?  (settles FT3168 vs CST9220, and tells
//      you whether a haptic driver is fitted at all)
//   2. Does the panel light up, at the right size, with the right offset?
//   3. Does touch track your finger?
//   4. Do two watches see each other over ESP-NOW?
//   5. Does a knock cross the room?
//
// Stage 5 is the whole product in miniature: tap a rhythm on one wrist, feel it
// on the other. If that lands, the Tin Can is real. If it feels mushy, stop and
// fix the motor before building anything on top of it.
//
// WHAT THIS IS NOT: there is no encryption here. Bring-up frames use their own
// magic (TC_BRINGUP_MAGIC) which the real Canary Link parser rejects outright —
// link_frame.h requires 0xCA5E and a minimum length this never reaches — so a
// bench frame can never be mistaken for product traffic. The banner on screen
// says BENCH for the same reason. Encryption lands with the real firmware
// (link_session.h), not here.
//
// Libraries (Arduino IDE → Library Manager):
//   GFX Library for Arduino  1.6.0+   (moononournation) — needs esp32 core 3.x
// Touch, IMU, RTC, PMU and the haptic are driven over raw Wire on purpose:
// every library skipped is a library that cannot fail to compile at 9am.
//
// Board settings: ESP32S3 Dev Module · USB CDC On Boot: Enabled ·
// Flash 32MB · PSRAM OPI PSRAM · Partition: 16MB (3MB APP/9.9MB FATFS)

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "Arduino_GFX_Library.h"
#include "HWCDC.h"

#include "knock_codec.h"  // staged copy of canary/tincan/knock_codec.h

using canary::tincan::Knock;
using canary::tincan::KnockCapture;
using canary::tincan::KnockPlayback;

HWCDC USBSerial;

// ---------------------------------------------------------------------------
// Pins — mirrors firmware/boards/waveshare-esp32s3-amoled206/pins/pins.h.
// Kept literal here so the sketch is self-contained in the Arduino IDE.
// ---------------------------------------------------------------------------

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK  11
#define LCD_CS    12
#define LCD_RESET 8
#define LCD_W     410
#define LCD_H     502

#define IIC_SDA   15
#define IIC_SCL   14
#define TP_INT    38
#define TP_RESET  9

#define ADDR_TOUCH  0x38  // FT3168 (expected)
#define ADDR_PMU    0x34  // AXP2101
#define ADDR_RTC    0x51  // PCF85063
#define ADDR_IMU    0x6B  // QMI8658
#define ADDR_HAPTIC 0x5A  // DRV2605L — the required add-on
#define ADDR_CODEC  0x18  // ES8311

// The 22-px column offset is NOT optional and NOT guessable: the CO5300's
// controller RAM is wider than the visible glass, so without it every pixel
// lands 22 columns left of where you asked. Straight from Waveshare's own
// example — if your image looks shifted, this is the number to doubt.
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_GFX *gfx = new Arduino_CO5300(bus, LCD_RESET, 0 /* rotation */,
                                      LCD_W, LCD_H,
                                      22 /* col_offset1 */, 0, 0, 0);

// ---------------------------------------------------------------------------
// Bench wire format — deliberately NOT a Canary Link frame
// ---------------------------------------------------------------------------

static const uint8_t TC_BRINGUP_MAGIC0 = 0x7B;  // '{' — nothing like 0xCA5E
static const uint8_t TC_BRINGUP_MAGIC1 = 0x7C;

enum : uint8_t {
  TC_HELLO = 1,  // "I am here"  — payload: none
  TC_KNOCK = 2,  // a knock      — payload: knock_encode() bytes
};

static uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static uint16_t g_my_id = 0;      // last 2 bytes of our MAC — our "fp4"
static bool g_haptic = false;     // a DRV2605L answered at 0x5A
static uint8_t g_touch_chip = 0;  // value of FT3x68 reg 0xA0

struct Peer {
  uint16_t id = 0;
  int16_t rssi = 0;
  uint32_t last_ms = 0;
  bool used = false;
};
static Peer g_peers[4];

static KnockCapture g_capture;
static KnockPlayback g_play;
static uint8_t g_play_next = 0;
static uint32_t g_play_start = 0;
static bool g_playing = false;
static uint32_t g_flash_until = 0;

static int g_stage = 0;
static uint32_t g_stage_since = 0;
static bool g_stage_drawn = false;

// ---------------------------------------------------------------------------
// I2C helpers
// ---------------------------------------------------------------------------

static bool i2c_present(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static bool i2c_read8(uint8_t addr, uint8_t reg, uint8_t &out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, 1) != 1) return false;
  out = Wire.read();
  return true;
}

static bool i2c_write8(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// Name the addresses we expect, so the scan reads like an answer rather than a
// list of numbers.
static const char *i2c_name(uint8_t a) {
  switch (a) {
    case ADDR_TOUCH:  return "touch (FT3168 expected)";
    case ADDR_PMU:    return "AXP2101 PMU";
    case ADDR_RTC:    return "PCF85063 RTC";
    case ADDR_IMU:    return "QMI8658 IMU";
    case ADDR_HAPTIC: return "DRV2605L HAPTIC";
    case ADDR_CODEC:  return "ES8311 codec";
    default:          return "unknown";
  }
}

// ---------------------------------------------------------------------------
// Touch — raw FocalTech register reads, no library
// ---------------------------------------------------------------------------

static bool touch_read(int32_t &x, int32_t &y) {
  uint8_t n = 0;
  if (!i2c_read8(ADDR_TOUCH, 0x02, n)) return false;
  if ((n & 0x0F) == 0) return false;

  uint8_t xh, xl, yh, yl;
  if (!i2c_read8(ADDR_TOUCH, 0x03, xh)) return false;
  if (!i2c_read8(ADDR_TOUCH, 0x04, xl)) return false;
  if (!i2c_read8(ADDR_TOUCH, 0x05, yh)) return false;
  if (!i2c_read8(ADDR_TOUCH, 0x06, yl)) return false;

  x = (int32_t)(((uint16_t)(xh & 0x0F) << 8) | xl);
  y = (int32_t)(((uint16_t)(yh & 0x0F) << 8) | yl);
  return true;
}

// ---------------------------------------------------------------------------
// Haptic — minimal DRV2605L in real-time playback mode
//
// RTP is the simplest thing that can produce a pulse: set MODE to RTP (which
// also clears standby), write an amplitude, write zero to stop. No waveform
// library, no ROM sequencing, nothing to get subtly wrong.
//
// UNTESTED ON HARDWARE — nobody has held one of these yet. If a knock does
// nothing but the scan found 0x5A, this function is the first suspect.
// ---------------------------------------------------------------------------

static void haptic_begin() {
  if (!g_haptic) return;
  i2c_write8(ADDR_HAPTIC, 0x01, 0x05);  // MODE = real-time playback
  i2c_write8(ADDR_HAPTIC, 0x02, 0x00);  // silent
}

static void haptic_pulse(uint8_t amplitude, uint16_t ms) {
  if (!g_haptic) return;
  i2c_write8(ADDR_HAPTIC, 0x02, amplitude);
  delay(ms);
  i2c_write8(ADDR_HAPTIC, 0x02, 0x00);
}

// One knock tap: short and sharp. Paired with a screen flash so the board is
// still legible when no motor is fitted — a knock you cannot feel must at
// least be a knock you can see, never one that silently did nothing.
static void tap_feedback() {
  haptic_pulse(0x7F, 18);
  g_flash_until = millis() + 60;
}

// ---------------------------------------------------------------------------
// ESP-NOW
// ---------------------------------------------------------------------------

static void note_peer(uint16_t id, int16_t rssi) {
  for (int i = 0; i < 4; i++) {
    if (g_peers[i].used && g_peers[i].id == id) {
      g_peers[i].rssi = rssi;
      g_peers[i].last_ms = millis();
      return;
    }
  }
  for (int i = 0; i < 4; i++) {
    if (!g_peers[i].used) {
      g_peers[i] = {id, rssi, millis(), true};
      return;
    }
  }
}

// The ESP-NOW receive callback runs on the Wi-Fi task, NOT in loop(). Touching
// the playback or peer state directly from here would race loop() — and the
// race is not theoretical: publishing g_playing before g_play_start is written
// lets loop() start replaying a knock against a stale timestamp, which shows up
// as a rhythm that is subtly wrong. On a device whose entire product is "the
// rhythm arrives exactly as tapped", that is the worst possible bug: it looks
// like bad haptics rather than bad code.
//
// So the callback does the least it can — copy the bytes into a small ring
// under a spinlock — and loop() does the decoding and installing. Keeping the
// callback short is also just correct ESP-NOW practice.

struct Inbound {
  uint16_t from;
  int16_t rssi;
  uint8_t kind;
  uint8_t len;
  uint8_t payload[32];
};

static const uint8_t INBOX_N = 4;
static Inbound g_inbox[INBOX_N];
static uint8_t g_in_head = 0;
static uint8_t g_in_tail = 0;
static portMUX_TYPE g_inbox_mux = portMUX_INITIALIZER_UNLOCKED;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data,
                    int len) {
  if (len < 5) return;
  if (data[0] != TC_BRINGUP_MAGIC0 || data[1] != TC_BRINGUP_MAGIC1) return;

  const uint16_t from = (uint16_t)(data[3] | ((uint16_t)data[4] << 8));
  if (from == g_my_id) return;  // our own broadcast, reflected

  size_t n = (size_t)(len - 5);
  if (n > sizeof(g_inbox[0].payload)) n = sizeof(g_inbox[0].payload);

  portENTER_CRITICAL(&g_inbox_mux);
  const uint8_t next = (uint8_t)((g_in_head + 1) % INBOX_N);
  if (next != g_in_tail) {  // full ring drops rather than overwrites
    Inbound &s = g_inbox[g_in_head];
    s.from = from;
    s.rssi = info ? info->rx_ctrl->rssi : 0;
    s.kind = data[2];
    s.len = (uint8_t)n;
    if (n) memcpy(s.payload, data + 5, n);
    g_in_head = next;
  }
  portEXIT_CRITICAL(&g_inbox_mux);
}

// Drain the ring on the loop task, where all the state actually lives.
static void drain_inbox() {
  for (;;) {
    Inbound f;
    bool have = false;

    portENTER_CRITICAL(&g_inbox_mux);
    if (g_in_tail != g_in_head) {
      f = g_inbox[g_in_tail];
      g_in_tail = (uint8_t)((g_in_tail + 1) % INBOX_N);
      have = true;
    }
    portEXIT_CRITICAL(&g_inbox_mux);

    if (!have) return;

    note_peer(f.from, f.rssi);

    if (f.kind == TC_KNOCK && f.len) {
      Knock k;
      // knock_decode is total: a malformed knock is dropped, never half-played.
      // A half-understood rhythm replayed on a wrist is worse than none.
      if (canary::tincan::knock_decode(f.payload, f.len, k) &&
          canary::tincan::knock_playback(k, g_play)) {
        g_play_next = 0;
        g_play_start = millis();
        g_playing = true;  // published LAST, once the schedule is complete
        USBSerial.printf("[knock] from %04x, %u taps, span %ums\n", f.from,
                         (unsigned)k.taps, (unsigned)k.span_ms());
      }
    }
  }
}

static void send_frame(uint8_t kind, const uint8_t *payload, size_t n) {
  uint8_t buf[64];
  if (n > sizeof(buf) - 5) return;
  buf[0] = TC_BRINGUP_MAGIC0;
  buf[1] = TC_BRINGUP_MAGIC1;
  buf[2] = kind;
  buf[3] = (uint8_t)(g_my_id & 0xFF);
  buf[4] = (uint8_t)(g_my_id >> 8);
  if (n && payload) memcpy(buf + 5, payload, n);
  esp_now_send(BCAST, buf, 5 + n);
}

static bool espnow_begin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(on_recv);

  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, BCAST, 6);
  p.channel = 0;  // follow whatever channel the STA interface is on
  p.encrypt = false;
  esp_now_add_peer(&p);

  uint8_t mac[6];
  WiFi.macAddress(mac);
  g_my_id = (uint16_t)(mac[4] | ((uint16_t)mac[5] << 8));
  return true;
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

static void banner(const char *title) {
  gfx->fillScreen(BLACK);
  gfx->fillRect(0, 0, LCD_W, 46, RED);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(10, 14);
  gfx->print("BENCH - not the product");

  gfx->setTextColor(WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(10, 64);
  gfx->println(title);
  gfx->setTextSize(2);
}

static void draw_scan() {
  banner("1 - I2C bus");
  int y = 120;
  gfx->setCursor(10, y);
  for (uint8_t a = 1; a < 0x7F; a++) {
    if (!i2c_present(a)) continue;
    gfx->setTextColor(a == ADDR_HAPTIC ? GREEN : WHITE);
    gfx->setCursor(10, y);
    gfx->printf("0x%02X %s", a, i2c_name(a));
    y += 26;
  }
  gfx->setTextColor(g_haptic ? GREEN : YELLOW);
  gfx->setCursor(10, y + 12);
  gfx->println(g_haptic ? "haptic: FITTED" : "haptic: MISSING - see runbook");

  gfx->setTextColor(g_touch_chip == 0x03 ? GREEN : YELLOW);
  gfx->setCursor(10, y + 40);
  gfx->printf("touch id 0x%02X %s", g_touch_chip,
              g_touch_chip == 0x03 ? "(FT3168)" : "(NOT FT3168!)");
}

static void draw_panel() {
  banner("2 - panel");
  // A 1px frame at the extreme edge: if the 22-px column offset is wrong, one
  // vertical edge is missing or the frame is clipped. Cheaper to read than a
  // color chart.
  gfx->drawRect(0, 0, LCD_W, LCD_H, WHITE);
  gfx->drawRect(1, 1, LCD_W - 2, LCD_H - 2, WHITE);
  gfx->fillRect(20, 140, 100, 60, RED);
  gfx->fillRect(140, 140, 100, 60, GREEN);
  gfx->fillRect(260, 140, 100, 60, BLUE);
  gfx->setTextColor(WHITE);
  gfx->setCursor(10, 230);
  gfx->println("All 4 edges visible?");
  gfx->setCursor(10, 258);
  gfx->println("R/G/B correct order?");
  gfx->setCursor(10, 300);
  gfx->println("touch & hold to advance");
}

static void draw_touch() {
  banner("3 - touch");
  gfx->setTextColor(WHITE);
  gfx->setCursor(10, 120);
  gfx->println("drag a finger");
  gfx->setCursor(10, 148);
  gfx->println("hold 2s to advance");
}

static void draw_peers() {
  banner("4 - peers");
  gfx->setTextColor(WHITE);
  gfx->setCursor(10, 120);
  gfx->printf("me: %04x", g_my_id);
  int y = 160;
  int n = 0;
  for (int i = 0; i < 4; i++) {
    if (!g_peers[i].used) continue;
    const uint32_t age = millis() - g_peers[i].last_ms;
    gfx->setTextColor(age < 4000 ? GREEN : YELLOW);
    gfx->setCursor(10, y);
    gfx->printf("%04x  %ddBm  %s", g_peers[i].id, (int)g_peers[i].rssi,
                age < 4000 ? "TAUT" : "slack");
    y += 28;
    n++;
  }
  if (!n) {
    gfx->setTextColor(YELLOW);
    gfx->setCursor(10, y);
    gfx->println("no peers yet - power the");
    gfx->setCursor(10, y + 26);
    gfx->println("second watch");
  }
  gfx->setTextColor(WHITE);
  gfx->setCursor(10, 400);
  gfx->println("hold 2s to advance");
}

static void draw_knock() {
  banner("5 - knock");
  gfx->setTextColor(WHITE);
  gfx->setCursor(10, 120);
  gfx->println("tap a rhythm");
  gfx->setCursor(10, 148);
  gfx->println("it lands on the other");
  gfx->setCursor(10, 176);
  gfx->println("watch, exactly as tapped");
}

// ---------------------------------------------------------------------------

void setup() {
  USBSerial.begin(115200);
  delay(300);
  USBSerial.println();
  USBSerial.println("=== Tin Can bench bring-up ===");
  USBSerial.println("BENCH TOOL - unencrypted, not the product");

  Wire.begin(IIC_SDA, IIC_SCL, 400000);

  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(10);
  digitalWrite(TP_RESET, HIGH);
  delay(50);

  USBSerial.println("-- I2C scan --");
  for (uint8_t a = 1; a < 0x7F; a++) {
    if (i2c_present(a)) USBSerial.printf("  0x%02X  %s\n", a, i2c_name(a));
  }

  g_haptic = i2c_present(ADDR_HAPTIC);
  USBSerial.printf("haptic (DRV2605L @0x5A): %s\n",
                   g_haptic ? "FITTED" : "MISSING");
  haptic_begin();

  // Register 0xA0 names the part outright: 0x03 = FT3168. This is what settles
  // the store page's CST9220 claim — not a guess, the chip's own answer.
  if (i2c_read8(ADDR_TOUCH, 0xA0, g_touch_chip)) {
    USBSerial.printf("touch device id 0x%02X %s\n", g_touch_chip,
                     g_touch_chip == 0x03 ? "= FT3168 (as expected)"
                                          : "= NOT FT3168 - update pins.h!");
  } else {
    USBSerial.println("touch: no answer at 0x38");
  }

  if (!gfx->begin()) USBSerial.println("gfx->begin() FAILED");
  gfx->fillScreen(BLACK);

  if (!espnow_begin()) USBSerial.println("esp_now_init FAILED");
  USBSerial.printf("my id: %04x\n", g_my_id);

  g_stage = 1;
  g_stage_since = millis();
}

void loop() {
  const uint32_t now = millis();

  if (!g_stage_drawn) {
    switch (g_stage) {
      case 1: draw_scan();  break;
      case 2: draw_panel(); break;
      case 3: draw_touch(); break;
      case 4: draw_peers(); break;
      default: draw_knock(); break;
    }
    g_stage_drawn = true;
  }

  // Beacon so the other watch can find us.
  static uint32_t last_hello = 0;
  if (now - last_hello > 1000) {
    last_hello = now;
    send_frame(TC_HELLO, nullptr, 0);
  }

  drain_inbox();

  // Touch, with edge detection so a press is one event.
  int32_t tx = 0, ty = 0;
  const bool down = touch_read(tx, ty);
  static bool was_down = false;
  static uint32_t down_since = 0;
  // One hold advances exactly one stage. Restarting the timer instead would
  // only postpone the next advance: two seconds later the same uninterrupted
  // press qualifies again, and a finger left resting walks straight past the
  // touch and peer checks to stage 5 — silently skipping the two stages whose
  // whole job is to catch a fault before you trust the knock.
  static bool hold_consumed = false;
  const bool pressed = down && !was_down;
  if (pressed) down_since = now;
  if (!down) hold_consumed = false;  // the latch clears only on release
  const bool long_held = down && !hold_consumed && (now - down_since > 2000);

  // Stage advance — long press everywhere except the auto-advancing scan.
  if (g_stage == 1 && now - g_stage_since > 6000) {
    g_stage = 2;
    g_stage_since = now;
    g_stage_drawn = false;
  } else if (long_held && g_stage < 5) {
    g_stage++;
    g_stage_since = now;
    g_stage_drawn = false;
    hold_consumed = true;
    tap_feedback();
  }

  if (g_stage == 3 && down) {
    gfx->fillCircle(tx, ty, 6, GREEN);
  }

  if (g_stage == 4) {
    static uint32_t repaint = 0;
    if (now - repaint > 700) {
      repaint = now;
      g_stage_drawn = false;
    }
  }

  // Stage 5: capture a rhythm and send it.
  if (g_stage == 5) {
    if (pressed) {
      if (g_capture.complete(now)) g_capture.reset();
      if (g_capture.tap(now)) {
        tap_feedback();
        gfx->fillCircle(30 + 34 * (g_capture.knock.taps - 1), 300, 12, WHITE);
      }
    }
    if (g_capture.started && g_capture.complete(now) && !down) {
      uint8_t wire[canary::tincan::KNOCK_MAX_WIRE];
      const size_t n =
          canary::tincan::knock_encode(g_capture.knock, wire, sizeof(wire));
      if (n) {
        send_frame(TC_KNOCK, wire, n);
        USBSerial.printf("[knock] sent %u taps (%u bytes)\n",
                         (unsigned)g_capture.knock.taps, (unsigned)n);
      }
      g_capture.reset();
      g_stage_drawn = false;  // clear the tap dots
    }
  }

  // Play back an inbound knock against its own schedule — the same pure
  // KnockPlayback the host tests exercise, so the timing here is the timing
  // that was tested.
  if (g_playing) {
    const uint32_t t = now - g_play_start;
    if (g_play_next < g_play.pulses && t >= g_play.at_ms[g_play_next]) {
      tap_feedback();
      g_play_next++;
    }
    if (g_play_next >= g_play.pulses) g_playing = false;
  }

  // The flash is the fallback when no motor is fitted.
  if (g_flash_until && now < g_flash_until) {
    gfx->fillRect(0, LCD_H - 40, LCD_W, 40, WHITE);
  } else if (g_flash_until && now >= g_flash_until) {
    gfx->fillRect(0, LCD_H - 40, LCD_W, 40, BLACK);
    g_flash_until = 0;
  }

  was_down = down;
  delay(8);
}
