#pragma once
// esp_attr.h stand-in. RTC slow memory is ordinary memory on the host, so
// the glue's RTC_NOINIT_ATTR window record starts zeroed in every test
// process (as a power-on leaves it as far as the glue can tell: the reset
// reason, not the bytes, is what it trusts). The record is put in a section
// of its own, rtc_noinit, so test_improv_glue.cpp can lay down the record a
// previous boot left before begin() reads it, and read back what the glue
// saved, through the linker's section bounds (__start_ / __stop_ on ELF,
// section$start$ / section$end$ on Mach-O).
//
// The section alone, as IDF's own RTC_NOINIT_ATTR is: what keeps the
// record's 8 bytes as the glue writes them is the glue's own `volatile`
// (improv_ble.cpp, sweep F490), not this macro. Without it clang -O2 sees
// the magic stored only as 0 or WINDOW_MAGIC, splits the struct and shrinks
// the magic to a one-byte bool, so any nonzero byte a previous boot left
// reads back as the magic; `reset` then fails under clang++ (-O2, -Os and
// -O3 alike). Host g++ 13 (x86-64) kept the 8 bytes at all three either
// way; the device builds' esp xtensa and riscv GCCs were not checked.
#if defined(__APPLE__)
#define RTC_NOINIT_ATTR __attribute__((section("__DATA,rtc_noinit")))
#else
#define RTC_NOINIT_ATTR __attribute__((section("rtc_noinit")))
#endif
