/*
 * SecuraCV Canary — Evidence Drive core logic. Host-testable; see the
 * header for the contract and docs/design/usb_evidence_drive.md for the
 * design. No Arduino/ESP includes allowed in this file.
 */

#include "evidence_drive_logic.h"

#include <string.h>
#include <stdio.h>

namespace evidence_drive {

// ════════════════════════════════════════════════════════════════════════════
// 1. Share-mode state machine
// ════════════════════════════════════════════════════════════════════════════

bool share_apply(ShareStatus& st, ShareEvent ev, bool sd_quiesced) {
  switch (st.state) {
    case ShareState::NORMAL:
      if (ev == ShareEvent::SHARE_REQUEST && sd_quiesced) {
        st.state = ShareState::SHARED;
        st.shares_started++;
        return true;
      }
      return false; // everything else is a no-op while we own the SD

    case ShareState::SHARED:
      switch (ev) {
        case ShareEvent::SHARE_RELEASE:
        case ShareEvent::HOST_EJECT:
        case ShareEvent::USB_UNPLUG:
          st.state = ShareState::NORMAL;
          st.shares_ended++;
          return true;
        case ShareEvent::RING_PRESSURE:
          // Durability outranks the host: end the share so buffered witness
          // records can land on the SD before the ring overflows.
          st.state = ShareState::NORMAL;
          st.shares_ended++;
          st.forced_ends++;
          return true;
        case ShareEvent::SHARE_REQUEST:
          return false; // already shared
      }
      return false;
  }
  return false;
}

bool share_note_buffered(ShareStatus& st, uint32_t ring_capacity) {
  st.buffered_records++;
  if (st.buffered_records > st.buffered_peak) st.buffered_peak = st.buffered_records;
  return ring_capacity > SHARE_RING_HEADROOM &&
         st.buffered_records >= ring_capacity - SHARE_RING_HEADROOM;
}

void share_note_drained(ShareStatus& st) {
  st.buffered_records = 0;
}

// ════════════════════════════════════════════════════════════════════════════
// 2. FAT16 staging volume
// ════════════════════════════════════════════════════════════════════════════

static void wr16(uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void wr32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = v >> 24;
}
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const char README_TEXT[] =
  "SecuraCV Canary update drop-zone\r\n"
  "--------------------------------\r\n"
  "Copy ONE signed firmware file (canary-...-factory.bin from a SecuraCV\r\n"
  "release) onto this drive, then eject it. The Canary checks the file's\r\n"
  "signature exactly like a network update: a good file installs on\r\n"
  "reboot, anything else is ignored and explained in RESULT.TXT.\r\n"
  "\r\n"
  "This drive is temporary memory - it empties every time you unplug.\r\n"
  "Your evidence lives on the read-only CANARY-EVIDENCE drive.\r\n";

// Write one 8.3 directory entry. `name83` is exactly 11 bytes, space-padded.
static void write_dirent(uint8_t* ent, const char* name83, uint8_t attr,
                         uint16_t first_cluster, uint32_t size) {
  memset(ent, 0, 32);
  memcpy(ent, name83, 11);
  ent[11] = attr;
  wr16(ent + 26, first_cluster);
  wr32(ent + 28, size);
}

void fat16_format(uint8_t* buf) {
  memset(buf, 0, FAT_VOLUME_BYTES);

  // Boot sector / BPB.
  uint8_t* b = buf;
  b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;             // jump
  memcpy(b + 3, "SECURACV", 8);                       // OEM
  wr16(b + 11, FAT_SECTOR_SIZE);
  b[13] = 1;                                          // sectors per cluster
  wr16(b + 14, 1);                                    // reserved sectors
  b[16] = 2;                                          // FAT copies
  wr16(b + 17, FAT_ROOT_ENTRIES);
  wr16(b + 19, FAT_TOTAL_SECTORS);                    // fits in 16 bits
  b[21] = 0xF8;                                       // media descriptor
  wr16(b + 22, FAT_SECTORS_PER_FAT);
  wr16(b + 24, 63); wr16(b + 26, 255);                // CHS geometry (ignored)
  b[38] = 0x29;                                       // extended boot signature
  wr32(b + 39, 0x5ec0ca01u);                          // volume id (arbitrary)
  memcpy(b + 43, "CANARY-UPD ", 11);                  // volume label
  memcpy(b + 54, "FAT16   ", 8);
  b[510] = 0x55; b[511] = 0xAA;

  // Both FATs: media byte + EOC, then the README's single cluster chain.
  for (int f = 0; f < 2; f++) {
    uint8_t* fat = buf + (1 + f * FAT_SECTORS_PER_FAT) * FAT_SECTOR_SIZE;
    wr16(fat + 0, 0xFFF8);
    wr16(fat + 2, 0xFFFF);
    wr16(fat + 4, 0xFFFF);                            // cluster 2 = README, EOC
  }

  // Root directory: volume label + README.TXT.
  uint8_t* root = buf + (1 + 2 * FAT_SECTORS_PER_FAT) * FAT_SECTOR_SIZE;
  write_dirent(root, "CANARY-UPD ", 0x08, 0, 0);
  write_dirent(root + 32, "README  TXT", 0x21 /* read-only + archive */,
               2, (uint32_t)sizeof(README_TEXT) - 1);

  // README contents in cluster 2 (first data cluster).
  memcpy(buf + FAT_FIRST_DATA_SECTOR * FAT_SECTOR_SIZE,
         README_TEXT, sizeof(README_TEXT) - 1);
}

// Walk one file's FAT chain, bounded. Returns chained cluster count (0 on
// corruption/loops) and optionally emits cluster numbers via callback-free
// second pass in fat16_read_file.
static uint32_t chain_length(const uint8_t* buf, uint16_t first, uint32_t max_hops) {
  const uint8_t* fat = buf + FAT_SECTOR_SIZE; // FAT #0
  uint32_t n = 0;
  uint16_t c = first;
  while (n < max_hops) {
    if (c < 2 || c >= FAT_DATA_CLUSTERS + 2) return 0;   // out of range
    n++;
    uint16_t next = rd16(fat + c * 2);
    if (next >= 0xFFF8) return n;                        // end of chain
    if (next == 0x0000 || next == 0xFFF7) return 0;      // free/bad mid-chain
    c = next;
  }
  return 0; // too long — treat as corrupt
}

static void name83_to_string(const uint8_t* ent, char* out /* cap 13 */) {
  int p = 0;
  for (int i = 0; i < 8 && ent[i] != ' '; i++) out[p++] = (char)ent[i];
  if (ent[8] != ' ') {
    out[p++] = '.';
    for (int i = 8; i < 11 && ent[i] != ' '; i++) out[p++] = (char)ent[i];
  }
  out[p] = 0;
}

size_t fat16_list_root(const uint8_t* buf, FatFile* out, size_t max_out) {
  const uint8_t* root = buf + (1 + 2 * FAT_SECTORS_PER_FAT) * FAT_SECTOR_SIZE;
  size_t found = 0;
  for (uint32_t i = 0; i < FAT_ROOT_ENTRIES && found < max_out; i++) {
    const uint8_t* ent = root + i * 32;
    const uint8_t first = ent[0];
    if (first == 0x00) break;          // end of directory
    if (first == 0xE5) continue;       // deleted
    const uint8_t attr = ent[11];
    if ((attr & 0x0F) == 0x0F) continue;   // long-name entry
    if (attr & 0x08) continue;             // volume label
    if (attr & 0x10) continue;             // directory
    FatFile& f = out[found];
    name83_to_string(ent, f.name);
    f.size = rd32(ent + 28);
    f.first_cluster = rd16(ent + 26);
    const uint32_t clusters_needed =
        (f.size + FAT_SECTOR_SIZE - 1) / FAT_SECTOR_SIZE;
    const uint32_t chained = f.size == 0 ? 0
        : chain_length(buf, f.first_cluster, FAT_DATA_CLUSTERS + 1);
    f.size_matches_chain = f.size > 0 && chained >= clusters_needed && chained > 0;
    found++;
  }
  return found;
}

static bool ends_with(const char* name, const char* ext) {
  const size_t n = strlen(name), e = strlen(ext);
  return n > e && strcmp(name + n - e, ext) == 0;
}

bool fat16_find_update(const uint8_t* buf, FatFile* bin_out, FatFile* manifest_out,
                       char* reason, size_t reason_cap) {
  const auto fail = [&](const char* msg) {
    if (reason && reason_cap) snprintf(reason, reason_cap, "%s", msg);
    return false;
  };
  FatFile files[16];
  const size_t n = fat16_list_root(buf, files, 16);
  FatFile *bin = nullptr, *manifest = nullptr;
  size_t bins = 0, manifests = 0;
  for (size_t i = 0; i < n; i++) {
    if (strcmp(files[i].name, "README.TXT") == 0 ||
        strcmp(files[i].name, "RESULT.TXT") == 0) continue;
    if (ends_with(files[i].name, ".BIN")) { bins++; bin = &files[i]; }
    // Hosts write "manifest-canary-….json" as an 8.3 alias — the extension
    // survives as its first three letters, "JSO".
    else if (ends_with(files[i].name, ".JSO")) { manifests++; manifest = &files[i]; }
  }
  if (bins == 0 && manifests == 0)
    return fail("copy the release .bin AND its manifest-*.json here, then eject");
  if (bins == 0) return fail("manifest found but no .bin - copy the release image too");
  if (manifests == 0) return fail("image found but no manifest-*.json - the manifest carries the signature; copy it too");
  if (bins > 1) return fail("more than one .bin file - copy exactly one, then eject");
  if (manifests > 1) return fail("more than one manifest file - copy exactly one, then eject");
  if (bin->size == 0) return fail("the .bin file is empty");
  if (bin->size > FAT_MAX_FILE_BYTES) return fail("the .bin file is larger than this drive");
  if (manifest->size == 0 || manifest->size > FAT_MAX_MANIFEST_BYTES)
    return fail("the manifest file doesn't look like a release manifest");
  if (!bin->size_matches_chain || !manifest->size_matches_chain)
    return fail("a file arrived incomplete - copy both again, then eject");
  *bin_out = *bin;
  *manifest_out = *manifest;
  return true;
}

uint32_t fat16_read_file(const uint8_t* buf, const FatFile& f,
                         uint8_t* dst, uint32_t dst_cap) {
  if (f.size == 0 || f.size > dst_cap) return 0;
  const uint8_t* fat = buf + FAT_SECTOR_SIZE;
  // Visited-cluster bitmap: a corrupt chain that cycles must fail closed,
  // not hand back duplicated sectors that happen to total `size` bytes.
  uint8_t visited[(FAT_DATA_CLUSTERS + 2 + 7) / 8] = {0};
  uint32_t copied = 0;
  uint16_t c = f.first_cluster;
  while (copied < f.size) {
    if (c < 2 || c >= FAT_DATA_CLUSTERS + 2) return 0;   // out of range
    if (visited[c >> 3] & (1u << (c & 7))) return 0;     // chain loop
    visited[c >> 3] |= (uint8_t)(1u << (c & 7));
    const uint32_t sector = FAT_FIRST_DATA_SECTOR + (c - 2);
    const uint32_t take = f.size - copied < FAT_SECTOR_SIZE
        ? f.size - copied : FAT_SECTOR_SIZE;
    memcpy(dst + copied, buf + sector * FAT_SECTOR_SIZE, take);
    copied += take;
    const uint16_t next = rd16(fat + c * 2);
    if (next >= 0xFFF8) break;
    c = next;
  }
  return copied == f.size ? copied : 0;
}

bool fat16_write_result(uint8_t* buf, const char* text) {
  uint8_t* root = buf + (1 + 2 * FAT_SECTORS_PER_FAT) * FAT_SECTOR_SIZE;
  uint8_t* fat0 = buf + FAT_SECTOR_SIZE;
  uint8_t* fat1 = buf + (1 + FAT_SECTORS_PER_FAT) * FAT_SECTOR_SIZE;
  const uint32_t len = (uint32_t)strlen(text) > FAT_SECTOR_SIZE
      ? FAT_SECTOR_SIZE : (uint32_t)strlen(text);

  // Reuse an existing RESULT.TXT entry, else take the first free slot.
  uint8_t* slot = nullptr;
  for (uint32_t i = 0; i < FAT_ROOT_ENTRIES; i++) {
    uint8_t* ent = root + i * 32;
    if (memcmp(ent, "RESULT  TXT", 11) == 0) { slot = ent; break; }
    if ((ent[0] == 0x00 || ent[0] == 0xE5) && !slot) slot = ent;
    if (ent[0] == 0x00) break;
  }
  if (!slot) return false;

  // Claim the highest free cluster (host allocators fill from the bottom).
  uint16_t cluster = 0;
  const uint16_t prev = slot[26] | (slot[27] << 8);
  if (memcmp(slot, "RESULT  TXT", 11) == 0 && prev >= 2) {
    cluster = prev; // rewrite in place
  } else {
    for (uint16_t c = (uint16_t)(FAT_DATA_CLUSTERS + 1); c >= 2; c--) {
      if (rd16(fat0 + c * 2) == 0x0000) { cluster = c; break; }
    }
    if (!cluster) return false;
  }
  wr16(fat0 + cluster * 2, 0xFFFF);
  wr16(fat1 + cluster * 2, 0xFFFF);
  uint8_t* data = buf + (FAT_FIRST_DATA_SECTOR + (cluster - 2)) * FAT_SECTOR_SIZE;
  memset(data, 0, FAT_SECTOR_SIZE);
  memcpy(data, text, len);
  write_dirent(slot, "RESULT  TXT", 0x20, cluster, len);
  return true;
}

} // namespace evidence_drive
