//! Admin approvals and holder revocation proofs, with domain separation.
//!
//! An approval is over the roster digest `d = SHA-256(signing bytes)`:
//!
//! | `alg` / `kind`      | Signed message |
//! |---------------------|----------------|
//! | `ed25519` / `cli`   | Ed25519 over `"securacv-hk-admin-v1\0" ‖ d` |
//! | `es256` / `enclave` | ECDSA P-256 SHA-256 over `"securacv-hk-admin-v1\0" ‖ d`, DER |
//! | `es256` / `fido2`   | an assertion with `clientDataHash = SHA-256("securacv-hk-admin-v1\0" ‖ d)`, UV required |
//!
//! A member's own credential may revoke that member ("revoke by holder"),
//! over its own domain, `"securacv-hk-revoke-v1\0" ‖ d`, signed `es256`
//! directly or as a FIDO2 assertion over `SHA-256` of it (UP required, UV
//! not), as the spec (`spec/household_keys_v0.md` §5.2) states. Cards revoke
//! only through an admin in v0. The hub countersigns every accepted roster
//! with Ed25519 over `"securacv-hk-hub-roster-v1\0" ‖ d` (spec §5.1).
//!
//! Approvals are counted by **distinct admin handle**. One admin is one
//! person, decided at the enrollment ceremony; the hub never infers distinct
//! people from distinct key bytes (and [`crate::roster::Roster::validate`]
//! refuses one key under two handles, and one credential as both an admin
//! and a member).

use crate::roster::{AdminAlg, AdminEntry, AdminKind, Handle, MemberAlg, MemberEntry};
use crate::{hex, verify, Error, Result};
use ed25519_dalek::Signer as _;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

/// Admin approval domain prefix.
pub const ADMIN_DOMAIN: &[u8; 21] = b"securacv-hk-admin-v1\0";
/// Holder revocation domain prefix.
pub const REVOKE_DOMAIN: &[u8; 22] = b"securacv-hk-revoke-v1\0";
/// Hub roster signature domain prefix.
pub const HUB_ROSTER_DOMAIN: &[u8; 26] = b"securacv-hk-hub-roster-v1\0";

/// `"securacv-hk-admin-v1\0" ‖ digest`.
pub fn admin_preimage(digest: &[u8; 32]) -> Vec<u8> {
    let mut m = ADMIN_DOMAIN.to_vec();
    m.extend_from_slice(digest);
    m
}

/// `"securacv-hk-revoke-v1\0" ‖ digest`.
pub fn revoke_preimage(digest: &[u8; 32]) -> Vec<u8> {
    let mut m = REVOKE_DOMAIN.to_vec();
    m.extend_from_slice(digest);
    m
}

/// `"securacv-hk-hub-roster-v1\0" ‖ digest`, what the hub roster key signs.
pub fn hub_preimage(digest: &[u8; 32]) -> Vec<u8> {
    let mut m = HUB_ROSTER_DOMAIN.to_vec();
    m.extend_from_slice(digest);
    m
}

/// One signature from an admin (or, for a holder proof, a member).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Approval {
    /// The signer's handle (hex).
    pub handle: String,
    /// The signature (hex): Ed25519 64 bytes, or ES256 DER.
    pub sig: String,
    /// FIDO2 only: the authenticator data (hex).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub authenticator_data: Option<String>,
}

impl Approval {
    fn parts(&self) -> Option<(Handle, Vec<u8>, Option<Vec<u8>>)> {
        let h = hex::decode_array::<16>(&self.handle).ok()?;
        let s = hex::decode(&self.sig).ok()?;
        let ad = match &self.authenticator_data {
            Some(a) => Some(hex::decode(a).ok()?),
            None => None,
        };
        Some((h, s, ad))
    }

    /// The signer's handle, if well-formed.
    pub fn handle(&self) -> Option<Handle> {
        hex::decode_array::<16>(&self.handle).ok()
    }
}

/// Verify one approval against one admin entry.
pub fn verify_approval(admin: &AdminEntry, digest: &[u8; 32], a: &Approval) -> bool {
    let Some((h, sig, ad)) = a.parts() else {
        return false;
    };
    if h != admin.handle {
        return false;
    }
    let pre = admin_preimage(digest);
    match (admin.alg, admin.kind, ad) {
        (AdminAlg::Ed25519, AdminKind::Cli, None) => {
            let Ok(pk) = <[u8; 32]>::try_from(admin.public_key.as_slice()) else {
                return false;
            };
            let Ok(vk) = ed25519_dalek::VerifyingKey::from_bytes(&pk) else {
                return false;
            };
            let Ok(sig) = ed25519_dalek::Signature::from_slice(&sig) else {
                return false;
            };
            vk.verify_strict(&pre, &sig).is_ok()
        }
        (AdminAlg::Es256, AdminKind::Enclave, None) => verify::es256(&admin.public_key, &pre, &sig),
        (AdminAlg::Es256, AdminKind::Fido2, Some(ad)) => {
            let Some(rp) = admin.rp_id.as_deref() else {
                return false;
            };
            let cdh: [u8; 32] = Sha256::digest(&pre).into();
            verify::fido2_es256(&admin.public_key, rp, &cdh, &ad, &sig, true)
        }
        _ => false,
    }
}

/// Count the distinct admin handles in `admins` with a valid approval over
/// `digest`. Unknown handles, duplicates and bad signatures count zero.
pub fn count_approvals(admins: &[AdminEntry], digest: &[u8; 32], approvals: &[Approval]) -> usize {
    let mut seen: Vec<Handle> = Vec::new();
    for a in approvals {
        let Some(h) = a.handle() else { continue };
        if seen.contains(&h) {
            continue;
        }
        if let Some(admin) = admins.iter().find(|x| x.handle == h) {
            if verify_approval(admin, digest, a) {
                seen.push(h);
            }
        }
    }
    seen.len()
}

/// Verify a holder revocation proof against the member being revoked.
pub fn verify_holder_proof(member: &MemberEntry, digest: &[u8; 32], p: &Approval) -> bool {
    let Some((h, sig, ad)) = p.parts() else {
        return false;
    };
    if h != member.handle {
        return false;
    }
    let Some(pk) = member.public_key.as_deref() else {
        return false;
    };
    let pre = revoke_preimage(digest);
    match (member.alg, ad) {
        (MemberAlg::Es256, None) => verify::es256(pk, &pre, &sig),
        (MemberAlg::Fido2Es256, Some(ad)) => {
            let Some(rp) = member.rp_id.as_deref() else {
                return false;
            };
            let cdh: [u8; 32] = Sha256::digest(&pre).into();
            verify::fido2_es256(pk, rp, &cdh, &ad, &sig, false)
        }
        _ => false,
    }
}

/// Sign an approval with an Ed25519 CLI admin key.
pub fn sign_cli(key: &ed25519_dalek::SigningKey, handle: &Handle, digest: &[u8; 32]) -> Approval {
    let sig = key.sign(&admin_preimage(digest));
    Approval {
        handle: hex::encode(handle),
        sig: hex::encode(&sig.to_bytes()),
        authenticator_data: None,
    }
}

/// Sign an approval with an ES256 enclave-style admin key.
pub fn sign_enclave(key: &p256::ecdsa::SigningKey, handle: &Handle, digest: &[u8; 32]) -> Approval {
    use p256::ecdsa::signature::Signer;
    let sig: p256::ecdsa::Signature = key.sign(&admin_preimage(digest));
    Approval {
        handle: hex::encode(handle),
        sig: hex::encode(sig.to_der().as_bytes()),
        authenticator_data: None,
    }
}

/// Produce a FIDO2-style assertion over `client_data_hash` with a software
/// P-256 key (tests and the reference client; a real authenticator does this
/// in hardware).
pub fn soft_fido2_assert(
    key: &p256::ecdsa::SigningKey,
    rp_id: &str,
    flags: u8,
    client_data_hash: &[u8; 32],
) -> (Vec<u8>, Vec<u8>) {
    use p256::ecdsa::signature::Signer;
    let ad = verify::build_auth_data(rp_id, flags, 0);
    let mut m = ad.clone();
    m.extend_from_slice(client_data_hash);
    let sig: p256::ecdsa::Signature = key.sign(&m);
    (ad, sig.to_der().as_bytes().to_vec())
}

/// Sign an approval as a FIDO2 admin (software stand-in for an authenticator).
pub fn sign_fido2_admin(
    key: &p256::ecdsa::SigningKey,
    handle: &Handle,
    rp_id: &str,
    flags: u8,
    digest: &[u8; 32],
) -> Approval {
    let cdh: [u8; 32] = Sha256::digest(admin_preimage(digest)).into();
    let (ad, sig) = soft_fido2_assert(key, rp_id, flags, &cdh);
    Approval {
        handle: hex::encode(handle),
        sig: hex::encode(&sig),
        authenticator_data: Some(hex::encode(&ad)),
    }
}

/// Sign a holder revocation proof with an `es256` member key.
pub fn sign_holder_es256(
    key: &p256::ecdsa::SigningKey,
    handle: &Handle,
    digest: &[u8; 32],
) -> Approval {
    use p256::ecdsa::signature::Signer;
    let sig: p256::ecdsa::Signature = key.sign(&revoke_preimage(digest));
    Approval {
        handle: hex::encode(handle),
        sig: hex::encode(sig.to_der().as_bytes()),
        authenticator_data: None,
    }
}

/// Hub roster signature over a digest.
pub fn hub_sign(key: &ed25519_dalek::SigningKey, digest: &[u8; 32]) -> [u8; 64] {
    key.sign(&hub_preimage(digest)).to_bytes()
}

/// Verify a hub roster signature.
pub fn hub_verify(
    key: &ed25519_dalek::VerifyingKey,
    digest: &[u8; 32],
    sig: &[u8; 64],
) -> Result<()> {
    let s = ed25519_dalek::Signature::from_bytes(sig);
    key.verify_strict(&hub_preimage(digest), &s)
        .map_err(|_| Error::Rejected("hub roster signature does not verify"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::roster::tests::{ed_admin, p256_pub};
    use crate::verify::{FLAG_UP, FLAG_UV};

    fn ed_key(seed: u8) -> ed25519_dalek::SigningKey {
        ed25519_dalek::SigningKey::from_bytes(&[seed; 32])
    }

    fn p_key(seed: u8) -> p256::ecdsa::SigningKey {
        p256::ecdsa::SigningKey::from_slice(&[seed; 32]).unwrap()
    }

    fn enclave_admin(handle: u8, seed: u8) -> AdminEntry {
        AdminEntry {
            handle: [handle; 16],
            alg: AdminAlg::Es256,
            kind: AdminKind::Enclave,
            public_key: p256_pub(seed),
            rp_id: None,
            credential_id: None,
        }
    }

    fn fido_admin(handle: u8, seed: u8) -> AdminEntry {
        AdminEntry {
            handle: [handle; 16],
            alg: AdminAlg::Es256,
            kind: AdminKind::Fido2,
            public_key: p256_pub(seed),
            rp_id: Some("hk-admin.example".into()),
            credential_id: Some(vec![1, 2, 3]),
        }
    }

    const D: [u8; 32] = [0x5a; 32];

    #[test]
    fn each_admin_kind_verifies_with_its_own_message() {
        let a = ed_admin(1, 1);
        assert!(verify_approval(
            &a,
            &D,
            &sign_cli(&ed_key(1), &a.handle, &D)
        ));
        let e = enclave_admin(2, 2);
        assert!(verify_approval(
            &e,
            &D,
            &sign_enclave(&p_key(2), &e.handle, &D)
        ));
        let f = fido_admin(3, 3);
        let ok = sign_fido2_admin(
            &p_key(3),
            &f.handle,
            "hk-admin.example",
            FLAG_UP | FLAG_UV,
            &D,
        );
        assert!(verify_approval(&f, &D, &ok));
    }

    #[test]
    fn fido2_admin_requires_uv_and_its_rp() {
        let f = fido_admin(3, 3);
        let no_uv = sign_fido2_admin(&p_key(3), &f.handle, "hk-admin.example", FLAG_UP, &D);
        assert!(!verify_approval(&f, &D, &no_uv));
        let wrong_rp =
            sign_fido2_admin(&p_key(3), &f.handle, "other.example", FLAG_UP | FLAG_UV, &D);
        assert!(!verify_approval(&f, &D, &wrong_rp));
    }

    #[test]
    fn domain_separation_holds() {
        // A signature over the bare digest, over the revoke preimage or over
        // the hub preimage is not an admin approval.
        use p256::ecdsa::signature::Signer;
        let e = enclave_admin(2, 2);
        for m in [D.to_vec(), revoke_preimage(&D), hub_preimage(&D)] {
            let sig: p256::ecdsa::Signature = p_key(2).sign(&m);
            let a = Approval {
                handle: hex::encode(&e.handle),
                sig: hex::encode(sig.to_der().as_bytes()),
                authenticator_data: None,
            };
            assert!(!verify_approval(&e, &D, &a));
        }
        let a = ed_admin(1, 1);
        let sig = ed_key(1).sign(&D);
        let bare = Approval {
            handle: hex::encode(&a.handle),
            sig: hex::encode(&sig.to_bytes()),
            authenticator_data: None,
        };
        assert!(!verify_approval(&a, &D, &bare));
        // A holder proof is not an admin approval even from the same key.
        let holder = sign_holder_es256(&p_key(2), &e.handle, &D);
        assert!(!verify_approval(&e, &D, &holder));
    }

    #[test]
    fn approvals_count_by_distinct_handle() {
        let admins = vec![ed_admin(1, 1), ed_admin(2, 2)];
        let a1 = sign_cli(&ed_key(1), &[1; 16], &D);
        let a2 = sign_cli(&ed_key(2), &[2; 16], &D);
        assert_eq!(
            count_approvals(&admins, &D, &[a1.clone(), a1.clone(), a1.clone()]),
            1
        );
        assert_eq!(count_approvals(&admins, &D, &[a1.clone(), a2.clone()]), 2);
        // A valid signature presented under someone else's handle counts zero.
        let mut forged = a1.clone();
        forged.handle = hex::encode(&[2; 16]);
        assert_eq!(count_approvals(&admins, &D, &[a1.clone(), forged]), 1);
        // Unknown handles count zero.
        let stranger = sign_cli(&ed_key(9), &[9; 16], &D);
        assert_eq!(count_approvals(&admins, &D, &[stranger]), 0);
        // Wrong digest counts zero.
        assert_eq!(count_approvals(&admins, &[0u8; 32], &[a1, a2]), 0);
    }

    #[test]
    fn hub_signature_round_trip() {
        let k = ed_key(7);
        let s = hub_sign(&k, &D);
        assert!(hub_verify(&k.verifying_key(), &D, &s).is_ok());
        assert!(hub_verify(&k.verifying_key(), &[1; 32], &s).is_err());
    }
}
