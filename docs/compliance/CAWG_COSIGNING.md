# CAWG co-signing — design note (item 7 CP5)

**Status: designed, not built, and gated.** This note is the design record
the `designed` rows `c2pa_conformance` cawg-identity and
`enterprise_assurance` cawg-cosigning point at. Item 7's CP5 builds it as
profile v2 of the C2PA witness-manifest profile, and rewrites this page into
the guide. It waits for item 6's CC1 (the profile and its checker) and for an
upstream `c2pa` release that encodes the identity assertion's
`signer_payload` with CBOR Core Deterministic Encoding (tracked as
`upstream:c2pa-cawg-cde`). The CAWG identity assertion 1.3 sources were read
from the working group's GitHub repository; cawg.io itself was unreachable,
and byte equality of those sources with the ratified text is unverified.

## The principal

A CAWG identity assertion names **the person the cited receipt records**:

- for a self-export, the operator the device-key holder declared in the
  export receipt's context;
- for a break-glass export, the requester in the authorizing grant's
  context, whom the trustees consented to;
- for disclosed frames, the grant reached through the unseal and the
  redemption.

Exports authorized by an API capability or a paired credential are refused:
they name a credential, not a person. A trustee, the device, a reviewer or a
custody witness is never the principal.

## The credential and the binding

The receipt must record a `requester_key`, and the certificate's key must
equal it. That key is the only binding the receipt carries; without it a name
is just a string. The credential is a dedicated Ed25519 key and an X.509 leaf
over it, either self-issued or issued by the operator's own organization CA:
EKU documentSigning only, no CA flag, subject CN (and O only when
organization-issued, never SecuraCV), and no AIA, CRL distribution point or
OCSP, so a validator has nothing to fetch. SecuraCV issues no identity
certificate. Self-issued trust is self-asserted: the binding is the
trustee-consented (or, on a self-export, declared) key, not identity
proofing.

## The payload

`sig_type` `cawg.x509.cose`; the referenced assertions are exactly the hard
binding, the actions and the witness assertion, each once; the role is
`cawg.publisher` only (the person published the disclosure; they did not
create the recording). No time stamp sits inside the identity assertion:
CAWG does not require one for documentSigning, and the claim's own time
stamp (item 6 CC3) is recommended as our choice, not CAWG's requirement.

## Profile v2 and the checker

Profile v2 is profile v1 plus at most one `cawg.identity` per manifest. A
manifest without one keeps writing v1, unchanged. A v1 checker meeting a v2
manifest says it is newer than the build and does not check it. The writer
refuses, before writing anything, a receipt that names no person, a missing
or mismatched requester key, a certificate naming someone else, a key reused
from a trustee, the device, the C2PA leaf, a reviewer or a witness, and a
certificate outside the profile; off a terminal it needs explicit
confirmation. The checker repeats every binding, prints names only on
request, and reports a missing identity anchor as "not anchored", never as
invalid.

## The deterministic-encoding gate

CAWG requires `signer_payload` in CBOR Core Deterministic Encoding. The
`c2pa` crate's holder signs, and its verifier re-encodes, in declaration
order. So the crate's assertions are expected to fail at a validator that
applies CDE, and a CDE-correct assertion is expected to fail the crate's own
checker. Neither direction is tested (no second validator was available).
CP5 starts when an upstream release uses deterministic encoding on both
sides; its first test pins the bytes the crate signs, so the change is
noticed when it comes.

## What the assertion reveals

One person's printed name, chosen by that person, together with a public
key. The key is the same on every manifest they co-sign, so those
disclosures are linkable to each other, as a handwritten signature would be.
Nothing about observed people, and no time beyond the claim's optional time
stamp, which dates the signing act.

`spec/co_signing.md` (an event-hash endorsement) is a different mechanism.
