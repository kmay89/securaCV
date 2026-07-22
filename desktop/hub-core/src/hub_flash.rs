//! hub_flash — the gate between "everything checks out" and the destructive write.
//!
//! This is where the whole safety chain converges. Writing a raw disk is the one
//! irreversible thing the flasher does, so it must be *impossible* to reach
//! without having (1) verified the image, (2) chosen a disk the safety gate
//! accepts, and (3) had the operator confirm. Those preconditions are encoded in
//! the type system: the write entry point (a later change, in the app) will take
//! a [`WriteAuthorization`] by value, and the ONLY way to obtain one is
//! [`authorize_write`] — which demands a [`VerifiedImage`] proof (bound to the
//! plan's image), and re-checks the target through [`crate::hub_disk::classify`].
//! "Write without verify + an eligible
//! target + confirmation" is therefore not a bug you can introduce; it doesn't
//! compile.

use crate::hub_disk::{classify, Eligibility, Refusal, TargetDisk};
use crate::hub_image::{VerifiedImage, WritePlan};

/// Why a write can't be authorized. Verification is deliberately *not* a case
/// here — it's required at the type level, since [`authorize_write`] cannot be
/// called without a [`VerifiedImage`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AuthzError {
    /// The verification proof is for a different image than this plan — e.g. the
    /// board/image was changed after verifying. The image must be re-verified.
    VerificationDoesNotMatchPlan,
    /// The chosen target is one the safety gate refuses (system disk, internal
    /// fixed disk, too small, unknown size). Carries the specific reason.
    TargetRefused(Refusal),
    /// The operator hasn't explicitly confirmed the write yet.
    NotConfirmed,
}

impl AuthzError {
    pub fn message(&self) -> String {
        match self {
            AuthzError::VerificationDoesNotMatchPlan => {
                "this image needs to be re-verified before writing (the selection changed after it \
                 was checked)"
                    .to_string()
            }
            AuthzError::TargetRefused(r) => format!("can't write to this disk — {}", r.reason()),
            AuthzError::NotConfirmed => "confirm the disk to write before flashing".to_string(),
        }
    }
}

/// Proof that a destructive write is authorized. It has no public constructor:
/// [`authorize_write`] is the only way to build one, so holding a
/// `WriteAuthorization` *is* proof that the image verified, the target is a legal
/// eligible disk, and the operator confirmed. The write entry point takes this by
/// value — you cannot call it any other way.
#[derive(Debug, Clone)]
pub struct WriteAuthorization {
    plan: WritePlan,
    target_path: String,
}

impl WriteAuthorization {
    /// The resolved image plan to write (board, URL, expected hash, …).
    pub fn plan(&self) -> &WritePlan {
        &self.plan
    }
    /// The device path to write to, e.g. `/dev/sdb`.
    pub fn target_path(&self) -> &str {
        &self.target_path
    }
}

/// The one and only gate to a destructive write. It requires, all at once:
///
///   * a [`VerifiedImage`] — proof the download passed verification. You cannot
///     even *call* this function without one, so an unverified image can never be
///     authorized;
///   * a `target` disk that STILL classifies as eligible right now — re-checked
///     here (belt-and-suspenders over enumeration), so a stale or wrong target
///     can never slip between "offered" and "written";
///   * explicit operator confirmation.
///
/// On success it returns a [`WriteAuthorization`]; there is no other way to make
/// one, so downstream code that holds it can trust every precondition was met.
pub fn authorize_write(
    plan: WritePlan,
    verified: &VerifiedImage,
    target: &TargetDisk,
    operator_confirmed: bool,
) -> Result<WriteAuthorization, AuthzError> {
    // The proof must be for THIS plan's image. A token minted for a
    // previously-selected image (before the operator changed board/version)
    // proves nothing about the image we're about to write, so reject it.
    if verified.image_url() != plan.image_url {
        return Err(AuthzError::VerificationDoesNotMatchPlan);
    }
    // Re-run the safety gate on the exact disk we're about to write. Even though
    // enumeration only ever offered eligible disks, this closes the gap between
    // "offered" and "written": never authorize a disk `classify` refuses.
    if let Eligibility::Refused(reason) = classify(target) {
        return Err(AuthzError::TargetRefused(reason));
    }
    if !operator_confirmed {
        return Err(AuthzError::NotConfirmed);
    }
    Ok(WriteAuthorization {
        plan,
        target_path: target.path.clone(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hub_image::verify_download;

    /// A minimal plan (its fields are public); the write path only needs the
    /// board id + URL from it here.
    fn a_plan() -> WritePlan {
        WritePlan {
            board_id: "rpi5-64".to_string(),
            os_label: "Home Assistant OS 18.1".to_string(),
            image_url: "https://example/haos_rpi5-64-18.1.img.xz".to_string(),
            expected_sha256: None,
            min_card_bytes: 28 * 1024 * 1024 * 1024,
            warnings: vec![],
        }
    }

    /// A real [`VerifiedImage`] — the only way to get one is a passing verify.
    fn a_verified_image() -> VerifiedImage {
        let sha = "a".repeat(64);
        verify_download(&a_plan(), &sha, Some(&sha)).expect("verify passes")
    }

    fn eligible_card() -> TargetDisk {
        TargetDisk {
            path: "/dev/sdb".to_string(),
            model: "SanDisk 64GB".to_string(),
            size_bytes: 64 * 1024 * 1024 * 1024,
            removable: true,
            external: false,
            system: false,
            has_mounts: false,
        }
    }

    #[test]
    fn a_verified_image_eligible_disk_and_confirmation_authorize_the_write() {
        let vi = a_verified_image();
        let authz = authorize_write(a_plan(), &vi, &eligible_card(), true).expect("authorized");
        assert_eq!(authz.plan().board_id, "rpi5-64");
        assert_eq!(authz.target_path(), "/dev/sdb");
    }

    #[test]
    fn a_system_disk_target_is_refused_even_with_verification_and_confirmation() {
        let vi = a_verified_image();
        let sys = TargetDisk {
            system: true,
            ..eligible_card()
        };
        assert_eq!(
            authorize_write(a_plan(), &vi, &sys, true).unwrap_err(),
            AuthzError::TargetRefused(Refusal::SystemDisk)
        );
    }

    #[test]
    fn an_internal_fixed_disk_target_is_refused() {
        let vi = a_verified_image();
        let internal = TargetDisk {
            removable: false,
            external: false,
            ..eligible_card()
        };
        assert_eq!(
            authorize_write(a_plan(), &vi, &internal, true).unwrap_err(),
            AuthzError::TargetRefused(Refusal::InternalFixedDisk)
        );
    }

    #[test]
    fn without_confirmation_there_is_no_authorization() {
        let vi = a_verified_image();
        assert_eq!(
            authorize_write(a_plan(), &vi, &eligible_card(), false).unwrap_err(),
            AuthzError::NotConfirmed
        );
    }

    #[test]
    fn a_refused_target_wins_over_a_missing_confirmation() {
        // The most dangerous condition (a disk we must never write) is reported
        // even when confirmation is also missing.
        let vi = a_verified_image();
        let sys = TargetDisk {
            system: true,
            ..eligible_card()
        };
        assert_eq!(
            authorize_write(a_plan(), &vi, &sys, false).unwrap_err(),
            AuthzError::TargetRefused(Refusal::SystemDisk)
        );
    }

    #[test]
    fn a_proof_for_a_different_image_cannot_authorize_this_plan() {
        // Verify image A, then try to authorize a DIFFERENT plan (image B) using
        // A's proof — the stale-token attack the binding closes.
        let sha = "a".repeat(64);
        let proof_a = verify_download(&a_plan(), &sha, Some(&sha)).unwrap();
        let plan_b = WritePlan {
            board_id: "rpi4-64".to_string(),
            image_url: "https://example/haos_rpi4-64-18.1.img.xz".to_string(),
            ..a_plan()
        };
        assert_eq!(
            authorize_write(plan_b, &proof_a, &eligible_card(), true).unwrap_err(),
            AuthzError::VerificationDoesNotMatchPlan
        );
    }

    #[test]
    fn a_matching_proof_still_authorizes() {
        // Sanity: the same proof against the plan it verified is accepted.
        let sha = "a".repeat(64);
        let proof = verify_download(&a_plan(), &sha, Some(&sha)).unwrap();
        assert!(authorize_write(a_plan(), &proof, &eligible_card(), true).is_ok());
    }

    #[test]
    fn authz_errors_render_human_text() {
        assert!(!AuthzError::NotConfirmed.message().is_empty());
        assert!(!AuthzError::VerificationDoesNotMatchPlan
            .message()
            .is_empty());
        assert!(AuthzError::TargetRefused(Refusal::SystemDisk)
            .message()
            .contains("computer runs from"));
    }
}
