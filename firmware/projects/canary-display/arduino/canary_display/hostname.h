#pragma once
#include <stddef.h>
#include <stdio.h>

#include "identity/device_pseudonym.h"

// The glass's LAN name, composed in exactly one place. mDNS registration
// (discovery.cpp) and every surface that PRINTS the name — the settings
// network page, the transparency sheet — must agree byte-for-byte, or the
// glass tells someone to type an address that doesn't answer.
//
// "canary_watch_001" -> "canary-watch-001-a1b2c3": mDNS hostnames are
// hyphen-world, and the salted pseudonym suffix keeps two same-id units
// from colliding without ever touching the MAC (Invariant III).
//
// Header-only so the emulator build (which omits discovery.cpp) prints
// the same name the real radio registers.

namespace canary::net {

inline void make_hostname(const char* device_id, char* out, size_t cap) {
  char devid_hex[device_pseudonym::HEX_LEN + 1] = {0};
  device_pseudonym::device_id_hex(devid_hex, sizeof(devid_hex));
  char base[24];
  {
    const char* src =
        device_id && device_id[0] ? device_id : "canary-display";
    size_t i = 0;
    for (; i + 1 < sizeof(base) && src[i]; i++) {
      const char c = src[i];
      base[i] = (c == '_' || c == ' ' || c == '.') ? '-' : c;
    }
    base[i] = '\0';
  }
  snprintf(out, cap, "%s-%.6s", base, devid_hex);
}

}  // namespace canary::net
