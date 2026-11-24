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
// `used` says what RTC memory is: written by something this translation
// unit cannot see (the previous boot; here, the test). Without it clang -O2
// sees the record's magic stored only as 0 or WINDOW_MAGIC, splits the
// struct and shrinks the magic to a one-byte bool, so any nonzero byte reads
// back as the magic; GCC keeps the 8 bytes. IDF's own RTC_NOINIT_ATTR is the
// section alone (the device builds are GCC's).
#if defined(__APPLE__)
#define RTC_NOINIT_ATTR __attribute__((used, section("__DATA,rtc_noinit")))
#else
#define RTC_NOINIT_ATTR __attribute__((used, section("rtc_noinit")))
#endif
