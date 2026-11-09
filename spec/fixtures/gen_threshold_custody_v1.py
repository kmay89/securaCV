#!/usr/bin/env python3
"""Write spec/fixtures/threshold_custody_v1.json, the known-answer vectors of
the threshold-custody core (spec/quorum_unseal_v2.md §2; design record
docs/design/threshold_custody.md).

Python 3 standard library only, and an implementation of its own: field
arithmetic mod 2^255 - 19, ristretto255 (encode, decode, the one-way map),
scalars mod l, X25519, ChaCha20-Poly1305, HKDF-SHA256, HPKE (RFC 9180,
single shot, base and PSK modes, export), Ed25519 signing, and the custody
formats (Pedersen commitments, SHR1 share files, the MKC1 container, TEK1,
recipient and split-record JSON). None of it is a port of the Rust code; the
Rust known-answer tests (src/vault/custody/kat.rs) compare the Rust
implementation against the file this one writes. A disagreement is a
stop-ship, never a fixture edit.

Before anything is written the generator checks itself (exit 2 on failure):
curve25519-dalek 5.0.0's table of the encodings of i*G (i = 0..15), dalek's
Elligator and one-way-map vectors (the latter from
draft-irtf-cfrg-ristretto255-decaf448-04 Appendix A.3), the HPKE editor's
DHKEM(X25519, HKDF-SHA256)/HKDF-SHA256/ChaCha20Poly1305 Base and PSK vectors,
and RFC 8032 section 7.1 test 1.

    python3 -I spec/fixtures/gen_threshold_custody_v1.py           # write
    python3 -I spec/fixtures/gen_threshold_custody_v1.py --check   # compare

--check regenerates in memory, compares bytes, prints the first differing
top-level key and exits 1 on any difference.
"""

import argparse
import hashlib
import hmac
import json
import os
import struct
import sys

OUT_NAME = "threshold_custody_v1.json"

# ---------------------------------------------------------------------------
# The field, the curve and ristretto255.
# The constants are RFC 9496 section 4.1's (copied as integers from
# curve25519-dalek 5.0.0, src/backend/serial/u64/constants.rs; the RFC itself
# was not reachable from the machine this was written on). Each is checked
# against its defining equation below, and the one-way map is checked against
# the draft's own vectors, which only the odd SQRT_AD_MINUS_ONE reproduces.
# ---------------------------------------------------------------------------

P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
D = 37095705934669439343138083508754565189542113879843219016388785533085940283555
SQRT_M1 = 19681161376707505956807079304988542015446066515923890162744021073123829784752
SQRT_AD_MINUS_ONE = 25063068953384623474111414158702152701244531502492656460079210482610430750235
INVSQRT_A_MINUS_D = 54469307008909316920995813868745141605393597292927456921205312896311721017578
ONE_MINUS_D_SQ = 1159843021668779879193775521855586647937357759715417654439879720876111806838
D_MINUS_ONE_SQ = 40440834346308536858101042469323190826248399146238708352240133220865137265952


def fe_inv(x):
    return pow(x, P - 2, P)


def is_negative(x):
    return (x % P) & 1


def ct_abs(x):
    x %= P
    return (P - x) % P if is_negative(x) else x


def sqrt_ratio_m1(u, v):
    """RFC 9496 SQRT_RATIO_M1: (was_square, the non-negative root)."""
    u %= P
    v %= P
    v3 = v * v % P * v % P
    v7 = v3 * v3 % P * v % P
    r = u * v3 % P * pow(u * v7 % P, (P - 5) // 8, P) % P
    check = v * r % P * r % P
    correct = check == u
    flipped = check == (-u) % P
    flipped_i = check == (-u * SQRT_M1) % P
    if flipped or flipped_i:
        r = r * SQRT_M1 % P
    return (correct or flipped), ct_abs(r)


def check_constants():
    assert D == (-121665 * fe_inv(121666)) % P
    assert SQRT_M1 * SQRT_M1 % P == P - 1
    assert SQRT_AD_MINUS_ONE * SQRT_AD_MINUS_ONE % P == (-D - 1) % P
    assert is_negative(SQRT_AD_MINUS_ONE), "RFC 9496 lists the odd root"
    assert INVSQRT_A_MINUS_D * INVSQRT_A_MINUS_D % P * ((-1 - D) % P) % P == 1
    assert ONE_MINUS_D_SQ == (1 - D * D) % P
    assert D_MINUS_ONE_SQ == (D - 1) * (D - 1) % P


# Extended twisted Edwards coordinates (X, Y, Z, T), a = -1.
IDENTITY = (0, 1, 1, 0)


def pt_add(p1, p2):
    x1, y1, z1, t1 = p1
    x2, y2, z2, t2 = p2
    a = (y1 - x1) * (y2 - x2) % P
    b = (y1 + x1) * (y2 + x2) % P
    c = 2 * D * t1 % P * t2 % P
    d = 2 * z1 * z2 % P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def pt_mul(k, point):
    """k * point by double-and-add (public tooling: not constant time)."""
    k %= L
    acc = IDENTITY
    for bit in bin(k)[2:] if k else "":
        acc = pt_add(acc, acc)
        if bit == "1":
            acc = pt_add(acc, point)
    return acc


def _recover_x(y):
    x2 = (y * y - 1) * fe_inv(D * y * y + 1) % P
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * SQRT_M1 % P
    if x & 1:
        x = P - x
    return x


_BY = 4 * fe_inv(5) % P
_BX = _recover_x(_BY)
BASE = (_BX, _BY, 1, _BX * _BY % P)


def r_encode(point):
    """RFC 9496 section 4.3.2 ENCODE."""
    x0, y0, z0, t0 = point
    u1 = (z0 + y0) * (z0 - y0) % P
    u2 = x0 * y0 % P
    _, invsqrt = sqrt_ratio_m1(1, u1 * u2 % P * u2 % P)
    den1 = invsqrt * u1 % P
    den2 = invsqrt * u2 % P
    z_inv = den1 * den2 % P * t0 % P
    ix0 = x0 * SQRT_M1 % P
    iy0 = y0 * SQRT_M1 % P
    enchanted = den1 * INVSQRT_A_MINUS_D % P
    rotate = is_negative(t0 * z_inv)
    x, y = (iy0, ix0) if rotate else (x0, y0)
    den_inv = enchanted if rotate else den2
    if is_negative(x * z_inv):
        y = (-y) % P
    s = ct_abs(den_inv * (z0 - y))
    return s.to_bytes(32, "little")


def r_decode(data):
    """RFC 9496 section 4.3.1 DECODE: None for a non-canonical or invalid
    encoding."""
    s = int.from_bytes(data, "little")
    if s >= P or is_negative(s):
        return None
    ss = s * s % P
    u1 = (1 - ss) % P
    u2 = (1 + ss) % P
    u2_sqr = u2 * u2 % P
    v = (-(D * u1 % P * u1) - u2_sqr) % P
    was_square, invsqrt = sqrt_ratio_m1(1, v * u2_sqr % P)
    den_x = invsqrt * u2 % P
    den_y = invsqrt * den_x % P * v % P
    x = ct_abs(2 * s * den_x)
    y = u1 * den_y % P
    t = x * y % P
    if not was_square or is_negative(t) or y == 0:
        return None
    return (x, y, 1, t)


def r_map(t):
    """RFC 9496 section 4.3.4 MAP."""
    r = SQRT_M1 * t % P * t % P
    u = (r + 1) * ONE_MINUS_D_SQ % P
    v = (-1 - r * D) * (r + D) % P
    was_square, s = sqrt_ratio_m1(u, v)
    s_prime = (-ct_abs(s * t)) % P
    if not was_square:
        s = s_prime
    c = (P - 1) if was_square else r
    n = (c * (r - 1) % P * D_MINUS_ONE_SQ - v) % P
    w0 = 2 * s * v % P
    w1 = n * SQRT_AD_MINUS_ONE % P
    w2 = (1 - s * s) % P
    w3 = (1 + s * s) % P
    return (w0 * w3 % P, w2 * w1 % P, w1 * w3 % P, w0 * w2 % P)


def from_uniform(b64):
    """The one-way map of 64 uniform bytes (each half's low 255 bits)."""
    mask = (1 << 255) - 1
    t1 = int.from_bytes(b64[:32], "little") & mask
    t2 = int.from_bytes(b64[32:], "little") & mask
    return pt_add(r_map(t1 % P), r_map(t2 % P))


GENERATOR_H_LABEL = b"securacv:custody:pedersen-generator-H:v1"
H = from_uniform(hashlib.sha512(GENERATOR_H_LABEL).digest())


def commit(a, b):
    return pt_add(pt_mul(a, BASE), pt_mul(b, H))


def scalar_bytes(x):
    return (x % L).to_bytes(32, "little")


def scalar_hex(x):
    return scalar_bytes(x).hex()

# ---------------------------------------------------------------------------
# X25519 (RFC 7748), ChaCha20-Poly1305 (RFC 8439), HKDF-SHA256 (RFC 5869).
# ---------------------------------------------------------------------------


def x25519(k_bytes, u_bytes):
    k = bytearray(k_bytes)
    k[0] &= 248
    k[31] &= 127
    k[31] |= 64
    kn = int.from_bytes(k, "little")
    u = int.from_bytes(u_bytes, "little") & ((1 << 255) - 1)
    x1, x2, z2, x3, z3, swap = u, 1, 0, u, 1, 0
    for t in reversed(range(255)):
        kt = (kn >> t) & 1
        swap ^= kt
        if swap:
            x2, x3, z2, z3 = x3, x2, z3, z2
        swap = kt
        a = (x2 + z2) % P
        aa = a * a % P
        b = (x2 - z2) % P
        bb = b * b % P
        e = (aa - bb) % P
        c = (x3 + z3) % P
        d = (x3 - z3) % P
        da = d * a % P
        cb = c * b % P
        x3 = (da + cb) * (da + cb) % P
        z3 = x1 * (da - cb) % P * (da - cb) % P
        x2 = aa * bb % P
        z2 = e * (aa + 121665 * e) % P
    if swap:
        x2, x3, z2, z3 = x3, x2, z3, z2
    return (x2 * fe_inv(z2) % P).to_bytes(32, "little")


X25519_BASE = (9).to_bytes(32, "little")


def _rotl(v, c):
    return ((v << c) & 0xFFFFFFFF) | (v >> (32 - c))


def _quarter(s, a, b, c, d):
    s[a] = (s[a] + s[b]) & 0xFFFFFFFF
    s[d] = _rotl(s[d] ^ s[a], 16)
    s[c] = (s[c] + s[d]) & 0xFFFFFFFF
    s[b] = _rotl(s[b] ^ s[c], 12)
    s[a] = (s[a] + s[b]) & 0xFFFFFFFF
    s[d] = _rotl(s[d] ^ s[a], 8)
    s[c] = (s[c] + s[d]) & 0xFFFFFFFF
    s[b] = _rotl(s[b] ^ s[c], 7)


def chacha20_block(key, counter, nonce):
    state = (
        [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574]
        + list(struct.unpack("<8I", key))
        + [counter]
        + list(struct.unpack("<3I", nonce))
    )
    w = state[:]
    for _ in range(10):
        _quarter(w, 0, 4, 8, 12)
        _quarter(w, 1, 5, 9, 13)
        _quarter(w, 2, 6, 10, 14)
        _quarter(w, 3, 7, 11, 15)
        _quarter(w, 0, 5, 10, 15)
        _quarter(w, 1, 6, 11, 12)
        _quarter(w, 2, 7, 8, 13)
        _quarter(w, 3, 4, 9, 14)
    return struct.pack("<16I", *[(w[i] + state[i]) & 0xFFFFFFFF for i in range(16)])


def chacha20_xor(key, counter, nonce, data):
    out = bytearray()
    for i in range(0, len(data), 64):
        stream = chacha20_block(key, counter + i // 64, nonce)
        out += bytes(x ^ y for x, y in zip(data[i : i + 64], stream))
    return bytes(out)


def poly1305(key, msg):
    r = int.from_bytes(key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(key[16:], "little")
    acc = 0
    p130 = 2**130 - 5
    for i in range(0, len(msg), 16):
        n = int.from_bytes(msg[i : i + 16] + b"\x01", "little")
        acc = (acc + n) * r % p130
    return ((acc + s) % 2**128).to_bytes(16, "little")


def _pad16(b):
    return b"\x00" * (-len(b) % 16)


def _aead_tag(key, nonce, aad, ct):
    otk = chacha20_block(key, 0, nonce)[:32]
    mac_data = aad + _pad16(aad) + ct + _pad16(ct) + struct.pack("<QQ", len(aad), len(ct))
    return poly1305(otk, mac_data)


def aead_seal(key, nonce, aad, pt):
    ct = chacha20_xor(key, 1, nonce, pt)
    return ct + _aead_tag(key, nonce, aad, ct)


def aead_open(key, nonce, aad, sealed):
    ct, tag = sealed[:-16], sealed[-16:]
    if not hmac.compare_digest(_aead_tag(key, nonce, aad, ct), tag):
        return None
    return chacha20_xor(key, 1, nonce, ct)


def hkdf_extract(salt, ikm):
    return hmac.new(salt if salt else b"\x00" * 32, ikm, hashlib.sha256).digest()


def hkdf_expand(prk, info, length):
    out, t, i = b"", b"", 1
    while len(out) < length:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        out += t
        i += 1
    return out[:length]


def hkdf(salt, ikm, info, length):
    return hkdf_expand(hkdf_extract(salt, ikm), info, length)


# ---------------------------------------------------------------------------
# HPKE: DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, ChaCha20Poly1305; single
# shot at sequence number 0, base (0x00) and PSK (0x01) modes, and Export.
# ---------------------------------------------------------------------------

KEM_SUITE = b"KEM" + struct.pack(">H", 0x0020)
HPKE_SUITE = b"HPKE" + struct.pack(">HHH", 0x0020, 0x0001, 0x0003)


def labeled_extract(suite, salt, label, ikm):
    return hkdf_extract(salt, b"HPKE-v1" + suite + label + ikm)


def labeled_expand(suite, prk, label, info, length):
    return hkdf_expand(prk, struct.pack(">H", length) + b"HPKE-v1" + suite + label + info, length)


def derive_keypair(ikm):
    prk = labeled_extract(KEM_SUITE, b"", b"dkp_prk", ikm)
    sk = labeled_expand(KEM_SUITE, prk, b"sk", b"", 32)
    return sk, x25519(sk, X25519_BASE)


def _dh(sk, pk):
    out = x25519(sk, pk)
    if out == bytes(32):
        raise ValueError("all-zero X25519 output")
    return out


def _kem_shared(dh, enc, pk_r):
    prk = labeled_extract(KEM_SUITE, b"", b"eae_prk", dh)
    return labeled_expand(KEM_SUITE, prk, b"shared_secret", enc + pk_r, 32)


def encap_derand(pk_r, ikm_e):
    sk_e, pk_e = derive_keypair(ikm_e)
    return _kem_shared(_dh(sk_e, pk_r), pk_e, pk_r), pk_e


def decap(enc, sk_r):
    return _kem_shared(_dh(sk_r, enc), enc, x25519(sk_r, X25519_BASE))


def key_schedule(mode, shared_secret, info, psk=b"", psk_id=b""):
    assert (len(psk) > 0) == (len(psk_id) > 0) == (mode == 1)
    ctx = (
        bytes([mode])
        + labeled_extract(HPKE_SUITE, b"", b"psk_id_hash", psk_id)
        + labeled_extract(HPKE_SUITE, b"", b"info_hash", info)
    )
    secret = labeled_extract(HPKE_SUITE, shared_secret, b"secret", psk)
    return {
        "key": labeled_expand(HPKE_SUITE, secret, b"key", ctx, 32),
        "base_nonce": labeled_expand(HPKE_SUITE, secret, b"base_nonce", ctx, 12),
        "exporter_secret": labeled_expand(HPKE_SUITE, secret, b"exp", ctx, 32),
    }


def hpke_export(ctx, exporter_context, length):
    return labeled_expand(HPKE_SUITE, ctx["exporter_secret"], b"sec", exporter_context, length)


def seal_base_derand(pk_r, info, aad, pt, ikm_e):
    ss, enc = encap_derand(pk_r, ikm_e)
    ctx = key_schedule(0, ss, info)
    return enc, aead_seal(ctx["key"], ctx["base_nonce"], aad, pt)


def seal_psk_derand(pk_r, info, aad, pt, psk, psk_id, ikm_e):
    ss, enc = encap_derand(pk_r, ikm_e)
    ctx = key_schedule(1, ss, info, psk, psk_id)
    return enc, aead_seal(ctx["key"], ctx["base_nonce"], aad, pt)


def open_hpke(mode, sk_r, enc, info, aad, ct, psk=b"", psk_id=b""):
    ctx = key_schedule(mode, decap(enc, sk_r), info, psk, psk_id)
    return aead_open(ctx["key"], ctx["base_nonce"], aad, ct)


# ---------------------------------------------------------------------------
# Ed25519 signing (RFC 8032 section 5.1.6), over the same Edwards arithmetic.
# ---------------------------------------------------------------------------


def ed_encode(point):
    x, y, z, _ = point
    zi = fe_inv(z)
    x = x * zi % P
    y = y * zi % P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def ed25519_public(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return ed_encode(pt_mul(a, BASE))


def ed25519_sign(seed, msg):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    pub = ed_encode(pt_mul(a, BASE))
    r = int.from_bytes(hashlib.sha512(h[32:] + msg).digest(), "little") % L
    big_r = ed_encode(pt_mul(r, BASE))
    k = int.from_bytes(hashlib.sha512(big_r + pub + msg).digest(), "little") % L
    return big_r + ((r + k * a) % L).to_bytes(32, "little")


def domain_separated_hash(domain, msg32):
    """The kernel's signing hash: SHA-256(u32le(len domain) || domain || msg)."""
    d = domain.encode()
    return hashlib.sha256(struct.pack("<I", len(d)) + d + msg32).digest()


def sign_domain(seed, domain, msg32):
    return ed25519_sign(seed, domain_separated_hash(domain, msg32))

# ---------------------------------------------------------------------------
# The vectors the generator checks itself against before writing anything.
# ---------------------------------------------------------------------------

# Encodings of i*G, i = 0..15: curve25519-dalek 5.0.0 src/ristretto.rs:1389-1455
# (its comment: "Generated using ristretto.sage").
SMALL_MULTIPLES = [
    "0000000000000000000000000000000000000000000000000000000000000000",
    "e2f2ae0a6abc4e71a884a961c500515f58e30b6aa582dd8db6a65945e08d2d76",
    "6a493210f7499cd17fecb510ae0cea23a110e8d5b901f8acadd3095c73a3b919",
    "94741f5d5d52755ece4f23f044ee27d5d1ea1e2bd196b462166b16152a9d0259",
    "da80862773358b466ffadfe0b3293ab3d9fd53c5ea6c955358f568322daf6a57",
    "e882b131016b52c1d3337080187cf768423efccbb517bb495ab812c4160ff44e",
    "f64746d3c92b13050ed8d80236a7f0007c3b3f962f5ba793d19a601ebb1df403",
    "44f53520926ec81fbd5a387845beb7df85a96a24ece18738bdcfa6a7822a176d",
    "903293d8f2287ebe10e2374dc1a53e0bc887e592699f02d077d5263cdd55601c",
    "02622ace8f7303a31cafc63f8fc48fdc16e1c8c8d234b2f0d6685282a9076031",
    "20706fd788b2720a1ed2a5dad4952b01f413bcf0e7564de8cdc816689e2db95f",
    "bce83f8ba5dd2fa572864c24ba1810f9522bc6004afe95877ac73241cafdab42",
    "e4549ee16b9aa03099ca208c67adafcafa4c3f3e4e5303de6026e3ca8ff84460",
    "aa52e000df2e16f55fb1032fc33bc42742dad6bd5a8fc0be0167436c5948501f",
    "46376b80f409b29dc2b5f6f0c52591990896e5716f41477cd30085ab7f10301e",
    "e0c418f7c8d9c4cdd7395b93ea124f3ad99021bb681dfc3302a9d99a2e53e64e",
]

# Elligator (MAP of one field element): curve25519-dalek 5.0.0
# src/ristretto/elligator.rs, test elligator_vs_ristretto_sage.
ELLIGATOR_VECTORS = [
    ("b8f98731fd7b597143a006ef0769d329c0f9b939096646c60f7f071aa0668647",
     "b09ded61421d8ca6a85e1a9dd4d8e5a0c3f6e8efa9703fc1402098450bbef656"),
    ("e50ef1e34b09763c8099e215b7d95b886200e79c7c4d528b8e86a4a9a93efa34",
     "ea8d4dcbb5e1fa4aab3e0f764ed49613830ebceec2f48d8aa6a2537ae4c9131a"),
    ("736d24dcb4df6306cca9131da9445417156dbd957fcd5b66ac2370238645ba22",
     "e8e7335c05a85024adb36844ba9544288caa1b67638c15f22b3efa86d0ff3d59"),
    ("1031606babc7a4098110403ef13f84add1a070d769329d51fd69019ae5197853",
     "d0788c81b1b3ed9ffca01c0dce05d3f1c0da016182f114a9772ef61d4f504d54"),
    ("9c83a1a2ecfb05bba7ab11b294d25acf56154fa1a7d7ea0188f2b6f826554f56",
     "ca0bec913a0cb59dd106d5584b930b77bf8b2f8e212499c1dfb7b208cd78f86e"),
    ("fbb17c3612654bebf5ba132e859de5400a88b5b94e90fea789316b0a3d0a1519",
     "1a42e743cbaf748220883efdd72e05d6a6f86cedd847f4ad488552068ff06829"),
    ("e8c11444f04dba4db7282c56961fc6d44c5103d9c5087e807e98a4d0992cbd4d",
     "289d6660c9dfc8c596b56a53677e8f2191e64e06ab92d28f7005f517b78a1278"),
    ("ade595b125e61e453d38acbedb73a7c247863b4b1cf4761aa26140100fbd1e40",
     "dc251bcbefc4b0832542bcf3b9fa7117a7d39af3a8d736ab9f24c3510d962b2b"),
    ("6a473d6bfa752a975bcad46434bcbe157dda1f12fdf1a08539f203a4bd446f4b",
     "e879b0deb7c49f5aeec1693465a7f4aa7972c406439850b9dd075369b0d0e079"),
    ("70ccb65adcc67849ad6bc111e328a224968deb37acb70c27c2882b99f4765b59",
     "e2b5b734f1a33db3ddcfdc49f5f219ec4354b3dea73ea7b620095c1ea57fcc44"),
    ("6f18cb7bfebd0ba233c4a388cc8f0ade217051cd222308425a06a43aaab12219",
     "e27710f2c88bf0570bde5c929cf32e77413b01f85cb732af5728ce35d0dc940d"),
    ("e1b71e34ec5206b76d19e3b5195229c1504da150f2cb4fcc88f5836eed6a033a",
     "46f04f70369de4924a7ad858e83e9e0d0e927375b0de5ae1f4175ebe96078860"),
    ("cff626381e56b05a1bc83d2add1b38d24fb2bd7844c178a74db935c57c80bf7e",
     "1647f1672dc1c390b7659a322744316e332c3e00e5714851a81d496a66288418"),
    ("0188d750f02e3f9310f4e6cf52bd4a326aa98a561e83d6caa67dfbe462182415",
     "c4856b0b82694a21ccab85ddaec1f12426b3c46bdbb9b5fde42f9b2ae749294e"),
    ("d2cfe4389b74cf3654c3fbd7f9c7744b6defc4fbc2f6fce446929c231927f104",
     "3affe1c573d0a08f27c552458feb5caa4a28390babe31ab9d9cf5ab9c5be233c"),
    ("22747b0908285dbd0967396742e303029d6b86dbca4ae69a4e6bdbc3d60e5450",
     "582b5c76df886991eeba7308d67099fd266ccde69d820b426555fd6e6e0e9470"),
]

# The one-way map of 64 bytes: draft-irtf-cfrg-ristretto255-decaf448-04
# Appendix A.3, as copied into curve25519-dalek 5.0.0
# src/ristretto/elligator.rs, test one_way_map.
ONE_WAY_MAP_VECTORS = [
    ("5d1be09e3d0c82fc538112490e35701979d99e06ca3e2b5b54bffe8b4dc772c1"
     "4d98b696a1bbfb5ca32c436cc61c16563790306c79eaca7705668b47dffe5bb6",
     "3066f82a1a747d45120d1740f14358531a8f04bbffe6a819f86dfe50f44a0a46"),
    ("f116b34b8f17ceb56e8732a60d913dd10cce47a6d53bee9204be8b44f6678b27"
     "0102a56902e2488c46120e9276cfe54638286b9e4b3cdb470b542d46c2068d38",
     "f26e5b6f7d362d2d2a94c5d0e7602cb4773c95a2e5c31a64f133189fa76ed61b"),
    ("8422e1bbdaab52938b81fd602effb6f89110e1e57208ad12d9ad767e2e25510c"
     "27140775f9337088b982d83d7fcf0b2fa1edffe51952cbe7365e95c86eaf325c",
     "006ccd2a9e6867e6a2c5cea83d3302cc9de128dd2a9a57dd8ee7b9d7ffe02826"),
    ("ac22415129b61427bf464e17baee8db65940c233b98afce8d17c57beeb7876c2"
     "150d15af1cb1fb824bbd14955f2b57d08d388aab431a391cfc33d5bafb5dbbaf",
     "f8f0c87cf237953c5890aec3998169005dae3eca1fbb04548c635953c817f92a"),
    ("165d697a1ef3d5cf3c38565beefcf88c0f282b8e7dbd28544c483432f1cec767"
     "5debea8ebb4e5fe7d6f6e5db15f15587ac4d4d4a1de7191e0c1ca6664abcc413",
     "ae81e7dedf20a497e10c304a765c1767a42d6e06029758d2d7e8ef7cc4c41179"),
    ("a836e6c9a9ca9f1e8d486273ad56a78c70cf18f0ce10abb1c7172ddd605d7fd2"
     "979854f47ae1ccf204a33102095b4200e5befc0465accc263175485f0e17ea5c",
     "e2705652ff9f5e44d3e841bf1c251cf7dddb77d140870d1ab2ed64f1a9ce8628"),
    ("2cdc11eaeb95daf01189417cdddbf95952993aa9cb9c640eb5058d09702c7462"
     "2c9965a697a3b345ec24ee56335b556e677b30e6f90ac77d781064f866a3c982",
     "80bd07262511cdde4863f8a7434cef696750681cb9510eea557088f76d9e5065"),
    ("edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
     "1200000000000000000000000000000000000000000000000000000000000000",
     "304282791023b73128d277bdcb5c7746ef2eac08dde9f2983379cb8e5ef0517f"),
    ("edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"
     "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "304282791023b73128d277bdcb5c7746ef2eac08dde9f2983379cb8e5ef0517f"),
    ("0000000000000000000000000000000000000000000000000000000000000080"
     "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
     "304282791023b73128d277bdcb5c7746ef2eac08dde9f2983379cb8e5ef0517f"),
    ("0000000000000000000000000000000000000000000000000000000000000000"
     "1200000000000000000000000000000000000000000000000000000000000080",
     "304282791023b73128d277bdcb5c7746ef2eac08dde9f2983379cb8e5ef0517f"),
]

# draft-irtf-cfrg-hpke (the editor's source RFC 9180 was published from),
# "DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, ChaCha20Poly1305", lines 2290-2489
# of the fetched hpke.md.
HPKE_INFO = "4f6465206f6e2061204772656369616e2055726e"
HPKE_PT = "4265617574792069732074727574682c20747275746820626561757479"
HPKE_AAD0 = "436f756e742d30"
HPKE_EXPORT_CONTEXTS = ["", "00", "54657374436f6e74657874"]
HPKE_BASE = {
    "source": "draft-irtf-cfrg-hpke test vectors, DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, ChaCha20Poly1305, Base Setup (hpke.md:2292-2388)",
    "mode": 0,
    "info": HPKE_INFO,
    "ikmE": "909a9b35d3dc4713a5e72a4da274b55d3d3821a37e5d099e74a647db583a904b",
    "pkEm": "1afa08d3dec047a643885163f1180476fa7ddb54c6a8029ea33f95796bf2ac4a",
    "skEm": "f4ec9b33b792c372c1d2c2063507b684ef925b8c75a42dbcbf57d63ccd381600",
    "ikmR": "1ac01f181fdf9f352797655161c58b75c656a6cc2716dcb66372da835542e1df",
    "pkRm": "4310ee97d88cc1f088a5576c77ab0cf5c3ac797f3d95139c6c84b5429c59662a",
    "skRm": "8057991eef8f1f1af18f4a9491d16a1ce333f695d4db8e38da75975c4478e0fb",
    "enc": "1afa08d3dec047a643885163f1180476fa7ddb54c6a8029ea33f95796bf2ac4a",
    "shared_secret": "0bbe78490412b4bbea4812666f7916932b828bba79942424abb65244930d69a7",
    "key": "ad2744de8e17f4ebba575b3f5f5a8fa1f69c2a07f6e7500bc60ca6e3e3ec1c91",
    "base_nonce": "5c4d98150661b848853b547f",
    "exporter_secret": "a3b010d4994890e2c6968a36f64470d3c824c8f5029942feb11e7a74b2921922",
    "pt": HPKE_PT,
    "aad": HPKE_AAD0,
    "ct": "1c5250d8034ec2b784ba2cfd69dbdb8af406cfe3ff938e131f0def8c8b60b4db21993c62ce81883d2dd1b51a28",
    "exports": [
        "4bbd6243b8bb54cec311fac9df81841b6fd61f56538a775e7c80a9f40160606e",
        "8c1df14732580e5501b00f82b10a1647b40713191b7c1240ac80e2b68808ba69",
        "5acb09211139c43b3090489a9da433e8a30ee7188ba8b0a9a1ccf0c229283e53",
    ],
}
HPKE_PSK = {
    "source": "draft-irtf-cfrg-hpke test vectors, DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, ChaCha20Poly1305, PSK Setup (hpke.md:2390-2489)",
    "mode": 1,
    "info": HPKE_INFO,
    "ikmE": "35706a0b09fb26fb45c39c2f5079c709c7cf98e43afa973f14d88ece7e29c2e3",
    "pkEm": "2261299c3f40a9afc133b969a97f05e95be2c514e54f3de26cbe5644ac735b04",
    "skEm": "0c35fdf49df7aa01cd330049332c40411ebba36e0c718ebc3edf5845795f6321",
    "ikmR": "26b923eade72941c8a85b09986cdfa3f1296852261adedc52d58d2930269812b",
    "pkRm": "13640af826b722fc04feaa4de2f28fbd5ecc03623b317834e7ff4120dbe73062",
    "skRm": "77d114e0212be51cb1d76fa99dd41cfd4d0166b08caa09074430a6c59ef17879",
    "psk": "0247fd33b913760fa1fa51e1892d9f307fbe65eb171e8132c2af18555a738b82",
    "psk_id": "456e6e796e20447572696e206172616e204d6f726961",
    "enc": "2261299c3f40a9afc133b969a97f05e95be2c514e54f3de26cbe5644ac735b04",
    "shared_secret": "4be079c5e77779d0215b3f689595d59e3e9b0455d55662d1f3666ec606e50ea7",
    "key": "600d2fdb0313a7e5c86a9ce9221cd95bed069862421744cfb4ab9d7203a9c019",
    "base_nonce": "112e0465562045b7368653e7",
    "exporter_secret": "73b506dc8b6b4269027f80b0362def5cbb57ee50eed0c2873dac9181f453c5ac",
    "pt": HPKE_PT,
    "aad": HPKE_AAD0,
    "ct": "4a177f9c0d6f15cfdf533fb65bf84aecdc6ab16b8b85b4cf65a370e07fc1d78d28fb073214525276f4a89608ff",
    "exports": [
        "813c1bfc516c99076ae0f466671f0ba5ff244a41699f7b2417e4c59d46d39f40",
        "2745cf3d5bb65c333658732954ee7af49eb895ce77f8022873a62a13c94cb4e1",
        "ad40e3ae14f21c99bfdebc20ae14ab86f4ca2dc9a4799d200f43a25f99fa78ae",
    ],
}

# RFC 8032 section 7.1, TEST 1 (the empty message). Recalled, not fetched
# (spec unverified list); the independent computation below reproduces it.
ED25519_TEST1 = {
    "source": "RFC 8032 section 7.1 TEST 1 (recalled, not fetched; reproduced by this generator's own Ed25519)",
    "secret_key": "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
    "public_key": "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
    "message": "",
    "signature": "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
}


def _hpke_check(v):
    hx = bytes.fromhex
    sk_e, pk_e = derive_keypair(hx(v["ikmE"]))
    if sk_e.hex() != v["skEm"] or pk_e.hex() != v["pkEm"]:
        return "ephemeral key pair"
    if x25519(hx(v["skRm"]), X25519_BASE).hex() != v["pkRm"]:
        return "recipient public key"
    if derive_keypair(hx(v["ikmR"]))[0].hex() != v["skRm"]:
        return "recipient key derivation"
    ss, enc = encap_derand(hx(v["pkRm"]), hx(v["ikmE"]))
    if enc.hex() != v["enc"] or ss.hex() != v["shared_secret"]:
        return "encapsulation"
    if decap(enc, hx(v["skRm"])) != ss:
        return "decapsulation"
    psk = hx(v.get("psk", ""))
    psk_id = hx(v.get("psk_id", ""))
    ctx = key_schedule(v["mode"], ss, hx(v["info"]), psk, psk_id)
    for name in ("key", "base_nonce", "exporter_secret"):
        if ctx[name].hex() != v[name]:
            return name
    ct = aead_seal(ctx["key"], ctx["base_nonce"], hx(v["aad"]), hx(v["pt"]))
    if ct.hex() != v["ct"]:
        return "sequence-0 ciphertext"
    if open_hpke(v["mode"], hx(v["skRm"]), enc, hx(v["info"]), hx(v["aad"]), ct, psk, psk_id) != hx(v["pt"]):
        return "open"
    for c, want in zip(HPKE_EXPORT_CONTEXTS, v["exports"]):
        if hpke_export(ctx, hx(c), 32).hex() != want:
            return "export"
    return None


def self_check():
    """Every check runs before a byte is written; any failure exits 2."""
    failures = []
    try:
        check_constants()
    except AssertionError as e:
        failures.append("ristretto255 constants: %s" % e)
    for i, want in enumerate(SMALL_MULTIPLES):
        point = pt_mul(i, BASE)
        if r_encode(point).hex() != want:
            failures.append("ristretto255 small multiple %d (encode)" % i)
        if i and (r_decode(bytes.fromhex(want)) is None or r_encode(r_decode(bytes.fromhex(want))).hex() != want):
            failures.append("ristretto255 small multiple %d (decode)" % i)
    for i, (inp, want) in enumerate(ELLIGATOR_VECTORS):
        t = int.from_bytes(bytes.fromhex(inp), "little") & ((1 << 255) - 1)
        if r_encode(r_map(t % P)).hex() != want:
            failures.append("ristretto255 Elligator vector %d" % i)
    for i, (inp, want) in enumerate(ONE_WAY_MAP_VECTORS):
        if r_encode(from_uniform(bytes.fromhex(inp))).hex() != want:
            failures.append("ristretto255 one-way-map vector %d" % i)
    for name, v in (("base", HPKE_BASE), ("psk", HPKE_PSK)):
        why = _hpke_check(v)
        if why:
            failures.append("HPKE %s vector: %s" % (name, why))
    seed = bytes.fromhex(ED25519_TEST1["secret_key"])
    if ed25519_public(seed).hex() != ED25519_TEST1["public_key"]:
        failures.append("Ed25519 RFC 8032 test 1 public key")
    if ed25519_sign(seed, b"").hex() != ED25519_TEST1["signature"]:
        failures.append("Ed25519 RFC 8032 test 1 signature")
    # The decoder refuses what it must: a negative s, s >= p, and the
    # encoding of p - 1.
    g_enc = bytes.fromhex(SMALL_MULTIPLES[1])
    if r_decode(bytes([g_enc[0] ^ 1]) + g_enc[1:]) is not None:
        failures.append("ristretto255 decode accepted a negative s")
    if r_decode((P - 1).to_bytes(32, "little")) is not None:
        failures.append("ristretto255 decode accepted p - 1")
    if r_decode(b"\xff" * 32) is not None:
        failures.append("ristretto255 decode accepted s >= p")
    return failures

# ---------------------------------------------------------------------------
# Deterministic randomness (normative; the Rust KatRng is identical):
#   block(label, i) = SHA-512("securacv-threshold-custody-kat:v1"
#                             || u32le(len label) || label || u64le(i))
#   each draw takes the next block(s); scalar() = block mod l, redrawn on 0;
#   bytes(n) = successive blocks, first n bytes.
# ---------------------------------------------------------------------------

KAT_DRBG_DOMAIN = b"securacv-threshold-custody-kat:v1"


class Stream:
    def __init__(self, label):
        self.label = label.encode()
        self.i = 0

    def _block(self):
        b = hashlib.sha512(
            KAT_DRBG_DOMAIN + struct.pack("<I", len(self.label)) + self.label + struct.pack("<Q", self.i)
        ).digest()
        self.i += 1
        return b

    def bytes(self, n):
        out = b""
        while len(out) < n:
            out += self._block()
        return out[:n]

    def scalar(self):
        while True:
            x = int.from_bytes(self._block(), "little") % L
            if x:
                return x


# ---------------------------------------------------------------------------
# The custody formats (spec section 2.5), frozen as v1.
# ---------------------------------------------------------------------------

COMMITMENTS_DOMAIN = b"securacv:custody:commitments:v1"
RECIPIENT_DOMAIN = b"securacv:custody:recipient:v1"
SHARE_FILE_CHECK_DOMAIN = b"securacv-share-file:v1"
SHARE_AT_REST_AAD_PREFIX = b"securacv-share-at-rest-v1"
HPKE_INFO_SHARE = b"securacv-custody-share:v1"
MLKEM_PSK_ID_PREFIX = b"securacv:custody:mlkem768-psk-id:v1"
MKC1_AAD_DOMAIN = b"securacv-vault-custody-v1"
KEK_WRAP_INFO = b"securacv:vault:kek-wrap:v1"
VAULT_X25519_SALT = b"securacv:vault:x25519-ikm:v1"
TEK1_AAD_DOMAIN = b"securacv-custody-recipient-key-v1"
BINDING_DOMAIN = "securacv:pwk:custody-recipient-binding:v1"
RECIPIENT_FORMAT = "securacv-custody-recipient:v1"
RECIPIENT_KEY_TEXT_PREFIX = "securacv-custody-recipient-key:v1"
SPLIT_RECORD_FORMAT = "securacv-custody-split-record:v1"
SHARE_TEXT_PREFIX = "securacv-share:v1"
ARGON2 = (65536, 3, 1)
SUITE_V1 = 0x01
PROTECTION_BOX1, PROTECTION_BOX2, PROTECTION_AT_REST = 0x01, 0x02, 0x03
ROLE_OPERATIONAL, ROLE_RECOVERY = 0x01, 0x02
HOLDER_TRUSTEE, HOLDER_RECOVERY = 0x01, 0x02
MODE_CLASSICAL, MODE_HYBRID = 0x01, 0x03
BUCKET = 1794312000
MLKEM_PK_LEN, MLKEM_SK_LEN, MLKEM_CT_LEN = 1184, 2400, 1088


def frame(x):
    return struct.pack("<I", len(x)) + x


def sha256(b):
    return hashlib.sha256(b).digest()


def commitments_sha256(role, split_id, n, m, commitments):
    return sha256(
        frame(COMMITMENTS_DOMAIN) + bytes([SUITE_V1, role]) + split_id + bytes([n, m]) + b"".join(commitments)
    )


def recipient_key_hash(holder, role, signing_pk, x25519_pk, mlkem_pk=None):
    tail = b"\x00" if mlkem_pk is None else b"\x01" + sha256(mlkem_pk)
    return sha256(frame(RECIPIENT_DOMAIN) + frame(holder.encode()) + bytes([role]) + signing_pk + x25519_pk + tail)


def fingerprint(h):
    return h[:4].hex() + " " + h[4:8].hex()


def compact_json(obj):
    return json.dumps(obj, separators=(",", ":")) + "\n"


def recipient_json(holder, role, signing_pk, x25519_pk, signature):
    return compact_json(
        {
            "format": RECIPIENT_FORMAT,
            "holder": holder,
            "role": "trustee" if role == HOLDER_TRUSTEE else "recovery",
            "signing_public_key": signing_pk.hex(),
            "x25519": x25519_pk.hex(),
            "binding_signature": signature.hex(),
        }
    )


def vault_x25519_from_master(master):
    return derive_keypair(hkdf(VAULT_X25519_SALT, master, b"", 32))




def mkc1_bytes(k, r, kek_salt, nonce, mode, master, kem=None, header_x25519=None):
    vx_pk = vault_x25519_from_master(master)[1] if header_x25519 is None else header_x25519
    pk = b"" if kem is None else kem[0]
    header = (
        b"MKC1"
        + bytes([1, SUITE_V1, mode])
        + kek_salt
        + r_encode(commit(k, r))
        + vx_pk
        + struct.pack("<H", len(pk))
        + pk
    )
    payload = b"\x01" + master
    if kem is None:
        payload += b"\x00"
    else:
        payload += b"\x01" + struct.pack("<I", len(kem[0])) + kem[0] + struct.pack("<I", len(kem[1])) + kem[1]
    header += nonce + struct.pack("<I", len(payload) + 16)
    wrap_key = hkdf(kek_salt, scalar_bytes(k), KEK_WRAP_INFO, 32)
    return header + aead_seal(wrap_key, nonce, MKC1_AAD_DOMAIN + header, payload)


def share_header(protection, role, release_box, split_id, commitments, container, device_pk, n, m, index, roster):
    """bytes[0..Hd] of an SHR1 file. roster: [(index, id, signing_pk, rkh)]."""
    own = roster[index - 1]
    out = b"SHR1" + bytes([1, SUITE_V1, protection, role, release_box]) + split_id + commitments[0]
    out += sha256(container) + device_pk + struct.pack("<Q", BUCKET)
    out += bytes([n, m, index, len(own[1])]) + own[1].encode() + own[3]
    out += b"".join(commitments[1:])
    out += bytes([m])
    for idx, hid, spk, rkh in roster:
        out += bytes([idx, len(hid)]) + hid.encode() + spk + rkh
    out += struct.pack("<H", len(container)) + container
    return out


def with_file_check(body):
    return body + sha256(SHARE_FILE_CHECK_DOMAIN + body)


def transport_body(enc, ct):
    assert len(ct) == 80
    return struct.pack("<H", len(enc)) + enc + struct.pack("<H", 80) + ct


def at_rest_body(salt, argon_out, nonce, ct):
    return struct.pack("<III", *ARGON2) + salt + sha256(argon_out) + nonce + ct


def tek1_bytes(holder, role, signing_pk, rkh, x25519_sk, argon_out, salt, nonce):
    header = b"TEK1" + bytes([1, 1]) + struct.pack("<III", *ARGON2) + bytes([16]) + salt + sha256(argon_out)
    header += bytes([len(holder)]) + holder.encode() + bytes([role]) + signing_pk + rkh
    pt = x25519_sk + b"\x00"
    return header + nonce + struct.pack("<I", len(pt) + 16) + aead_seal(argon_out, nonce, TEK1_AAD_DOMAIN + header, pt)


def split_record_json(role, split_id, n, m, commitments, container, device_pk, roster):
    return compact_json(
        {
            "format": SPLIT_RECORD_FORMAT,
            "role": "operational" if role == ROLE_OPERATIONAL else "recovery",
            "split_id": split_id.hex(),
            "n": n,
            "m": m,
            "kek_commitment": commitments[0].hex(),
            "commitments": [c.hex() for c in commitments],
            "commitments_sha256": commitments_sha256(role, split_id, n, m, commitments).hex(),
            "container_sha256": sha256(container).hex(),
            "device_public_key": device_pk.hex(),
            "issued_bucket": BUCKET,
            "holders": [
                {"index": i, "id": h, "signing_public_key": s.hex(), "recipient_key_hash": k.hex()}
                for i, h, s, k in roster
            ],
        }
    )


def poly_eval(coeffs, x):
    return sum(c * pow(x, j, L) for j, c in enumerate(coeffs)) % L


def public_share(commitment_points, x):
    acc = IDENTITY
    for j, c in enumerate(commitment_points):
        acc = pt_add(acc, pt_mul(pow(x, j, L), c))
    return acc


def lagrange_at_zero(xs):
    out = []
    for i in xs:
        num, den = 1, 1
        for j in xs:
            if j != i:
                num = num * j % L
                den = den * (j - i) % L
        out.append(num * pow(den, L - 2, L) % L)
    return out


def subsets(xs, n):
    if n == 0:
        return [[]]
    if len(xs) < n:
        return []
    return [[xs[0]] + rest for rest in subsets(xs[1:], n - 1)] + subsets(xs[1:], n)

# ---------------------------------------------------------------------------
# Argon2id is not in the Python standard library. The at-rest share and TEK1
# vectors therefore pin the AEAD layer under a *given* Argon2id output (drawn
# from the KAT stream). Separately, the Rust test computes
# Argon2id("kat passphrase", salt, 65536/3/1) with the argon2 0.6 crate and
# compares it with these values, which were produced by that crate and are
# not cross-implemented.
# ---------------------------------------------------------------------------

ARGON2ID_PASSPHRASE = "kat passphrase"
ARGON2ID_RUST_PINS = {
    # salt (the at-rest share's) -> Argon2id("kat passphrase", salt, 65536/3/1)
    "71a54f2c92c276b9b3b400962d394162": "ca250820dec3b09afc0404fd4239708086d8c8d40cd58e36d1a6fb7fb5bed2c7",
    # salt (TEK1's)
    "d1f6b6b6451f36329592e66b105db231": "241ca5db8e5ff57cafa3b0f960b8621c8eda811fb747d07d3a5e41690416255f",
}

OPERATIONAL_IDS = ["alice", "bob", "carol", "dave", "erin"]
RECOVERY_IDS = ["r1", "r2", "r3", "r4", "r5", "r6"]

NOTES = [
    "Every value is derived from the deterministic stream described in spec/quorum_unseal_v2.md and docs/design/threshold_custody.md (block(label, i) = SHA-512(\"securacv-threshold-custody-kat:v1\" || u32le(len label) || label || u64le(i))); nothing is random.",
    "Written by a Python 3 standard-library implementation of its own (ristretto255, scalars mod l, X25519, ChaCha20-Poly1305, HKDF, HPKE, Ed25519); the Rust tests in src/vault/custody/kat.rs compare the Rust implementation against this file. A disagreement is a stop-ship, never a fixture edit.",
    "Scalars and points are 32-byte little-endian / canonical ristretto255 encodings, in lowercase hex.",
    "generator_h is RistrettoPoint::from_uniform_bytes(SHA-512(\"securacv:custody:pedersen-generator-H:v1\")). The design prototype printed 2e99ac14...; it used the even square root for SQRT_AD_MINUS_ONE where RFC 9496 lists the odd one, and fails every one-way-map vector of the draft. This generator checks the odd constant against those vectors.",
    "Argon2id is not in the Python standard library: the at-rest share and TEK1 vectors pin the AEAD layer under a given Argon2id output (argon_out). argon2id_rust_pin is Argon2id(\"kat passphrase\", salt, 65536/3/1) as the Rust argon2 0.6 crate computes it; it is not cross-implemented.",
    "ML-KEM-768 is not implemented here: share_file_box2 pins box 2's HPKE PSK layer under a given ML-KEM shared secret and ciphertext, and mkc1.hybrid carries opaque KEM key bytes drawn from the stream (kem_public_sha256 / kem_secret_sha256 name them).",
    "Box 2 (HPKE PSK mode keyed by an ML-KEM-768 shared secret) is expected to stay secret unless both the X25519 secret and the ML-KEM secret are compromised; the HPKE draft states that this hybrid property is not proven.",
]


def build():
    obj = {
        "format": "securacv-threshold-custody-kat:v1",
        "generator": "spec/fixtures/gen_threshold_custody_v1.py",
        "notes": NOTES,
        "standards": {
            "ristretto255_constants": {
                "source": "RFC 9496 section 4.1 (copied as integers from curve25519-dalek 5.0.0 src/backend/serial/u64/constants.rs; each checked against its defining equation)",
                "sqrt_m1": str(SQRT_M1),
                "sqrt_ad_minus_one": str(SQRT_AD_MINUS_ONE),
                "invsqrt_a_minus_d": str(INVSQRT_A_MINUS_D),
                "one_minus_d_sq": str(ONE_MINUS_D_SQ),
                "d_minus_one_sq": str(D_MINUS_ONE_SQ),
            },
            "ristretto255_small_multiples": {
                "source": "curve25519-dalek 5.0.0 src/ristretto.rs:1389-1455 (\"Generated using ristretto.sage\"): the encodings of i*G, i = 0..15",
                "encodings": SMALL_MULTIPLES,
            },
            "ristretto255_elligator": {
                "source": "curve25519-dalek 5.0.0 src/ristretto/elligator.rs, test elligator_vs_ristretto_sage (MAP of one field element)",
                "vectors": [{"input": i, "output": o} for i, o in ELLIGATOR_VECTORS],
            },
            "ristretto255_one_way_map": {
                "source": "draft-irtf-cfrg-ristretto255-decaf448-04 Appendix A.3, as copied into curve25519-dalek 5.0.0 src/ristretto/elligator.rs, test one_way_map",
                "vectors": [{"input": i, "output": o} for i, o in ONE_WAY_MAP_VECTORS],
            },
            "hpke_base": dict(HPKE_BASE, exporter_contexts=HPKE_EXPORT_CONTEXTS),
            "hpke_psk": dict(HPKE_PSK, exporter_contexts=HPKE_EXPORT_CONTEXTS),
            "ed25519_rfc8032_test1": ED25519_TEST1,
        },
    }
    obj["generator_h"] = r_encode(H).hex()

    # 1. The KEK.
    s = Stream("kek")
    k = s.scalar()
    r = s.scalar()
    kek_salt = s.bytes(16)
    kek_commitment = r_encode(commit(k, r))
    obj["kek"] = {
        "k": scalar_hex(k),
        "r": scalar_hex(r),
        "kek_commitment": kek_commitment.hex(),
        "kek_salt": kek_salt.hex(),
        "wrap_key": hkdf(kek_salt, scalar_bytes(k), KEK_WRAP_INFO, 32).hex(),
    }

    # 4. Recipients (drawn before they are needed; streams are independent).
    recipients = {}
    roles = {}
    for hid in OPERATIONAL_IDS + RECOVERY_IDS:
        st = Stream("recipient/" + hid)
        seed = st.bytes(32)
        ikm = st.bytes(32)
        x_sk, x_pk = derive_keypair(ikm)
        spk = ed25519_public(seed)
        role = HOLDER_TRUSTEE if hid in OPERATIONAL_IDS else HOLDER_RECOVERY
        rkh = recipient_key_hash(hid, role, spk, x_pk)
        sig = sign_domain(seed, BINDING_DOMAIN, rkh)
        roles[hid] = role
        recipients[hid] = {
            "signing_seed": seed.hex(),
            "signing_public_key": spk.hex(),
            "x25519_ikm": ikm.hex(),
            "x25519_secret": x_sk.hex(),
            "x25519_public": x_pk.hex(),
            "role": "trustee" if role == HOLDER_TRUSTEE else "recovery",
            "recipient_key_hash": rkh.hex(),
            "fingerprint": fingerprint(rkh),
            "binding_signature": sig.hex(),
            "recipient_json": recipient_json(hid, role, spk, x_pk, sig),
        }
    obj["recipients"] = recipients

    # 5. The device key.
    device_seed = Stream("device").bytes(32)
    device_pk = ed25519_public(device_seed)
    obj["device"] = {"seed": device_seed.hex(), "public_key": device_pk.hex()}

    # 6-7. The containers.
    st = Stream("mkc1")
    master = st.bytes(32)
    nonce = st.bytes(12)
    vx_sk, vx_pk = vault_x25519_from_master(master)
    classical = mkc1_bytes(k, r, kek_salt, nonce, MODE_CLASSICAL, master)
    st = Stream("mkc1-kem")
    kem_pk = st.bytes(MLKEM_PK_LEN)
    kem_sk = st.bytes(MLKEM_SK_LEN)
    kem_nonce = st.bytes(12)
    hybrid = mkc1_bytes(k, r, kek_salt, kem_nonce, MODE_HYBRID, master, (kem_pk, kem_sk))
    assert len(hybrid) == 4931, len(hybrid)
    obj["mkc1"] = {
        "classical": {
            "crypto_mode": MODE_CLASSICAL,
            "master_key": master.hex(),
            "vault_x25519_secret": vx_sk.hex(),
            "vault_x25519_public": vx_pk.hex(),
            "nonce": nonce.hex(),
            "container_hex": classical.hex(),
            "sha256": sha256(classical).hex(),
        },
        "hybrid": {
            "crypto_mode": MODE_HYBRID,
            "master_key": master.hex(),
            "vault_x25519_public": vx_pk.hex(),
            "kem_public_sha256": sha256(kem_pk).hex(),
            "kem_secret_sha256": sha256(kem_sk).hex(),
            "nonce": kem_nonce.hex(),
            "container_hex": hybrid.hex(),
            "sha256": sha256(hybrid).hex(),
        },
    }

    # 2-3. The splits of the same (k, r).
    splits = {}
    rosters = {}
    split_ids = {}
    all_shares = {}
    for name, label, n, ids, role in (
        ("operational", "split/operational", 3, OPERATIONAL_IDS, ROLE_OPERATIONAL),
        ("recovery", "split/recovery", 4, RECOVERY_IDS, ROLE_RECOVERY),
    ):
        st = Stream(label)
        split_id = st.bytes(16)
        f = [k] + [st.scalar() for _ in range(n - 1)]
        g = [r] + [st.scalar() for _ in range(n - 1)]
        points = [commit(f[j], g[j]) for j in range(n)]
        commitments = [r_encode(c) for c in points]
        m = len(ids)
        shares = []
        for x in range(1, m + 1):
            sx, tx = poly_eval(f, x), poly_eval(g, x)
            pub = public_share(points, x)
            assert r_encode(commit(sx, tx)) == r_encode(pub), "share %d of %s" % (x, name)
            shares.append({"index": x, "s": scalar_hex(sx), "t": scalar_hex(tx), "public_share": r_encode(pub).hex()})
            all_shares[ids[x - 1]] = (x, sx, tx)
        if name == "operational":
            chosen = subsets(list(range(1, m + 1)), n)
        else:
            chosen = [[1, 2, 3, 4], [3, 4, 5, 6], [1, 3, 5, 6], [2, 4, 5, 6]]
        subset_rows = []
        for xs in chosen:
            lam = lagrange_at_zero(xs)
            kk = sum(l_i * poly_eval(f, x) for l_i, x in zip(lam, xs)) % L
            rr = sum(l_i * poly_eval(g, x) for l_i, x in zip(lam, xs)) % L
            assert (kk, rr) == (k, r), xs
            subset_rows.append(
                {"indices": xs, "lagrange": [scalar_hex(v) for v in lam], "k": scalar_hex(kk), "r": scalar_hex(rr)}
            )
        roster = [
            (i + 1, hid, bytes.fromhex(recipients[hid]["signing_public_key"]), bytes.fromhex(recipients[hid]["recipient_key_hash"]))
            for i, hid in enumerate(ids)
        ]
        rosters[name] = (role, n, m, commitments, roster)
        split_ids[name] = split_id
        splits[name] = {
            "split_id": split_id.hex(),
            "role": role,
            "n": n,
            "m": m,
            "holders": ids,
            "f": [scalar_hex(v) for v in f],
            "g": [scalar_hex(v) for v in g],
            "commitments": [c.hex() for c in commitments],
            "commitments_sha256": commitments_sha256(role, split_id, n, m, commitments).hex(),
            "shares": shares,
            "subsets": subset_rows,
        }
    assert splits["recovery"]["commitments"][0] == splits["operational"]["commitments"][0]
    obj["splits"] = splits

    # 8. The eleven box-1 transport share files.
    share_files = {}
    headers = {}
    for name in ("operational", "recovery"):
        role, n, m, commitments, roster = rosters[name]
        release_box = 0x01 if role == ROLE_OPERATIONAL else 0x00
        for idx, hid, _, _ in roster:
            header = share_header(
                PROTECTION_BOX1, role, release_box, split_ids[name], commitments, classical, device_pk, n, m, idx, roster
            )
            headers[hid] = (header, name)
            _, sx, tx = all_shares[hid]
            ikm_e = Stream("share/" + hid).bytes(32)
            pk = bytes.fromhex(recipients[hid]["x25519_public"])
            enc, ct = seal_base_derand(pk, HPKE_INFO_SHARE, header, scalar_bytes(sx) + scalar_bytes(tx), ikm_e)
            sk = bytes.fromhex(recipients[hid]["x25519_secret"])
            assert open_hpke(0, sk, enc, HPKE_INFO_SHARE, header, ct) == scalar_bytes(sx) + scalar_bytes(tx)
            file_bytes = with_file_check(header + transport_body(enc, ct))
            assert len(file_bytes) <= 16384
            share_files[hid] = {
                "split": name,
                "ikm_e": ikm_e.hex(),
                "header_hex": header.hex(),
                "file_hex": file_bytes.hex(),
                "text": SHARE_TEXT_PREFIX + " " + file_bytes.hex() + "\n",
            }
    obj["share_files"] = share_files

    # 9. Box 2's HPKE PSK layer for alice's share (protection 2, release box
    # 2, the hybrid container's copy).
    role, n, m, commitments, roster = rosters["operational"]
    st = Stream("box2")
    ss_m = st.bytes(32)
    ct_m = st.bytes(MLKEM_CT_LEN)
    ikm_e = st.bytes(32)
    psk_id = MLKEM_PSK_ID_PREFIX + sha256(ct_m)
    header2 = share_header(PROTECTION_BOX2, role, 0x02, split_ids["operational"], commitments, hybrid, device_pk, n, m, 1, roster)
    _, sx, tx = all_shares["alice"]
    enc_x, ct = seal_psk_derand(
        bytes.fromhex(recipients["alice"]["x25519_public"]), HPKE_INFO_SHARE, header2, scalar_bytes(sx) + scalar_bytes(tx), ss_m, psk_id, ikm_e
    )
    box2_file = with_file_check(header2 + transport_body(ct_m + enc_x, ct))
    obj["share_file_box2"] = {
        "holder": "alice",
        "ss_m": ss_m.hex(),
        "ct_m": ct_m.hex(),
        "ikm_e": ikm_e.hex(),
        "psk_id": psk_id.hex(),
        "header_hex": header2.hex(),
        "file_hex": box2_file.hex(),
    }

    # 10. alice's share wrapped at rest (protection 3) under a given Argon2id
    # output.
    st = Stream("at-rest")
    argon_out = st.bytes(32)
    salt = st.bytes(16)
    nonce3 = st.bytes(12)
    header3 = share_header(PROTECTION_AT_REST, role, 0x01, split_ids["operational"], commitments, classical, device_pk, n, m, 1, roster)
    ct3 = aead_seal(argon_out, nonce3, SHARE_AT_REST_AAD_PREFIX + header3, scalar_bytes(sx) + scalar_bytes(tx))
    at_rest_file = with_file_check(header3 + at_rest_body(salt, argon_out, nonce3, ct3))
    obj["share_file_at_rest"] = {
        "holder": "alice",
        "argon_out": argon_out.hex(),
        "salt": salt.hex(),
        "nonce": nonce3.hex(),
        "file_hex": at_rest_file.hex(),
        "argon2id_rust_pin": argon_pin(salt),
    }

    # 11. alice's recipient key wrapped as TEK1.
    st = Stream("tek1")
    argon_out_t = st.bytes(32)
    salt_t = st.bytes(16)
    nonce_t = st.bytes(12)
    alice = recipients["alice"]
    tek1 = tek1_bytes(
        "alice",
        HOLDER_TRUSTEE,
        bytes.fromhex(alice["signing_public_key"]),
        bytes.fromhex(alice["recipient_key_hash"]),
        bytes.fromhex(alice["x25519_secret"]),
        argon_out_t,
        salt_t,
        nonce_t,
    )
    obj["tek1"] = {
        "holder": "alice",
        "argon_out": argon_out_t.hex(),
        "salt": salt_t.hex(),
        "nonce": nonce_t.hex(),
        "tek1_hex": tek1.hex(),
        "file_text": RECIPIENT_KEY_TEXT_PREFIX + " " + tek1.hex() + "\n",
        "argon2id_rust_pin": argon_pin(salt_t),
    }

    # The split records.
    obj["split_records"] = {
        name: split_record_json(rosters[name][0], split_ids[name], rosters[name][1], rosters[name][2], rosters[name][3], classical, device_pk, rosters[name][4])
        for name in ("operational", "recovery")
    }

    obj["negative"] = negatives(k, r, kek_salt, master, classical, share_files, splits, recipients, tek1)
    return obj


def argon_pin(salt):
    return {
        "source": "rust argon2 0.6, not cross-implemented",
        "passphrase": ARGON2ID_PASSPHRASE,
        "params": {"m_kib": ARGON2[0], "t": ARGON2[1], "p": ARGON2[2]},
        "salt": salt.hex(),
        "output": ARGON2ID_RUST_PINS[salt.hex()],
    }


def _refile(file_bytes):
    return with_file_check(file_bytes[:-32])


def _flip(file_bytes, at, bit=0x01):
    b = bytearray(file_bytes)
    b[at] ^= bit
    return _refile(bytes(b))


def _set(file_bytes, at, value):
    b = bytearray(file_bytes)
    b[at] = value
    return _refile(bytes(b))


def negatives(k, r, kek_salt, master, classical, share_files, splits, recipients, tek1):
    out = []
    op = splits["operational"]
    sh = {row["index"]: row for row in op["shares"]}

    def share(idx, s=None, t=None, at=None):
        return {"index": idx if at is None else at, "s": s or sh[idx]["s"], "t": t or sh[idx]["t"]}

    def plus_one(hex_scalar):
        return scalar_hex(int.from_bytes(bytes.fromhex(hex_scalar), "little") + 1)

    out.append({"name": "share 2 with s + 1", "kind": "reconstruct", "split": "operational",
                "shares": [share(1), share(2, s=plus_one(sh[2]["s"])), share(3)],
                "error": "custody: share 2 does not match the split's commitments"})
    out.append({"name": "share 2 with t + 1", "kind": "reconstruct", "split": "operational",
                "shares": [share(1), share(2, t=plus_one(sh[2]["t"])), share(3)],
                "error": "custody: share 2 does not match the split's commitments"})
    out.append({"name": "share 2's scalars presented at index 4", "kind": "reconstruct", "split": "operational",
                "shares": [share(1), share(3), share(2, at=4)],
                "error": "custody: share 4 does not match the split's commitments"})
    out.append({"name": "duplicate Lagrange index", "kind": "lagrange", "indices": [1, 2, 2],
                "error": "custody: share indices must be distinct and between 1 and 32"})
    out.append({"name": "zero Lagrange index", "kind": "lagrange", "indices": [0, 1, 2],
                "error": "custody: share indices must be distinct and between 1 and 32"})
    out.append({"name": "a share relabeled to index 0 (the SLIP-0039 attack)", "kind": "reconstruct", "split": "operational",
                "shares": [share(1, at=0), share(2), share(3)],
                "error": "custody: share indices must be distinct and between 1 and 32"})
    out.append({"name": "non-canonical scalar (l itself)", "kind": "scalar_decode", "input": L.to_bytes(32, "little").hex(),
                "error": "custody: a scalar is not a canonical encoding (it must be below the group order)"})
    out.append({"name": "non-canonical point (the encoding of p - 1)", "kind": "commitments_decode",
                "input": [(P - 1).to_bytes(32, "little").hex(), op["commitments"][1]],
                "error": "commitment 0 is not a canonical ristretto255 encoding"})
    out.append({"name": "identity commitment", "kind": "commitments_decode",
                "input": [op["commitments"][0], "00" * 32],
                "error": "commitment 1 is the identity"})

    alice = bytes.fromhex(share_files["alice"]["file_hex"])
    hd = len(bytes.fromhex(share_files["alice"]["header_hex"]))
    holder_at = 133
    rkh_at = holder_at + len("alice")
    c1_at = rkh_at + 32
    roster_at = c1_at + 32 * (op["n"] - 1)
    bob_key_at = roster_at + 1 + (1 + 1 + len("alice") + 64) + 2 + len("bob")
    container_at = hd - len(classical)
    boundaries = [
        ("magic", 0, "share file: bad magic"),
        ("version", 4, "share file: unsupported version 0"),
        ("suite", 5, "share file: unsupported suite 0"),
        ("protection", 6, "share file: unknown protection 0"),
        ("role", 7, "share file: unknown role 0"),
        ("release box", 8, "share file: unknown release box 0"),
        ("split id", 9, None),
        ("KEK commitment", 25, "share file: commitment 0 is not a canonical ristretto255 encoding"),
        ("container hash", 57, "share file: the container copy does not match its recorded hash"),
        ("device key", 89, None),
        ("issued bucket", 121, "share file: the issued bucket must be a multiple of 600 seconds"),
        ("holder id", holder_at, "share file: holder id must be 1 to 64 bytes of [A-Za-z0-9._-]"),
        ("recipient key hash", rkh_at, "share file: the holder entry does not match the roster entry at index 1"),
        ("commitment 1", c1_at, "share file: commitment 1 is not a canonical ristretto255 encoding"),
        ("roster count", roster_at, "share file: the roster does not list indices 1..=5 exactly once"),
        ("another holder's pinned signing key", bob_key_at, None),
        ("container copy", container_at, "share file: the container copy does not match its recorded hash"),
    ]
    for field, at, error in boundaries:
        row = {"name": "alice's share with the first byte of its %s flipped" % field, "kind": "share_file",
               "holder": "alice", "file_hex": _flip(alice, at).hex()}
        if error is None:
            row["open_error"] = "HPKE: open failed"
        else:
            row["error"] = error
        out.append(row)
    damaged = bytearray(alice)
    damaged[-1] ^= 1
    out.append({"name": "alice's share with its file check flipped", "kind": "share_file", "holder": "alice",
                "file_hex": bytes(damaged).hex(), "error": "share file: file check mismatch — the file is damaged"})
    out.append({"name": "a trailing byte", "kind": "share_file", "holder": "alice", "file_hex": (alice + b"\x00").hex(),
                "error": "share file: trailing bytes"})
    out.append({"name": "n = 1", "kind": "share_file", "holder": "alice", "file_hex": _set(alice, 129, 1).hex(),
                "error": "share file: threshold must be between 2 and 32 and at most the holder count"})
    out.append({"name": "n > m", "kind": "share_file", "holder": "alice", "file_hex": _set(alice, 129, 6).hex(),
                "error": "share file: threshold must be between 2 and 32 and at most the holder count"})
    out.append({"name": "a 65-byte holder id", "kind": "share_file", "holder": "alice", "file_hex": _set(alice, 132, 65).hex(),
                "error": "share file: holder id must be 1 to 64 bytes of [A-Za-z0-9._-]"})
    out.append({"name": "a box-2 share with a 32-byte encapsulation", "kind": "share_file", "holder": "alice",
                "file_hex": _set(alice, 6, PROTECTION_BOX2).hex(),
                "error": "share file: encapsulation length 32 does not match protection 2"})

    ct_len_at = 89 + 12  # ct_len follows the nonce; a classical container has no ML-KEM key
    long_ct = bytearray(classical)
    long_ct[ct_len_at:ct_len_at + 4] = struct.pack("<I", 4097)
    out.append({"name": "MKC1 with ct_len 4097", "kind": "mkc1_header", "container_hex": bytes(long_ct).hex(),
                "error": "custody container: ciphertext length 4097 is out of bounds (17 to 4096)"})
    out.append({"name": "MKC1 opened with the wrong k (k + 1)", "kind": "mkc1_open", "container_hex": classical.hex(),
                "k": scalar_hex(k + 1), "r": scalar_hex(r),
                "error": "custody container: the key rebuilt from the shares is not this container's (KEK commitment differs) — nothing was decrypted"})
    altered = bytearray(classical)
    altered[55] ^= 1
    out.append({"name": "MKC1 with a header public key altered", "kind": "mkc1_open", "container_hex": bytes(altered).hex(),
                "k": scalar_hex(k), "r": scalar_hex(r),
                "error": "custody container: authentication failed — the file was altered or damaged"})
    lying = mkc1_bytes(k, r, kek_salt, bytes(12), MODE_CLASSICAL, master,
                       header_x25519=bytes.fromhex(recipients["alice"]["x25519_public"]))
    out.append({"name": "MKC1 sealed under a header whose X25519 key is not the master key's", "kind": "mkc1_open",
                "container_hex": lying.hex(), "k": scalar_hex(k), "r": scalar_hex(r),
                "error": "custody container: its public keys do not match its contents"})
    big_m = bytearray(tek1)
    big_m[6:10] = struct.pack("<I", 9999999)
    out.append({"name": "TEK1 with m = 9999999 (refused before the KDF)", "kind": "tek1_parse",
                "file_text": RECIPIENT_KEY_TEXT_PREFIX + " " + bytes(big_m).hex() + "\n",
                "error": "recipient key file: unexpected TEK1 v1 parameters"})
    return out


def render():
    return json.dumps(build(), indent=1, sort_keys=True) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="compare with the committed file; write nothing")
    args = parser.parse_args()
    failures = self_check()
    if failures:
        for f in failures:
            print("self-check FAILED: %s" % f, file=sys.stderr)
        return 2
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), OUT_NAME)
    text = render()
    if args.check:
        try:
            with open(path, "rb") as fh:
                current = fh.read()
        except OSError:
            print("%s is missing; run without --check" % OUT_NAME, file=sys.stderr)
            return 1
        if current == text.encode():
            print("%s is current" % OUT_NAME)
            return 0
        try:
            old = json.loads(current)
        except ValueError:
            old = {}
        new = json.loads(text)
        first = next((key for key in sorted(set(old) | set(new)) if old.get(key) != new.get(key)), "(formatting)")
        print("%s differs from what the generator writes; first differing top-level key: %s" % (OUT_NAME, first), file=sys.stderr)
        return 1
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("wrote %s" % OUT_NAME)
    return 0


if __name__ == "__main__":
    sys.exit(main())
