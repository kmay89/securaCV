# Wycheproof test vectors (excerpt)

Five test cases from [Wycheproof](https://github.com/C2SP/wycheproof)'s
`testvectors_v1/mldsa_44_verify_test.json` (180 cases), taken from the
`wycheproof` 0.7.0 crate on crates.io, which carries that file as
`src/data/mldsa_44_verify_test.json`. Wycheproof and the crate are licensed
Apache-2.0; the cases are reproduced as test data, with credit, and their
fields are unchanged. Crate tarball `wycheproof-0.7.0.crate`, SHA-256
`ffb6c33340cdfc5bebaa43b6debf9570812bb30465e42495188740ca8bf1e001`; the
packaged JSON file, SHA-256
`0ca1b5df4575263e29b31fae7569a3da41df9a3b6fee56720a992d0cd1153b68`. The same
five cases, field for field, are in the file on Wycheproof's `main` branch as
fetched on 2026-11-21 (SHA-256
`f70759c95146ff3057ae9708438f6e8d25ba822e3af11d2e87bb20d9a9c39d0e`; eight
other cases differ between that file and the crate's copy).

| File | Size | SHA-256 |
|---|---|---|
| `mldsa_44_verify_excerpt.json` | 32398 B | `5ea0901d56b7973009fdea501809c5fb076fa2d235751e145d4f7feb68318cb5` |

| `tcId` | Source group | Expected | What it is |
|---|---|---|---|
| 81 | `github/sophieschmieg/infinitynorm` | invalid | a coefficient of the response z past the bound, negative direction |
| 82 | same | invalid | the same, positive direction |
| 125 | same | invalid | a coefficient exactly `gamma1 - tau*eta`, the first value outside the bound |
| 126 | same | invalid | a coefficient exactly `-(gamma1 - tau*eta)` |
| 147 | `github/gendx` | valid | a signature whose verification calls `UseHint` on a coefficient with a zero low part |

The four invalid cases were made with the private key, so their challenge
hash is correct: nothing refuses them except the check that the infinity norm
of z is below `gamma1 - beta` (FIPS 204, Algorithm 8, line 13). They are the
kernel's regression vectors for RUSTSEC-2026-0586, read by
`crypto::signatures::tests::mldsa44_checks` in `src/crypto/signatures.rs`,
which requires each to be refused when the signature is decoded and case 147
to verify.

That advisory is about `pqcrypto-mldsa` 0.1.2, which the kernel linked until
it moved to `ml-dsa`. Run over all 180 cases of both copies of the file on
2026-11-21, on an x86_64 CPU with AVX2: PQClean's AVX2 verifier accepted 37 of
the 42 infinity-norm cases, these four among them, and rejected case 147; its
portable `clean` verifier and `ml-dsa` 0.1.2 gave the expected verdict on
every case.

## Extraction

The tarball and anything unpacked from it are data; the script lives
elsewhere and runs isolated (`python3 -I`):

```sh
mkdir crate && cd crate \
  && curl -sLO https://static.crates.io/crates/wycheproof/wycheproof-0.7.0.crate \
  && tar xzf wycheproof-0.7.0.crate wycheproof-0.7.0/src/data/mldsa_44_verify_test.json && cd ..
python3 -I extract.py crate/wycheproof-0.7.0/src/data/mldsa_44_verify_test.json mldsa_44_verify_excerpt.json
```

```python
import json
import sys

KEEP = {81, 82, 125, 126, 147}
src = json.load(open(sys.argv[1]))
groups = []
for group in src["testGroups"]:
    tests = [t for t in group["tests"] if t["tcId"] in KEEP]
    if tests:
        groups.append({"publicKey": group["publicKey"], "source": group["source"], "tests": tests})
assert {t["tcId"] for g in groups for t in g["tests"]} == KEEP
out = {
    "algorithm": src["algorithm"],
    "schema": src["schema"],
    "excerptOf": "testvectors_v1/mldsa_44_verify_test.json (180 tests)",
    "notes": {k: src["notes"][k] for k in ("InfinityNormViolation", "BoundaryCondition", "ValidSignature", "ZeroPublicKey")},
    "testGroups": groups,
}
with open(sys.argv[2], "w") as f:
    json.dump(out, f, indent=2)
    f.write("\n")
```
