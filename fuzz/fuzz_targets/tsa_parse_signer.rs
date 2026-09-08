#![no_main]
//! Fuzz the best-effort TSA identity reader in `src/tsa.rs`.
//!
//! `parse_token_signer` walks further into the same attacker-reachable CMS
//! `SignedData` as `parse_token` — into the embedded certificate set and the
//! single `SignerInfo` — and its output ends up in `list`/`verify` lines and
//! the court kit's custody record. It must never panic, and when it does
//! return an identity the fields must be internally consistent: a subject
//! commonName is read from the MATCHED certificate, so it can never appear
//! without that certificate's hash.

use libfuzzer_sys::fuzz_target;
use witness_kernel::tsa;

fuzz_target!(|data: &[u8]| {
    if let Ok(signer) = tsa::parse_token_signer(data) {
        assert!(
            !(signer.cert_sha256.is_none() && signer.cert_subject_cn.is_some()),
            "a commonName was reported without the certificate it was read from"
        );
        assert_eq!(signer.sid_hex.len(), 64, "sid_hex is always a SHA-256 hex digest");
    }
});
