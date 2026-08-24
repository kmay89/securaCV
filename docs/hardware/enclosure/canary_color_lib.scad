// ============================================================================
//  Canary COLOR library — the catalog's colorways, one home
//
//  NOT A PRINTABLE PART. `use <canary_color_lib.scad>` from a case file.
//
//  A Canary is printed from up to three spools, and the catalog already
//  names their ROLES (the 7"/C3/stick tri-color system, gen_3mf.py's slot
//  order): BODY is the case, INK is the contrasting detail — the mark, the
//  lettering, the one accent element a face is allowed to spend — and
//  LIGHT is the white that light bands and pipes glow through. What the
//  catalog never had is the PALETTE: which actual colors those roles take.
//  So the colors forked the way un-homed numbers always do — the website's
//  showroom settled on one graphite and one canary yellow, the AR models
//  hand-mixed a different charcoal, and the C3's preview yellow matched
//  nobody. This registry is where a colorway now lives; every consumer
//  (scad previews, the builder manifest carry, the website's AR variant
//  picker and showroom finishes) reads it from here.
//
//  DOCTRINE:
//    * ONE ACCENT SPEND. A face carries one ink element — a hairline, a
//      button ring, a wordmark — not a costume. The C3's band_notch note
//      says why: a controlled line reads as design, a second one as a
//      defect.
//    * INK CONTRASTS, LIGHT IS LIGHT. The self-check enforces both: a
//      colorway whose ink disappears into its body is not a colorway, and
//      a light role that is not near-white prints a light band that reads
//      switched-off (the C3's corners-went-dark print is the lesson).
//    * VALUES ARE sRGB HEX, lowercase, six digits — the human-facing form
//      every consumer converts from EXPLICITLY: OpenSCAD color() takes
//      sRGB 0..1 (cw_body() et al.), glTF baseColorFactor wants LINEAR
//      (the website's colorways module owns that conversion). Storing one
//      form and converting on use is what keeps "the same graphite" true
//      across a preview, a glb and a spool order.
//    * The registry names PREVIEW/RENDER truth. Real spools vary by vendor
//      and batch; gen_3mf.py's slot order is still what decides which
//      filament prints which role on the day.
//
//  `use<>` does not run top-level statements: call color_selfcheck() from
//  an adopter (the fit coupon does).
// ============================================================================

// ---------------------------------------------------------------------------
//  The registry.  [id, display name, body_hex, ink_hex, light_hex, note]
//  Order matters: the first row is the catalog default (what the site's AR
//  models open with, and what phone-AR handoffs show).
// ---------------------------------------------------------------------------
CW_REGISTRY = [
    ["graphite", "Graphite", "41454e", "f0b400", "f2f2f2",
     "the classic: charcoal body, canary ink — the showroom and AR default"],
    ["canary",   "Canary",   "f0b400", "1a1a1a", "f2f2f2",
     "the bold one — black-on-yellow ink is the C3's print-validated pairing"],
    ["snow",     "Snow",     "f2f1ec", "41454e", "f2f2f2",
     "gallery white; graphite ink keeps the deboss legible on a light body"],
    ["forest",   "Forest",   "2f4a3a", "f0b400", "f2f2f2",
     "outdoor green; canary ink survives the dark body"],
    ["midnight", "Midnight", "1a1a1a", "f0b400", "f2f2f2",
     "the stick's print-validated near-black body with canary ink"],
];

// ---------------------------------------------------------------------------
//  Hex plumbing — lowercase six-digit sRGB to the 0..1 triple color() wants
// ---------------------------------------------------------------------------
function _cw_nib(c)     = search(c, "0123456789abcdef")[0];
function _cw_byte(h, i) = 16*_cw_nib(h[i]) + _cw_nib(h[i + 1]);
function cw_rgb(hex)    = [_cw_byte(hex, 0), _cw_byte(hex, 2), _cw_byte(hex, 4)] / 255;
// approximate relative luminance, straight off the sRGB values — coarse on
// purpose: it gates contrast classes, it does not do colorimetry
function _cw_lum(rgb)   = 0.2126*rgb[0] + 0.7152*rgb[1] + 0.0722*rgb[2];

// ---------------------------------------------------------------------------
//  Accessors
// ---------------------------------------------------------------------------
function _cw_find(id, i = 0) =
    i >= len(CW_REGISTRY)
        ? assert(false, str("colorway registry: unknown id \"", id, "\"")) undef
        : CW_REGISTRY[i][0] == id ? CW_REGISTRY[i] : _cw_find(id, i + 1);

function cw_ids()          = [for (r = CW_REGISTRY) r[0]];
function cw_default()      = CW_REGISTRY[0][0];
function cw_name(id)       = _cw_find(id)[1];
function cw_body_hex(id)   = _cw_find(id)[2];
function cw_ink_hex(id)    = _cw_find(id)[3];
function cw_light_hex(id)  = _cw_find(id)[4];
function cw_note(id)       = _cw_find(id)[5];
function cw_body(id)       = cw_rgb(cw_body_hex(id));    // sRGB 0..1, for color()
function cw_ink(id)        = cw_rgb(cw_ink_hex(id));
function cw_light(id)      = cw_rgb(cw_light_hex(id));

// ---------------------------------------------------------------------------
//  Self-check — the doctrine's own arithmetic. Call once from an adopter
//  (the fit coupon does).
// ---------------------------------------------------------------------------
module color_selfcheck() {
    for (i = [0 : len(CW_REGISTRY) - 1]) {
        r = CW_REGISTRY[i];
        assert(len(r) == 6, str("colorway registry: record ", i, " malformed"));
        for (j = [0 : len(CW_REGISTRY) - 1])
            assert(j == i || CW_REGISTRY[j][0] != r[0],
                   str("colorway registry: duplicate id \"", r[0], "\""));
        for (h = [r[2], r[3], r[4]]) {
            assert(len(h) == 6,
                   str("colorway \"", r[0], "\": hex \"", h, "\" is not six digits"));
            for (k = [0 : 5])
                assert(_cw_nib(h[k]) != undef,
                       str("colorway \"", r[0], "\": hex \"", h,
                           "\" is not lowercase hex"));
        }
        // ink must contrast the body it details, or the mark vanishes
        assert(abs(_cw_lum(cw_rgb(r[2])) - _cw_lum(cw_rgb(r[3]))) >= 0.25,
               str("colorway \"", r[0], "\": ink does not contrast the body — ",
                   "a mark printed in it disappears"));
        // the light role glows through printed diffusers — it must be light
        assert(_cw_lum(cw_rgb(r[4])) >= 0.80,
               str("colorway \"", r[0], "\": the light role must be near-white ",
                   "or the band reads switched-off (the C3's dark-corners print)"));
    }
    assert(cw_default() == "graphite",
           "colorway registry: graphite is the catalog default — reorder deliberately or not at all");
    echo("canary_color_lib: self-check OK");
}
