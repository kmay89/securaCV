// The global names arduino-esp32 3.3.8 puts in front of every line of
// canary_wap.ino and of every header it includes: arduino-cli and PlatformIO
// both open the sketch with Arduino.h. Transcribed from the core's
// cores/esp32/Arduino.h (its macros, typedefs, declarations and
// using-declarations), esp32-hal-gpio.h (pin levels, modes and interrupt
// kinds), WString.h (F() and FPSTR()) and binary.h (the B0 … B11111111
// enumerators, values left out) at the 3.3.8 tag, the core both of the
// sketch's builds install (sketch.yaml's profile; [platform_core3] in
// firmware/envs/platformio/platforms.ini, which canary-wap.ini takes).
// Macro bodies are the core's; they only matter if a header spells the name.
//
// Why a host test includes it FIRST, before the header it tests: a pure
// header is compiled by tests_host without any of these names and by the
// sketch after all of them, so a header that collides with one passes every
// host test and breaks both ESP32 compiles. Sweep F212's first
// identity_json.h did exactly that: `using namespace wap_json;` and then
// `boolean(w, ...)`, which is ambiguous next to `typedef bool boolean;`.
// Not every name the sketch's translation unit holds is here (ESP-IDF,
// FreeRTOS and the libraries add their own); these are the ones Arduino.h
// itself adds, which a hosted header is likeliest to spell by accident.

#ifndef CANARY_WAP_TESTS_HOST_ARDUINO_GLOBALS_H
#define CANARY_WAP_TESTS_HOST_ARDUINO_GLOBALS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

// ── Arduino.h ────────────────────────────────────────────────────────────
#define PI         3.1415926535897932384626433832795
#define HALF_PI    1.5707963267948966192313216916398
#define TWO_PI     6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define EULER      2.718281828459045235360287471352

#define SERIAL  0x0
#define DISPLAY 0x1

#define LSBFIRST 0
#define MSBFIRST 1

#define DEFAULT  1
#define EXTERNAL 0

#ifndef __STRINGIFY
#define __STRINGIFY(a) #a
#endif

#define _min(a, b)                ((a) < (b) ? (a) : (b))
#define _max(a, b)                ((a) > (b) ? (a) : (b))
#define _abs(x)                   ((x) > 0 ? (x) : -(x))
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#define _round(x)                 ((x) >= 0 ? (long)((x) + 0.5) : (long)((x) - 0.5))
#define radians(deg)              ((deg) * DEG_TO_RAD)
#define degrees(rad)              ((rad) * RAD_TO_DEG)
#define sq(x)                     ((x) * (x))

#define sei()          portENABLE_INTERRUPTS()
#define cli()          portDISABLE_INTERRUPTS()
#define interrupts()   sei()
#define noInterrupts() cli()

#define clockCyclesPerMicrosecond()  ((long int)getCpuFrequencyMhz())
#define clockCyclesToMicroseconds(a) ((a) / clockCyclesPerMicrosecond())
#define microsecondsToClockCycles(a) ((a) * clockCyclesPerMicrosecond())

#define lowByte(w)  ((uint8_t)((w) & 0xff))
#define highByte(w) ((uint8_t)((w) >> 8))

#define bitRead(value, bit)            (((value) >> (bit)) & 0x01)
#define bitSet(value, bit)             ((value) |= (1UL << (bit)))
#define bitClear(value, bit)           ((value) &= ~(1UL << (bit)))
#define bitToggle(value, bit)          ((value) ^= (1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))

#ifndef _NOP
#define _NOP() do { __asm__ volatile("nop"); } while (0)
#endif

#define bit(b) (1UL << (b))
#define _BV(b) (1UL << (b))

#define digitalPinToTimer(pin)   (0)
#define analogInPinToBit(P)      (P)
#define digitalPinToPort(pin)    ((digitalPinToGPIONumber(pin) > 31) ? 1 : 0)
#define digitalPinToBitMask(pin) (1UL << (digitalPinToGPIONumber(pin) & 31))
#define portOutputRegister(port) ((volatile uint32_t *)((port) ? GPIO_OUT1_REG : GPIO_OUT_REG))
#define portInputRegister(port)  ((volatile uint32_t *)((port) ? GPIO_IN1_REG : GPIO_IN_REG))
#define portModeRegister(port)   ((volatile uint32_t *)((port) ? GPIO_ENABLE1_REG : GPIO_ENABLE_REG))

#define NOT_A_PIN        -1
#define NOT_A_PORT       -1
#define NOT_AN_INTERRUPT -1
#define NOT_ON_TIMER     0

#define NUM_DIGITAL_PINS           SOC_GPIO_PIN_COUNT
#define NUM_ANALOG_INPUTS          (SOC_ADC_CHANNEL_NUM(0) + SOC_ADC_CHANNEL_NUM(1))
#define EXTERNAL_NUM_INTERRUPTS    NUM_DIGITAL_PINS
#define analogInputToDigitalPin(p) (((p) < NUM_ANALOG_INPUTS) ? (analogChannelToDigitalPin(p)) : -1)
#define digitalPinToInterrupt(p)   ((((uint8_t)digitalPinToGPIONumber(p)) < NUM_DIGITAL_PINS) ? (p) : NOT_AN_INTERRUPT)
#define digitalPinHasPWM(p)        (((uint8_t)digitalPinToGPIONumber(p)) < NUM_DIGITAL_PINS)

typedef bool boolean;
typedef uint8_t byte;
typedef unsigned int word;

void setup(void);
void loop(void);
long random(long);
long random(long, long);
void randomSeed(unsigned long);
void useRealRandomGenerator(bool useRandomHW);
long map(long, long, long, long, long);

extern "C" {
void init(void);
void initVariant(void);
void initArduino(void);
uint8_t shiftIn(uint8_t dataPin, uint8_t clockPin, uint8_t bitOrder);
void shiftOut(uint8_t dataPin, uint8_t clockPin, uint8_t bitOrder, uint8_t val);
}

#include <algorithm>
#include <cmath>

using std::abs;
using std::isinf;
using std::isnan;
using std::max;
using std::min;
using std::round;

uint16_t makeWord(uint16_t w);
uint16_t makeWord(uint8_t h, uint8_t l);

#define word(...) makeWord(__VA_ARGS__)

size_t getArduinoLoopTaskStackSize(void);
#define SET_LOOP_TASK_STACK_SIZE(sz) \
  size_t getArduinoLoopTaskStackSize() { return sz; }

unsigned long pulseIn(uint8_t pin, uint8_t state, unsigned long timeout = 1000000L);
unsigned long pulseInLong(uint8_t pin, uint8_t state, unsigned long timeout = 1000000L);

extern "C" bool getLocalTime(struct tm* info, uint32_t ms = 5000);
extern "C" void configTime(long gmtOffset_sec, int daylightOffset_sec, const char* server1,
                           const char* server2 = nullptr, const char* server3 = nullptr);
extern "C" void configTzTime(const char* tz, const char* server1, const char* server2 = nullptr,
                             const char* server3 = nullptr);

void setToneChannel(uint8_t channel = 0);
void tone(uint8_t _pin, unsigned int frequency, unsigned long duration = 0);
void noTone(uint8_t _pin);

// ── esp32-hal-gpio.h ─────────────────────────────────────────────────────
#define LOW  0x0
#define HIGH 0x1

#define INPUT             0x01
#define OUTPUT            0x03
#define PULLUP            0x04
#define INPUT_PULLUP      0x05
#define PULLDOWN          0x08
#define INPUT_PULLDOWN    0x09
#define OPEN_DRAIN        0x10
#define OUTPUT_OPEN_DRAIN 0x13
#define ANALOG            0xC0

#define DISABLED  0x00
#define RISING    0x01
#define FALLING   0x02
#define CHANGE    0x03
#define ONLOW     0x04
#define ONHIGH    0x05
#define ONLOW_WE  0x0C
#define ONHIGH_WE 0x0D

// ── WString.h ────────────────────────────────────────────────────────────
class __FlashStringHelper;
#define FPSTR(str_pointer) (reinterpret_cast<const __FlashStringHelper*>(str_pointer))
#define F(string_literal)  (FPSTR(PSTR(string_literal)))

// ── binary.h (values left out: only the names can collide) ───────────────
enum {
  B0, B1,
  B00, B01, B10, B11,
  B000, B001, B010, B011, B100, B101, B110, B111,
  B0000, B0001, B0010, B0011, B0100, B0101, B0110, B0111, B1000, B1001,
  B1010, B1011, B1100, B1101, B1110, B1111,
  B00000, B00001, B00010, B00011, B00100, B00101, B00110, B00111, B01000,
  B01001, B01010, B01011, B01100, B01101, B01110, B01111, B10000, B10001,
  B10010, B10011, B10100, B10101, B10110, B10111, B11000, B11001, B11010,
  B11011, B11100, B11101, B11110, B11111,
  B000000, B000001, B000010, B000011, B000100, B000101, B000110, B000111,
  B001000, B001001, B001010, B001011, B001100, B001101, B001110, B001111,
  B010000, B010001, B010010, B010011, B010100, B010101, B010110, B010111,
  B011000, B011001, B011010, B011011, B011100, B011101, B011110, B011111,
  B100000, B100001, B100010, B100011, B100100, B100101, B100110, B100111,
  B101000, B101001, B101010, B101011, B101100, B101101, B101110, B101111,
  B110000, B110001, B110010, B110011, B110100, B110101, B110110, B110111,
  B111000, B111001, B111010, B111011, B111100, B111101, B111110, B111111,
  B0000000, B0000001, B0000010, B0000011, B0000100, B0000101, B0000110,
  B0000111, B0001000, B0001001, B0001010, B0001011, B0001100, B0001101,
  B0001110, B0001111, B0010000, B0010001, B0010010, B0010011, B0010100,
  B0010101, B0010110, B0010111, B0011000, B0011001, B0011010, B0011011,
  B0011100, B0011101, B0011110, B0011111, B0100000, B0100001, B0100010,
  B0100011, B0100100, B0100101, B0100110, B0100111, B0101000, B0101001,
  B0101010, B0101011, B0101100, B0101101, B0101110, B0101111, B0110000,
  B0110001, B0110010, B0110011, B0110100, B0110101, B0110110, B0110111,
  B0111000, B0111001, B0111010, B0111011, B0111100, B0111101, B0111110,
  B0111111, B1000000, B1000001, B1000010, B1000011, B1000100, B1000101,
  B1000110, B1000111, B1001000, B1001001, B1001010, B1001011, B1001100,
  B1001101, B1001110, B1001111, B1010000, B1010001, B1010010, B1010011,
  B1010100, B1010101, B1010110, B1010111, B1011000, B1011001, B1011010,
  B1011011, B1011100, B1011101, B1011110, B1011111, B1100000, B1100001,
  B1100010, B1100011, B1100100, B1100101, B1100110, B1100111, B1101000,
  B1101001, B1101010, B1101011, B1101100, B1101101, B1101110, B1101111,
  B1110000, B1110001, B1110010, B1110011, B1110100, B1110101, B1110110,
  B1110111, B1111000, B1111001, B1111010, B1111011, B1111100, B1111101,
  B1111110, B1111111,
  B00000000, B00000001, B00000010, B00000011, B00000100, B00000101,
  B00000110, B00000111, B00001000, B00001001, B00001010, B00001011,
  B00001100, B00001101, B00001110, B00001111, B00010000, B00010001,
  B00010010, B00010011, B00010100, B00010101, B00010110, B00010111,
  B00011000, B00011001, B00011010, B00011011, B00011100, B00011101,
  B00011110, B00011111, B00100000, B00100001, B00100010, B00100011,
  B00100100, B00100101, B00100110, B00100111, B00101000, B00101001,
  B00101010, B00101011, B00101100, B00101101, B00101110, B00101111,
  B00110000, B00110001, B00110010, B00110011, B00110100, B00110101,
  B00110110, B00110111, B00111000, B00111001, B00111010, B00111011,
  B00111100, B00111101, B00111110, B00111111, B01000000, B01000001,
  B01000010, B01000011, B01000100, B01000101, B01000110, B01000111,
  B01001000, B01001001, B01001010, B01001011, B01001100, B01001101,
  B01001110, B01001111, B01010000, B01010001, B01010010, B01010011,
  B01010100, B01010101, B01010110, B01010111, B01011000, B01011001,
  B01011010, B01011011, B01011100, B01011101, B01011110, B01011111,
  B01100000, B01100001, B01100010, B01100011, B01100100, B01100101,
  B01100110, B01100111, B01101000, B01101001, B01101010, B01101011,
  B01101100, B01101101, B01101110, B01101111, B01110000, B01110001,
  B01110010, B01110011, B01110100, B01110101, B01110110, B01110111,
  B01111000, B01111001, B01111010, B01111011, B01111100, B01111101,
  B01111110, B01111111, B10000000, B10000001, B10000010, B10000011,
  B10000100, B10000101, B10000110, B10000111, B10001000, B10001001,
  B10001010, B10001011, B10001100, B10001101, B10001110, B10001111,
  B10010000, B10010001, B10010010, B10010011, B10010100, B10010101,
  B10010110, B10010111, B10011000, B10011001, B10011010, B10011011,
  B10011100, B10011101, B10011110, B10011111, B10100000, B10100001,
  B10100010, B10100011, B10100100, B10100101, B10100110, B10100111,
  B10101000, B10101001, B10101010, B10101011, B10101100, B10101101,
  B10101110, B10101111, B10110000, B10110001, B10110010, B10110011,
  B10110100, B10110101, B10110110, B10110111, B10111000, B10111001,
  B10111010, B10111011, B10111100, B10111101, B10111110, B10111111,
  B11000000, B11000001, B11000010, B11000011, B11000100, B11000101,
  B11000110, B11000111, B11001000, B11001001, B11001010, B11001011,
  B11001100, B11001101, B11001110, B11001111, B11010000, B11010001,
  B11010010, B11010011, B11010100, B11010101, B11010110, B11010111,
  B11011000, B11011001, B11011010, B11011011, B11011100, B11011101,
  B11011110, B11011111, B11100000, B11100001, B11100010, B11100011,
  B11100100, B11100101, B11100110, B11100111, B11101000, B11101001,
  B11101010, B11101011, B11101100, B11101101, B11101110, B11101111,
  B11110000, B11110001, B11110010, B11110011, B11110100, B11110101,
  B11110110, B11110111, B11111000, B11111001, B11111010, B11111011,
  B11111100, B11111101, B11111110, B11111111,
};

#endif  // CANARY_WAP_TESTS_HOST_ARDUINO_GLOBALS_H
