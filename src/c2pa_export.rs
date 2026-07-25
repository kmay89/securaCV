//! C2PA Content Credentials for export bundles (feature `c2pa-export`).
//!
//! Wraps a witness export bundle in an industry-standard C2PA manifest so the
//! artifact is verifiable by any Content Credentials tool, not only our own
//! `export_verify`. Design doc: `docs/design/c2pa_export.md`.
//!
//! Shape of the integration:
//!
//! - **Sidecar manifest** (`<bundle>.c2pa`): the bundle file is a JSON
//!   artifact, not a C2PA-supported container, so the manifest travels next
//!   to it as an external/remote-style manifest whose `c2pa.hash.data`
//!   assertion covers the *entire* bundle bytes (no exclusions). The bundle
//!   file itself stays byte-identical with or without C2PA.
//! - **Keys derive from the device seed** with HKDF domain separation
//!   (`securacv:c2pa:*:v1`), so the C2PA credential is bound to the same
//!   root secret as the witness chain but never reuses the chain's key.
//! - **The credential chain is reproducible.** Certificates use fixed
//!   validity, key-derived serials, and Ed25519's deterministic signatures,
//!   so `credentials_from_seed` yields byte-identical PEMs on every host —
//!   the trust anchor can always be re-derived, never lost.
//! - **The trust anchor is the device-local CA**, not a public trust list:
//!   a self-signed CA issues a non-CA signing leaf (digitalSignature +
//!   emailProtection EKU + AKI), which is the shape the C2PA certificate
//!   profile requires. Public verifiers will report the manifest as signed
//!   but not on the public trust list; verifiers given the device CA (or the
//!   seed) validate to full trust. The hash-chained witness log remains the
//!   root of trust either way (Principle 4).
//! - **Fully offline**: no timestamp authority, no HTTP backend compiled in.
//!
//! A custom `org.securacv.witness` assertion binds the manifest to the
//! witness chain by embedding the signed export-receipt entry hash, so the
//! C2PA layer and the tamper-evident log corroborate each other.

use anyhow::{anyhow, Result};
use c2pa::{
    assertions::DataHash, create_signer, settings::Settings, Builder, Context, Reader, SigningAlg,
    ValidationState,
};
use ed25519_dalek::pkcs8::{spki::der::pem::LineEnding, EncodePrivateKey};
use ed25519_dalek::SigningKey;
use hkdf::Hkdf;
use rcgen::{
    date_time_ymd, BasicConstraints, CertificateParams, DistinguishedName, DnType,
    ExtendedKeyUsagePurpose, IsCa, Issuer, KeyPair, KeyUsagePurpose, SerialNumber, PKCS_ED25519,
};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::io::Cursor;
use zeroize::Zeroizing;

/// Assertion label binding the manifest to the witness chain.
pub const WITNESS_ASSERTION_LABEL: &str = "org.securacv.witness";

/// HKDF domain-separation contexts. Versioned so a future scheme change can
/// coexist with the old one instead of silently changing keys.
const HKDF_INFO_LEAF: &[u8] = b"securacv:c2pa:signing:v1";
const HKDF_INFO_CA: &[u8] = b"securacv:c2pa:ca:v1";

/// C2PA signing credentials derived from the device key seed.
///
/// Everything here is deterministic in the seed; nothing needs to be stored.
pub struct C2paCredentials {
    /// Signing leaf followed by the issuing device CA (PEM bundle) — the
    /// chain embedded in every manifest.
    pub cert_chain_pem: String,
    /// The device CA alone (PEM) — the trust anchor to hand to verifiers.
    pub ca_cert_pem: String,
    /// PKCS#8 PEM of the leaf private key (zeroized on drop).
    key_pem: Zeroizing<String>,
}

impl C2paCredentials {
    /// Hex SHA-256 fingerprint of the device CA certificate (DER).
    pub fn ca_fingerprint(&self) -> Result<String> {
        let der = pem_to_der(&self.ca_cert_pem)?;
        Ok(hex::encode(Sha256::digest(der)))
    }
}

/// Data bound into the `org.securacv.witness` assertion: enough for a
/// verifier holding the manifest to cross-check the witness chain, and for a
/// verifier holding only the chain to locate this disclosure.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct WitnessBinding {
    /// Hex hash of the signed export-receipt entry appended to the sealed
    /// log for this disclosure (Invariant IV).
    pub receipt_entry_hash: String,
    /// Hex hash of the export artifact as committed in that receipt.
    pub artifact_hash: String,
    /// Hex Ed25519 device verifying key for the witness chain itself.
    pub device_public_key: String,
    /// Authorization mode recorded on the receipt (`self_export` /
    /// `break_glass`).
    pub auth_mode: String,
    /// Ruleset identifier the export was interpreted under.
    pub ruleset_id: String,
    /// Kernel version that produced the bundle.
    pub kernel_version: String,
}

/// Derive a domain-separated Ed25519 key from the device seed. Seed policy
/// (length, placeholder rejection) is enforced by `signing_key_from_seed`;
/// the device identity key is the HKDF input keying material, so C2PA keys
/// rotate together with device identity but never equal it.
fn derived_key(seed: &str, info: &[u8]) -> Result<SigningKey> {
    let device = crate::signing_key_from_seed(seed)?;
    let hk = Hkdf::<Sha256>::new(None, device.to_bytes().as_ref());
    let mut okm = Zeroizing::new([0u8; 32]);
    hk.expand(info, okm.as_mut())
        .expect("HKDF-SHA256 expand with 32-byte output cannot fail");
    Ok(SigningKey::from_bytes(&okm))
}

/// A serial number derived from the public key: deterministic, unique per
/// key, and masked positive so the DER integer never reads as negative.
fn serial_from_key(key: &SigningKey) -> SerialNumber {
    let digest = Sha256::digest(key.verifying_key().as_bytes());
    let mut serial = digest[..16].to_vec();
    serial[0] &= 0x7f;
    SerialNumber::from(serial)
}

fn rcgen_keypair(key: &SigningKey) -> Result<KeyPair> {
    let pem = key
        .to_pkcs8_pem(LineEnding::LF)
        .map_err(|e| anyhow!("PKCS#8 encoding failed: {e}"))?;
    KeyPair::from_pkcs8_pem_and_sign_algo(&pem, &PKCS_ED25519)
        .map_err(|e| anyhow!("rcgen key import failed: {e}"))
}

/// Short fingerprint of the *device* verifying key, used in subject CNs so
/// certificates from different devices are distinguishable at a glance.
fn device_fp(seed: &str) -> Result<String> {
    let vk = crate::verifying_key_from_seed(seed)?;
    Ok(hex::encode(Sha256::digest(vk.as_bytes()))[..12].to_string())
}

/// Build the reproducible device credential chain from the seed.
pub fn credentials_from_seed(seed: &str) -> Result<C2paCredentials> {
    let fp = device_fp(seed)?;
    let ca_key = derived_key(seed, HKDF_INFO_CA)?;
    let leaf_key = derived_key(seed, HKDF_INFO_LEAF)?;

    // Fixed validity keeps the chain reproducible and spares devices an
    // expiry-rotation ceremony; the C2PA validator checks validity against
    // the claim, and the witness chain is the real freshness signal.
    let not_before = date_time_ymd(2026, 1, 1);
    let not_after = date_time_ymd(2126, 1, 1);

    let mut ca_params = CertificateParams::default();
    ca_params.distinguished_name = DistinguishedName::new();
    ca_params
        .distinguished_name
        .push(DnType::CommonName, format!("SecuraCV Device CA {fp}"));
    ca_params
        .distinguished_name
        .push(DnType::OrganizationName, "SecuraCV");
    ca_params.is_ca = IsCa::Ca(BasicConstraints::Constrained(0));
    ca_params.key_usages = vec![KeyUsagePurpose::KeyCertSign, KeyUsagePurpose::CrlSign];
    ca_params.serial_number = Some(serial_from_key(&ca_key));
    ca_params.not_before = not_before;
    ca_params.not_after = not_after;

    let ca_keypair = rcgen_keypair(&ca_key)?;
    let ca_cert = ca_params
        .self_signed(&ca_keypair)
        .map_err(|e| anyhow!("device CA generation failed: {e}"))?;
    let ca_cert_pem = ca_cert.pem();

    // The signing leaf follows the C2PA certificate profile: non-CA,
    // digitalSignature key usage, an allowed EKU (emailProtection), and an
    // authority key identifier pointing at the device CA.
    let mut leaf_params = CertificateParams::default();
    leaf_params.distinguished_name = DistinguishedName::new();
    leaf_params.distinguished_name.push(
        DnType::CommonName,
        format!("SecuraCV Witness Export Signer {fp}"),
    );
    leaf_params
        .distinguished_name
        .push(DnType::OrganizationName, "SecuraCV");
    leaf_params.is_ca = IsCa::ExplicitNoCa;
    leaf_params.key_usages = vec![KeyUsagePurpose::DigitalSignature];
    leaf_params.extended_key_usages = vec![ExtendedKeyUsagePurpose::EmailProtection];
    leaf_params.use_authority_key_identifier_extension = true;
    leaf_params.serial_number = Some(serial_from_key(&leaf_key));
    leaf_params.not_before = not_before;
    leaf_params.not_after = not_after;

    let leaf_keypair = rcgen_keypair(&leaf_key)?;
    let issuer = Issuer::new(ca_params, ca_keypair);
    let leaf_cert = leaf_params
        .signed_by(&leaf_keypair, &issuer)
        .map_err(|e| anyhow!("signing-leaf generation failed: {e}"))?;

    let key_pem = leaf_key
        .to_pkcs8_pem(LineEnding::LF)
        .map_err(|e| anyhow!("PKCS#8 encoding failed: {e}"))?;

    Ok(C2paCredentials {
        cert_chain_pem: format!("{}{}", leaf_cert.pem(), ca_cert_pem),
        ca_cert_pem,
        key_pem: Zeroizing::new(key_pem.to_string()),
    })
}

fn pem_to_der(pem: &str) -> Result<Vec<u8>> {
    let body: String = pem
        .lines()
        .filter(|l| !l.starts_with("-----"))
        .collect::<Vec<_>>()
        .join("");
    base64_decode(&body).ok_or_else(|| anyhow!("invalid PEM"))
}

/// Minimal strict base64 decoder (standard alphabet, `=` padding). Avoids a
/// new dependency for the one PEM-body decode a fingerprint needs.
fn base64_decode(s: &str) -> Option<Vec<u8>> {
    fn val(c: u8) -> Option<u32> {
        match c {
            b'A'..=b'Z' => Some((c - b'A') as u32),
            b'a'..=b'z' => Some((c - b'a' + 26) as u32),
            b'0'..=b'9' => Some((c - b'0' + 52) as u32),
            b'+' => Some(62),
            b'/' => Some(63),
            _ => None,
        }
    }
    let bytes: Vec<u8> = s.bytes().filter(|b| !b.is_ascii_whitespace()).collect();
    let trimmed = bytes
        .strip_suffix(b"==")
        .unwrap_or_else(|| bytes.strip_suffix(b"=").unwrap_or(&bytes));
    let mut out = Vec::with_capacity(trimmed.len() * 3 / 4);
    for chunk in trimmed.chunks(4) {
        let mut acc: u32 = 0;
        for &c in chunk {
            acc = (acc << 6) | val(c)?;
        }
        match chunk.len() {
            4 => out.extend_from_slice(&[(acc >> 16) as u8, (acc >> 8) as u8, acc as u8]),
            3 => {
                acc <<= 6;
                out.extend_from_slice(&[(acc >> 16) as u8, (acc >> 8) as u8]);
            }
            2 => {
                acc <<= 12;
                out.push((acc >> 16) as u8);
            }
            _ => return None,
        }
    }
    Some(out)
}

/// Sign a sidecar C2PA manifest over the exact bundle bytes.
///
/// Returns the raw JUMBF manifest store, to be written as `<bundle>.c2pa`.
pub fn sign_export_sidecar(
    bundle_bytes: &[u8],
    credentials: &C2paCredentials,
    binding: &WitnessBinding,
    title: &str,
) -> Result<Vec<u8>> {
    let signer = create_signer::from_keys(
        credentials.cert_chain_pem.as_bytes(),
        credentials.key_pem.as_bytes(),
        SigningAlg::Ed25519,
        None, // no timestamp authority: signing stays fully offline
    )
    .map_err(|e| anyhow!("C2PA signer construction failed: {e}"))?;

    let definition = serde_json::json!({
        "claim_generator_info": [{
            "name": "securacv-witness-kernel",
            "version": binding.kernel_version,
        }],
        "title": title,
        "format": "application/json",
        "assertions": [{
            "label": "c2pa.actions",
            "data": {
                "actions": [{
                    "action": "c2pa.created",
                    "digitalSourceType":
                        "http://cv.iptc.org/newscodes/digitalsourcetype/dataDrivenMedia",
                }]
            }
        }]
    });

    let mut builder = Builder::default()
        .with_definition(definition.to_string())
        .map_err(|e| anyhow!("C2PA manifest definition rejected: {e}"))?;
    builder.set_no_embed(true);
    builder
        .add_assertion(WITNESS_ASSERTION_LABEL, binding)
        .map_err(|e| anyhow!("witness assertion rejected: {e}"))?;
    // Seeds the hard-binding DataHash assertion slot; the placeholder bytes
    // themselves are irrelevant for a sidecar and are discarded.
    builder
        .data_hashed_placeholder(signer.reserve_size(), "application/c2pa")
        .map_err(|e| anyhow!("C2PA placeholder failed: {e}"))?;

    // Sidecar binding: hash the complete bundle file, no exclusions — the
    // manifest lives outside the asset, so every byte is covered.
    let mut data_hash = DataHash::new("org.securacv.export_bundle", "sha256");
    data_hash.set_hash(Sha256::digest(bundle_bytes).to_vec());

    builder
        .sign_data_hashed_embeddable(signer.as_ref(), &data_hash, "application/c2pa")
        .map_err(|e| anyhow!("C2PA signing failed: {e}"))
}

/// Outcome of verifying a sidecar manifest against bundle bytes.
#[derive(Debug)]
pub struct SidecarReport {
    /// Overall C2PA validation state (`Trusted` when the chain roots in the
    /// supplied anchor, `Valid` when consistent but unanchored).
    pub state: ValidationState,
    /// Manifest title, when present.
    pub title: Option<String>,
    /// The recovered witness binding, when present and well-formed.
    pub binding: Option<WitnessBinding>,
    /// Signature issuer common name, when present.
    pub issuer: Option<String>,
}

/// Verify `manifest_bytes` (a sidecar JUMBF store) against `bundle_bytes`,
/// trusting `ca_anchor_pem` as the root. Hard-fails on any validation error
/// (hash mismatch, bad signature, malformed manifest); an untrusted-but-
/// consistent manifest is returned with `state == ValidationState::Valid`
/// so the caller can decide how to present it.
pub fn verify_export_sidecar(
    manifest_bytes: &[u8],
    bundle_bytes: &[u8],
    ca_anchor_pem: &str,
) -> Result<SidecarReport> {
    let settings = Settings::new()
        .with_json(
            &serde_json::json!({
                "trust": { "user_anchors": ca_anchor_pem },
                "verify": { "verify_trust": true },
            })
            .to_string(),
        )
        .map_err(|e| anyhow!("C2PA trust settings rejected: {e}"))?;
    let context = Context::new()
        .with_settings(settings)
        .map_err(|e| anyhow!("C2PA context rejected: {e}"))?;

    let reader = Reader::from_context(context)
        .with_manifest_data_and_stream(
            manifest_bytes,
            "application/json",
            Cursor::new(bundle_bytes),
        )
        .map_err(|e| anyhow!("C2PA validation failed: {e}"))?;

    let state = reader.validation_state();
    if state == ValidationState::Invalid {
        let detail = reader
            .validation_status()
            .map(|statuses| {
                statuses
                    .iter()
                    .map(|s| s.code().to_string())
                    .collect::<Vec<_>>()
                    .join(", ")
            })
            .unwrap_or_else(|| "unknown".to_string());
        return Err(anyhow!("TAMPER: C2PA validation failed: {detail}"));
    }

    let manifest = reader
        .active_manifest()
        .ok_or_else(|| anyhow!("C2PA manifest store has no active manifest"))?;
    let binding = manifest
        .assertions()
        .iter()
        .find(|a| a.label().starts_with(WITNESS_ASSERTION_LABEL))
        .and_then(|a| a.to_assertion::<WitnessBinding>().ok());

    Ok(SidecarReport {
        state,
        title: manifest.title().map(str::to_string),
        binding,
        issuer: manifest.signature_info().and_then(|s| s.issuer.clone()),
    })
}

/// Convenience: derive the CA anchor PEM straight from a seed (what
/// `export_verify --device-key-seed` uses when no anchor file is given).
pub fn ca_anchor_from_seed(seed: &str) -> Result<String> {
    Ok(credentials_from_seed(seed)?.ca_cert_pem)
}

#[cfg(test)]
mod tests {
    use super::*;

    const SEED: &str = "devkey:test:c2pa-0123456789abcdef0123456789abcdef";
    const OTHER_SEED: &str = "devkey:test:c2pa-fedcba9876543210fedcba9876543210";

    fn test_binding() -> WitnessBinding {
        WitnessBinding {
            receipt_entry_hash: "aa".repeat(32),
            artifact_hash: "bb".repeat(32),
            device_public_key: "cc".repeat(32),
            auth_mode: "self_export".to_string(),
            ruleset_id: "ruleset:v0.1".to_string(),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        }
    }

    #[test]
    fn credentials_are_deterministic_and_domain_separated() {
        let a = credentials_from_seed(SEED).expect("credentials");
        let b = credentials_from_seed(SEED).expect("credentials");
        // Reproducible: the anchor can always be re-derived from the seed.
        assert_eq!(a.cert_chain_pem, b.cert_chain_pem);
        assert_eq!(a.ca_cert_pem, b.ca_cert_pem);
        assert_eq!(a.ca_fingerprint().unwrap(), b.ca_fingerprint().unwrap());
        // Different seed → different chain.
        let c = credentials_from_seed(OTHER_SEED).expect("credentials");
        assert_ne!(a.ca_cert_pem, c.ca_cert_pem);
        // Domain separation: no C2PA key ever equals the device identity key.
        let device = crate::signing_key_from_seed(SEED).unwrap();
        for info in [HKDF_INFO_LEAF, HKDF_INFO_CA] {
            let derived = derived_key(SEED, info).unwrap();
            assert_ne!(derived.to_bytes(), device.to_bytes());
        }
        assert_ne!(
            derived_key(SEED, HKDF_INFO_LEAF).unwrap().to_bytes(),
            derived_key(SEED, HKDF_INFO_CA).unwrap().to_bytes()
        );
    }

    #[test]
    fn sidecar_roundtrip_verifies_trusted() {
        let creds = credentials_from_seed(SEED).expect("credentials");
        let bundle = br#"{"artifact":{"batches":[]},"note":"test bundle"}"#;
        let manifest = sign_export_sidecar(bundle, &creds, &test_binding(), "test export")
            .expect("sign sidecar");

        let report =
            verify_export_sidecar(&manifest, bundle, &creds.ca_cert_pem).expect("verify sidecar");
        assert_eq!(report.state, ValidationState::Trusted);
        let binding = report.binding.expect("witness binding present");
        assert_eq!(binding, test_binding());
        assert_eq!(report.title.as_deref(), Some("test export"));
    }

    #[test]
    fn tampered_bundle_is_rejected() {
        let creds = credentials_from_seed(SEED).expect("credentials");
        let bundle = br#"{"artifact":{"batches":[]},"note":"test bundle"}"#.to_vec();
        let manifest = sign_export_sidecar(&bundle, &creds, &test_binding(), "test export")
            .expect("sign sidecar");

        let mut tampered = bundle.clone();
        *tampered.last_mut().unwrap() ^= 0x01;
        let err = verify_export_sidecar(&manifest, &tampered, &creds.ca_cert_pem)
            .expect_err("tamper must fail");
        assert!(err.to_string().contains("TAMPER"), "got: {err}");
    }

    #[test]
    fn wrong_anchor_is_not_trusted() {
        let creds = credentials_from_seed(SEED).expect("credentials");
        let wrong = credentials_from_seed(OTHER_SEED).expect("credentials");
        let bundle = br#"{"artifact":{"batches":[]}}"#;
        let manifest = sign_export_sidecar(bundle, &creds, &test_binding(), "test export")
            .expect("sign sidecar");

        // Consistent manifest, wrong root: must not report Trusted. (An Err
        // outcome — validators that treat an unanchored chain as a hard
        // failure — is also an acceptable non-Trusted result.)
        if let Ok(report) = verify_export_sidecar(&manifest, bundle, &wrong.ca_cert_pem) {
            assert_ne!(report.state, ValidationState::Trusted);
        }
    }

    #[test]
    fn base64_decoder_matches_expectations() {
        assert_eq!(base64_decode("aGVsbG8=").unwrap(), b"hello");
        assert_eq!(base64_decode("aGVsbG8h").unwrap(), b"hello!");
        assert_eq!(base64_decode("aA==").unwrap(), b"h");
        assert!(base64_decode("a!b").is_none());
    }
}
