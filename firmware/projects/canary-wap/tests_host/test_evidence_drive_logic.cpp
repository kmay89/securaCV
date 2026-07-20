// Host tests for evidence_drive_logic — the USB evidence drive's pure core:
// the single-writer share state machine and the FAT16 update staging volume
// (format → simulate a host dropping files → find/read the update pair).
// See docs/design/usb_evidence_drive.md. Build via the Makefile target
// test_evidence_drive_logic.

#include "../arduino/canary_wap/evidence_drive_logic.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

using namespace evidence_drive;

static int checks = 0;
#define CHECK(cond) do { \
  if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } \
  checks++; \
} while (0)

// ── helpers: act like a host OS writing an 8.3 file onto the volume ─────────
// Allocates a free cluster chain, writes the FAT entries (both copies), the
// data, and a root directory entry — the minimal faithful subset of what a
// real host does when you drop a file on the drive.
static void host_write_file(uint8_t* vol, const char* name83 /* 11 bytes */,
                            const uint8_t* data, uint32_t size) {
  uint8_t* fat0 = vol + FAT_SECTOR_SIZE;
  uint8_t* fat1 = vol + (1 + FAT_SECTORS_PER_FAT) * FAT_SECTOR_SIZE;
  const uint32_t clusters = size ? (size + FAT_SECTOR_SIZE - 1) / FAT_SECTOR_SIZE : 1;

  // find free clusters
  std::vector<uint16_t> chain;
  for (uint16_t c = 2; c < FAT_DATA_CLUSTERS + 2 && chain.size() < clusters; c++) {
    const uint16_t v = (uint16_t)(fat0[c * 2] | (fat0[c * 2 + 1] << 8));
    if (v == 0x0000) chain.push_back(c);
  }
  CHECK(chain.size() == clusters);

  // link the chain in both FATs + write data
  for (size_t i = 0; i < chain.size(); i++) {
    const uint16_t c = chain[i];
    const uint16_t next = (i + 1 < chain.size()) ? chain[i + 1] : 0xFFFF;
    for (uint8_t* fat : { fat0, fat1 }) {
      fat[c * 2] = next & 0xff;
      fat[c * 2 + 1] = next >> 8;
    }
    const uint32_t off = i * FAT_SECTOR_SIZE;
    const uint32_t take = size - off < FAT_SECTOR_SIZE ? size - off : FAT_SECTOR_SIZE;
    if (size) memcpy(vol + (FAT_FIRST_DATA_SECTOR + (chain[i] - 2)) * FAT_SECTOR_SIZE,
                     data + off, take);
  }

  // root directory entry in the first free slot
  uint8_t* root = vol + (1 + 2 * FAT_SECTORS_PER_FAT) * FAT_SECTOR_SIZE;
  for (uint32_t i = 0; i < FAT_ROOT_ENTRIES; i++) {
    uint8_t* ent = root + i * 32;
    if (ent[0] == 0x00 || ent[0] == 0xE5) {
      memset(ent, 0, 32);
      memcpy(ent, name83, 11);
      ent[11] = 0x20; // archive
      ent[26] = chain[0] & 0xff; ent[27] = chain[0] >> 8;
      ent[28] = size & 0xff; ent[29] = (size >> 8) & 0xff;
      ent[30] = (size >> 16) & 0xff; ent[31] = (size >> 24) & 0xff;
      return;
    }
  }
  CHECK(false && "root directory full");
}

int main() {
  // ── share-mode state machine ──────────────────────────────────────────────
  {
    ShareStatus st;
    // Can't enter SHARED until the SD is quiesced.
    CHECK(!share_apply(st, ShareEvent::SHARE_REQUEST, /*sd_quiesced=*/false));
    CHECK(st.state == ShareState::NORMAL);
    CHECK(share_apply(st, ShareEvent::SHARE_REQUEST, true));
    CHECK(st.state == ShareState::SHARED);
    CHECK(st.shares_started == 1);
    // Re-request while shared is a no-op.
    CHECK(!share_apply(st, ShareEvent::SHARE_REQUEST, true));
    // Host eject ends the share cleanly.
    CHECK(share_apply(st, ShareEvent::HOST_EJECT, true));
    CHECK(st.state == ShareState::NORMAL && st.shares_ended == 1 && st.forced_ends == 0);
    // Ring pressure forces an end (durability outranks the host).
    CHECK(share_apply(st, ShareEvent::SHARE_REQUEST, true));
    CHECK(share_apply(st, ShareEvent::RING_PRESSURE, true));
    CHECK(st.forced_ends == 1);
    // Events while NORMAL don't fire transitions.
    CHECK(!share_apply(st, ShareEvent::HOST_EJECT, true));
    CHECK(!share_apply(st, ShareEvent::USB_UNPLUG, true));

    // Ring accounting: pressure trips at capacity - headroom.
    ShareStatus st2;
    const uint32_t cap = 16;
    bool pressured = false;
    for (uint32_t i = 0; i < cap - SHARE_RING_HEADROOM; i++)
      pressured = share_note_buffered(st2, cap);
    CHECK(pressured);
    CHECK(st2.buffered_peak == cap - SHARE_RING_HEADROOM);
    share_note_drained(st2);
    CHECK(st2.buffered_records == 0);
  }

  // ── FAT16 volume: format then parse our own work ──────────────────────────
  std::vector<uint8_t> vol(FAT_VOLUME_BYTES);
  fat16_format(vol.data());
  CHECK(vol[510] == 0x55 && vol[511] == 0xAA);

  {
    FatFile files[8];
    const size_t n = fat16_list_root(vol.data(), files, 8);
    CHECK(n == 1); // README.TXT only (volume label is skipped)
    CHECK(strcmp(files[0].name, "README.TXT") == 0);
    CHECK(files[0].size_matches_chain);
    std::vector<uint8_t> txt(files[0].size);
    CHECK(fat16_read_file(vol.data(), files[0], txt.data(), (uint32_t)txt.size()) == files[0].size);
    CHECK(memcmp(txt.data(), "SecuraCV", 8) == 0);
  }

  // ── the update pair policy ────────────────────────────────────────────────
  char reason[96];
  FatFile bin, man;

  // Empty drive (just README) → clear instruction.
  CHECK(!fat16_find_update(vol.data(), &bin, &man, reason, sizeof reason));
  CHECK(strstr(reason, "manifest-*.json") != nullptr);

  // Drop a .bin alone → asks for the manifest (it carries the signature).
  std::vector<uint8_t> image(200000);
  for (size_t i = 0; i < image.size(); i++) image[i] = (uint8_t)(i * 7);
  host_write_file(vol.data(), "CANARY~1BIN", image.data(), (uint32_t)image.size());
  // NB: 8.3 name field is 11 chars: 8 name + 3 ext, no dot.
  CHECK(!fat16_find_update(vol.data(), &bin, &man, reason, sizeof reason));
  CHECK(strstr(reason, "manifest") != nullptr);

  // Drop the manifest too → pair found, bytes read back exactly.
  const char manifest_json[] = "{\"product\":\"securacv-canary-wap\",\"version\":\"2.3.0-wap\"}";
  host_write_file(vol.data(), "MANIFE~1JSO",
                  (const uint8_t*)manifest_json, (uint32_t)sizeof(manifest_json) - 1);
  CHECK(fat16_find_update(vol.data(), &bin, &man, reason, sizeof reason));
  CHECK(strcmp(bin.name, "CANARY~1.BIN") == 0);
  CHECK(strcmp(man.name, "MANIFE~1.JSO") == 0);
  CHECK(bin.size == image.size());
  std::vector<uint8_t> back(image.size());
  CHECK(fat16_read_file(vol.data(), bin, back.data(), (uint32_t)back.size()) == image.size());
  CHECK(memcmp(back.data(), image.data(), image.size()) == 0);
  std::vector<uint8_t> mback(man.size);
  CHECK(fat16_read_file(vol.data(), man, mback.data(), (uint32_t)mback.size()) == man.size);
  CHECK(memcmp(mback.data(), manifest_json, man.size) == 0);

  // A second .bin → refused with "exactly one".
  host_write_file(vol.data(), "OTHER   BIN", image.data(), 512);
  CHECK(!fat16_find_update(vol.data(), &bin, &man, reason, sizeof reason));
  CHECK(strstr(reason, "one") != nullptr);

  // ── corruption resistance ─────────────────────────────────────────────────
  {
    // Fresh volume; a file whose directory size exceeds its FAT chain
    // (interrupted copy) must be flagged and refused.
    std::vector<uint8_t> v2(FAT_VOLUME_BYTES);
    fat16_format(v2.data());
    host_write_file(v2.data(), "TORN    BIN", image.data(), 1024);
    // Truncate its chain: mark the second cluster free in FAT0.
    uint8_t* fat0 = v2.data() + FAT_SECTOR_SIZE;
    // README owns cluster 2; TORN got clusters 3,4 → cut 3's link to 4.
    fat0[3 * 2] = 0xFF; fat0[3 * 2 + 1] = 0xFF; // 3 = EOC → chain shorter than size? no: 1024B needs 2 clusters; now chain=1
    FatFile files[8];
    const size_t n = fat16_list_root(v2.data(), files, 8);
    bool saw_torn = false;
    for (size_t i = 0; i < n; i++) {
      if (strcmp(files[i].name, "TORN.BIN") == 0) {
        saw_torn = true;
        CHECK(!files[i].size_matches_chain);
      }
    }
    CHECK(saw_torn);

    // A looped FAT chain must terminate, not hang.
    std::vector<uint8_t> v3(FAT_VOLUME_BYTES);
    fat16_format(v3.data());
    host_write_file(v3.data(), "LOOP    BIN", image.data(), 2048);
    uint8_t* f3 = v3.data() + FAT_SECTOR_SIZE;
    // Point cluster 4 back at 3 (README has 2; LOOP has 3..6).
    f3[4 * 2] = 3; f3[4 * 2 + 1] = 0;
    FatFile lf{};
    strcpy(lf.name, "LOOP.BIN"); lf.size = 2048; lf.first_cluster = 3;
    std::vector<uint8_t> sink(4096);
    CHECK(fat16_read_file(v3.data(), lf, sink.data(), (uint32_t)sink.size()) == 0);
  }

  // Oversized file claim → refused before any read.
  {
    std::vector<uint8_t> v4(FAT_VOLUME_BYTES);
    fat16_format(v4.data());
    host_write_file(v4.data(), "HUGE    BIN", image.data(), 4096);
    // Lie about the size in the directory entry.
    uint8_t* root = v4.data() + (1 + 2 * FAT_SECTORS_PER_FAT) * FAT_SECTOR_SIZE;
    for (uint32_t i = 0; i < FAT_ROOT_ENTRIES; i++) {
      uint8_t* ent = root + i * 32;
      if (memcmp(ent, "HUGE    BIN", 11) == 0) {
        const uint32_t lie = FAT_MAX_FILE_BYTES + 1;
        ent[28] = lie & 0xff; ent[29] = (lie >> 8) & 0xff;
        ent[30] = (lie >> 16) & 0xff; ent[31] = (lie >> 24) & 0xff;
      }
    }
    host_write_file(v4.data(), "MANIFE~1JSO",
                    (const uint8_t*)manifest_json, (uint32_t)sizeof(manifest_json) - 1);
    FatFile b2, m2;
    CHECK(!fat16_find_update(v4.data(), &b2, &m2, reason, sizeof reason));
  }

  // ── RESULT.TXT write-back ─────────────────────────────────────────────────
  {
    std::vector<uint8_t> v5(FAT_VOLUME_BYTES);
    fat16_format(v5.data());
    CHECK(fat16_write_result(v5.data(), "the manifest signature does not verify"));
    FatFile files[8];
    const size_t n = fat16_list_root(v5.data(), files, 8);
    FatFile* res = nullptr;
    for (size_t i = 0; i < n; i++)
      if (strcmp(files[i].name, "RESULT.TXT") == 0) res = &files[i];
    CHECK(res && res->size_matches_chain);
    std::vector<uint8_t> txt(res->size);
    CHECK(fat16_read_file(v5.data(), *res, txt.data(), (uint32_t)txt.size()) == res->size);
    CHECK(memcmp(txt.data(), "the manifest", 12) == 0);
    // Rewriting reuses the same entry (no directory litter).
    CHECK(fat16_write_result(v5.data(), "verified - installing on reboot"));
    FatFile files2[8];
    size_t results = 0;
    const size_t n2 = fat16_list_root(v5.data(), files2, 8);
    for (size_t i = 0; i < n2; i++)
      if (strcmp(files2[i].name, "RESULT.TXT") == 0) results++;
    CHECK(results == 1);
  }

  printf("test_evidence_drive_logic: %d checks passed\n", checks);
  return 0;
}
