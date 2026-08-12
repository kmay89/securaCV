// test_fleet_figures_art.cpp — the on-glass figure art tier
// (common/core/fleet_figures_art.h) stays in lockstep with the lookups
// (common/core/fleet_figures.h) and stays drawable.
//
// The contract under test:
//   1. COVERAGE — every figure id the firmware can RESOLVE (each kFigures and
//      kHardware row) has art. A device that names itself and then draws
//      nothing is the regression this pins out.
//   2. REV LOCKSTEP — the art's rev matches the lookup's rev for the same id,
//      so a cached drawing can never be one generator run behind its identity.
//   3. GEOMETRY SANITY — every triangle vertex lands inside the declared art
//      box, every face has at least one triangle, no face is fully
//      degenerate. A renderer with no tessellator has no second chance at
//      malformed data.
//   4. HONESTY — art is never emitted for an "idea": ideas render as ghosts
//      on every other surface, and the glass simply doesn't draw them.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/fleet_figures.h"
#include "core/fleet_figures_art.h"

static int g_fail = 0;

#define CHECK(cond, ...)                              \
  do {                                                \
    if (!(cond)) {                                    \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                       \
      std::printf("\n");                              \
      g_fail++;                                       \
    }                                                 \
  } while (0)

using namespace canary::figures;

static const FigureArt* must_art(const char* figure_id, const char* rev) {
  const FigureArt* art = figure_art(figure_id);
  CHECK(art != nullptr, "no art for resolvable figure %s", figure_id);
  if (art) {
    CHECK(figure_streq(art->rev, rev),
          "art rev %s != lookup rev %s for %s", art->rev, rev, figure_id);
  }
  return art;
}

int main() {
  // 1 + 2: coverage and rev lockstep for both lookups.
  for (size_t i = 0; i < kFigureCount; i++) {
    must_art(kFigures[i].figure_id, kFigures[i].rev);
    CHECK(!figure_streq(kFigures[i].confidence, "idea"),
          "kFigures carries an idea: %s", kFigures[i].figure_id);
  }
  for (size_t i = 0; i < kHardwareCount; i++) {
    must_art(kHardware[i].figure_id, kHardware[i].rev);
    CHECK(!figure_streq(kHardware[i].confidence, "idea"),
          "kHardware carries an idea: %s", kHardware[i].figure_id);
  }

  // 3: geometry sanity across the whole art table.
  for (size_t i = 0; i < kFigureArtCount; i++) {
    const FigureArt& a = kFigureArt[i];
    CHECK(a.face_count > 0, "%s has no faces", a.figure_id);
    for (uint16_t f = 0; f < a.face_count; f++) {
      const ArtFace& face = a.faces[f];
      CHECK(face.tri_count > 0, "%s face %u has no triangles", a.figure_id, f);
      CHECK(face.rgb <= 0xFFFFFF, "%s face %u color out of range", a.figure_id, f);
      long area2_sum = 0;
      for (uint16_t t = 0; t < face.tri_count; t++) {
        const int16_t* v = face.tris + t * 6;
        for (int k = 0; k < 6; k++) {
          CHECK(v[k] >= 0 && v[k] <= kFigureArtScale,
                "%s face %u tri %u coord %d out of the art box (%d)",
                a.figure_id, f, t, k, (int)v[k]);
        }
        const long ax = v[0], ay = v[1], bx = v[2], by = v[3], cx = v[4], cy = v[5];
        const long area2 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        area2_sum += area2 < 0 ? -area2 : area2;
      }
      CHECK(area2_sum > 0, "%s face %u is fully degenerate", a.figure_id, f);
    }
  }

  // 4: no idea ever gets art. The lookups above are already idea-free; this
  // checks the art table itself never grows one by accident.
  for (size_t i = 0; i < kFigureArtCount; i++) {
    bool resolvable = false;
    for (size_t j = 0; j < kFigureCount; j++) {
      if (figure_streq(kFigures[j].figure_id, kFigureArt[i].figure_id)) resolvable = true;
    }
    for (size_t j = 0; j < kHardwareCount; j++) {
      if (figure_streq(kHardware[j].figure_id, kFigureArt[i].figure_id)) resolvable = true;
    }
    CHECK(resolvable, "art for %s, which nothing can resolve — flash spent on nothing",
          kFigureArt[i].figure_id);
  }

  // The lookup helper's own honesty: unknown and empty ids draw nothing.
  CHECK(figure_art("device.no-such-thing") == nullptr, "unknown id must be nullptr");
  CHECK(figure_art("") == nullptr, "empty id must be nullptr");
  CHECK(figure_art(nullptr) == nullptr, "null id must be nullptr");

  if (g_fail) {
    std::printf("test_fleet_figures_art: %d FAILURE(S)\n", g_fail);
    return 1;
  }
  std::printf("test_fleet_figures_art: all checks passed "
              "(%zu figures with art)\n", kFigureArtCount);
  return 0;
}
