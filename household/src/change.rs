//! Roster change rules, the epoch/prev chain and rollback refusal.
//!
//! | Change | Requires |
//! |---|---|
//! | Add members | `threshold` approvals of the **previous** roster's admins; when that threshold is 1 (the single-admin path), also a physical confirm on the hub |
//! | Revoke members | any one admin of the previous roster, or (for exactly one member) that member's own credential |
//! | Change the admin set or the threshold | `threshold` approvals of the **current** (previous-roster) admins, and the proposal must have been pending for 24 hours |
//! | Every change | `epoch + 1`, `prev` = digest of the previous roster |
//!
//! A change is exactly one of those kinds: an admin-set change may not also
//! touch members, and adds and revokes are separate changes, so the cheap
//! revoke path can never carry anything else. A member entry is never edited
//! in place (that would be an add disguised as nothing); replace it with a
//! revoke and an add under a new handle.
//!
//! The hub additionally signs every accepted roster's digest with its roster
//! key ([`crate::admin::hub_sign`]). [`check_not_rollback`] refuses any
//! roster below the highest epoch ever accepted, and a different roster at
//! that same epoch.

use crate::admin::{self, Approval};
use crate::roster::{Handle, MemberEntry, Roster};
use crate::{hex, Error, Result};
use serde_json::{json, Value};

/// How long an admin-set or threshold change must be pending.
pub const ADMIN_CHANGE_PENDING_SECS: u64 = 86_400;

/// What a change does.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChangeKind {
    /// Adds one or more members.
    AddMembers,
    /// Revokes one or more members.
    RevokeMembers,
    /// Changes the admin set and/or the threshold.
    AdminSet,
}

/// The conditions a change is checked under.
#[derive(Clone, Copy, Debug)]
pub enum Mode {
    /// Applying now, on the hub.
    Live {
        /// Hub wall clock, Unix seconds.
        now_unix: u64,
        /// When the hub first recorded the proposal, Unix seconds.
        proposed_at: u64,
        /// Whether a person confirmed physically at the hub.
        physical_confirm: bool,
    },
    /// Re-verifying an already-accepted chain at load. The pending timer and
    /// the physical confirm happened (or not) in the past and cannot be
    /// re-checked; every signature rule is.
    Replay,
}

/// An accepted roster with the approvals that admitted it and the hub's
/// signature over its digest.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SignedRoster {
    /// The roster.
    pub roster: Roster,
    /// Admin approvals over its digest.
    pub approvals: Vec<Approval>,
    /// A holder revocation proof, for a revoke by the member's own credential.
    pub holder_proof: Option<Approval>,
    /// The hub roster key's signature over the digest.
    pub hub_sig: [u8; 64],
}

impl SignedRoster {
    /// JSON form for the data directory.
    pub fn to_value(&self) -> Value {
        json!({
            "roster": self.roster.to_value(),
            "approvals": self.approvals,
            "holder_proof": self.holder_proof,
            "hub_sig": hex::encode(&self.hub_sig),
        })
    }

    /// Parse the JSON form (structure only; see [`verify_chain`]).
    pub fn from_value(v: &Value) -> Result<SignedRoster> {
        let obj = v
            .as_object()
            .ok_or(Error::Invalid("signed roster is not an object"))?;
        if obj.len() != 4 {
            return Err(Error::Invalid("signed roster has unexpected fields"));
        }
        let roster =
            Roster::from_value(obj.get("roster").ok_or(Error::Invalid("missing roster"))?)?;
        let approvals: Vec<Approval> = serde_json::from_value(
            obj.get("approvals")
                .cloned()
                .ok_or(Error::Invalid("missing approvals"))?,
        )
        .map_err(|_| Error::Invalid("bad approvals"))?;
        let holder_proof: Option<Approval> = serde_json::from_value(
            obj.get("holder_proof")
                .cloned()
                .ok_or(Error::Invalid("missing holder_proof"))?,
        )
        .map_err(|_| Error::Invalid("bad holder proof"))?;
        let hub_sig = hex::decode_array::<64>(
            obj.get("hub_sig")
                .and_then(Value::as_str)
                .ok_or(Error::Invalid("missing hub_sig"))?,
        )?;
        Ok(SignedRoster {
            roster,
            approvals,
            holder_proof,
            hub_sig,
        })
    }
}

fn handles(ms: &[MemberEntry]) -> Vec<Handle> {
    ms.iter().map(|m| m.handle).collect()
}

/// Work out what kind of change `next` is relative to `prev`, checking the
/// chain link and the one-kind-per-change rule.
pub fn classify(prev: &Roster, next: &Roster) -> Result<ChangeKind> {
    next.validate()?;
    if next.household != prev.household {
        return Err(Error::Rejected(
            "a roster change cannot change the household",
        ));
    }
    if Some(next.epoch) != prev.epoch.checked_add(1) {
        return Err(Error::Rejected("a roster change must be epoch + 1"));
    }
    if next.prev != prev.digest()? {
        return Err(Error::Rejected(
            "prev is not the digest of the current roster",
        ));
    }
    let admin_changed = next.admins != prev.admins || next.threshold != prev.threshold;
    let members_changed = next.members != prev.members;
    if admin_changed {
        if members_changed {
            return Err(Error::Rejected(
                "an admin-set change must not also change members",
            ));
        }
        return Ok(ChangeKind::AdminSet);
    }
    let prev_h = handles(&prev.members);
    let next_h = handles(&next.members);
    let added = next_h.iter().filter(|h| !prev_h.contains(h)).count();
    let removed = prev_h.iter().filter(|h| !next_h.contains(h)).count();
    for m in &next.members {
        if let Some(old) = prev.member(&m.handle) {
            if old != m {
                return Err(Error::Rejected("a member entry cannot be edited in place"));
            }
        }
    }
    match (added, removed) {
        (0, 0) => Err(Error::Rejected("the change changes nothing")),
        (_, 0) => Ok(ChangeKind::AddMembers),
        (0, _) => Ok(ChangeKind::RevokeMembers),
        _ => Err(Error::Rejected("adds and revokes are separate changes")),
    }
}

/// Check a proposed change against every rule. Approvals are checked under
/// the **previous** roster's admin set.
pub fn check_change(
    prev: &Roster,
    next: &Roster,
    approvals: &[Approval],
    holder_proof: Option<&Approval>,
    mode: Mode,
) -> Result<ChangeKind> {
    let kind = classify(prev, next)?;
    let digest = next.digest()?;
    let n = admin::count_approvals(&prev.admins, &digest, approvals) as u64;
    match kind {
        ChangeKind::AddMembers => {
            if n < prev.threshold {
                return Err(Error::Rejected(
                    "adding a member needs threshold admin approvals",
                ));
            }
            if let Mode::Live {
                physical_confirm, ..
            } = mode
            {
                if prev.threshold < 2 && !physical_confirm {
                    return Err(Error::Rejected(
                        "single-approval add needs a physical confirm on the hub",
                    ));
                }
            }
        }
        ChangeKind::RevokeMembers => {
            if n < 1 {
                let removed: Vec<&MemberEntry> = prev
                    .members
                    .iter()
                    .filter(|m| next.member(&m.handle).is_none())
                    .collect();
                let by_holder = match (removed.as_slice(), holder_proof) {
                    ([only], Some(p)) => admin::verify_holder_proof(only, &digest, p),
                    _ => false,
                };
                if !by_holder {
                    return Err(Error::Rejected(
                        "revoking needs one admin approval or the member's own credential",
                    ));
                }
            }
        }
        ChangeKind::AdminSet => {
            if n < prev.threshold {
                return Err(Error::Rejected(
                    "an admin-set change needs threshold approvals of the current admins",
                ));
            }
            if let Mode::Live {
                now_unix,
                proposed_at,
                ..
            } = mode
            {
                let ready = proposed_at
                    .checked_add(ADMIN_CHANGE_PENDING_SECS)
                    .is_some_and(|t| now_unix >= t);
                if !ready {
                    return Err(Error::Rejected(
                        "an admin-set change must be pending for 24 hours",
                    ));
                }
            }
        }
    }
    Ok(kind)
}

/// Check a genesis roster: epoch 1, `prev` zeros, no members, approvals from
/// `threshold` of its own admins, and (live) a physical confirm on the hub.
pub fn check_genesis(next: &Roster, approvals: &[Approval], mode: Mode) -> Result<()> {
    next.validate()?;
    if next.epoch != 1 || next.prev != [0u8; 32] || !next.members.is_empty() {
        return Err(Error::Rejected(
            "genesis is epoch 1, prev zeros, no members",
        ));
    }
    let n = admin::count_approvals(&next.admins, &next.digest()?, approvals) as u64;
    if n < next.threshold {
        return Err(Error::Rejected(
            "genesis needs threshold approvals of its own admins",
        ));
    }
    if let Mode::Live {
        physical_confirm, ..
    } = mode
    {
        if !physical_confirm {
            return Err(Error::Rejected(
                "genesis needs a physical confirm on the hub",
            ));
        }
    }
    Ok(())
}

/// Re-verify a whole accepted chain, genesis first: every hub signature,
/// every link and every approval rule. Returns the last roster.
pub fn verify_chain<'a>(
    chain: &'a [SignedRoster],
    hub_key: &ed25519_dalek::VerifyingKey,
) -> Result<&'a Roster> {
    let first = chain.first().ok_or(Error::Invalid("empty roster chain"))?;
    for s in chain {
        admin::hub_verify(hub_key, &s.roster.digest()?, &s.hub_sig)?;
    }
    check_genesis(&first.roster, &first.approvals, Mode::Replay)?;
    for w in chain.windows(2) {
        check_change(
            &w[0].roster,
            &w[1].roster,
            &w[1].approvals,
            w[1].holder_proof.as_ref(),
            Mode::Replay,
        )?;
    }
    Ok(&chain[chain.len() - 1].roster)
}

/// The highest roster the checker has ever accepted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HighWater {
    /// Its epoch.
    pub epoch: u64,
    /// Its digest.
    pub digest: [u8; 32],
}

/// Refuse a roster below the high-water epoch, or a different roster at the
/// same epoch (a fork). `external_epoch` is the highest epoch any paired
/// satellite reports (satellites are not built in v0; pass `None`).
pub fn check_not_rollback(
    high: Option<HighWater>,
    external_epoch: Option<u64>,
    roster: &Roster,
) -> Result<()> {
    if let Some(e) = external_epoch {
        if roster.epoch < e {
            return Err(Error::Rejected(
                "roster epoch is below what a satellite reports",
            ));
        }
    }
    if let Some(h) = high {
        if roster.epoch < h.epoch {
            return Err(Error::Rejected(
                "roster epoch is below the highest accepted epoch",
            ));
        }
        if roster.epoch == h.epoch && roster.digest()? != h.digest {
            return Err(Error::Rejected(
                "a different roster at the highest accepted epoch",
            ));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::admin::{sign_cli, sign_fido2_admin, sign_holder_es256, sign_holder_fido2};
    use crate::roster::tests::{ed_admin, fido2, phone};
    use crate::roster::CredentialClass;
    use crate::verify::{FLAG_UP, FLAG_UV};

    const NOW: u64 = 1_800_000_000;

    fn ed(seed: u8) -> ed25519_dalek::SigningKey {
        ed25519_dalek::SigningKey::from_bytes(&[seed; 32])
    }

    fn live(confirm: bool) -> Mode {
        Mode::Live {
            now_unix: NOW,
            proposed_at: NOW,
            physical_confirm: confirm,
        }
    }

    fn approve(r: &Roster, seeds: &[u8]) -> Vec<Approval> {
        let d = r.digest().unwrap();
        seeds
            .iter()
            .map(|s| sign_cli(&ed(*s), &[*s; 16], &d))
            .collect()
    }

    /// Three admins (handles/seeds 1, 2, 3), threshold 2, one phone member.
    fn three_admins() -> Roster {
        let g = Roster::genesis(
            [0xaa; 16],
            vec![ed_admin(1, 1), ed_admin(2, 2), ed_admin(3, 3)],
            2,
        )
        .unwrap();
        let mut r = g.successor().unwrap();
        r.members.push(phone(0x10, 0x31, CredentialClass::Intent));
        r.sort();
        r
    }

    fn add_member(prev: &Roster, h: u8, seed: u8) -> Roster {
        let mut n = prev.successor().unwrap();
        n.members.push(phone(h, seed, CredentialClass::Possession));
        n.sort();
        n
    }

    #[test]
    fn genesis_rules() {
        let g = Roster::genesis([0xaa; 16], vec![ed_admin(1, 1)], 1).unwrap();
        let a = approve(&g, &[1]);
        assert!(check_genesis(&g, &a, live(true)).is_ok());
        assert!(
            check_genesis(&g, &a, live(false)).is_err(),
            "genesis without confirm"
        );
        assert!(
            check_genesis(&g, &[], live(true)).is_err(),
            "genesis without approval"
        );
        assert!(check_genesis(&g, &approve(&g, &[9]), live(true)).is_err());
    }

    #[test]
    fn add_needs_threshold() {
        let r = three_admins();
        let n = add_member(&r, 0x11, 0x41);
        assert_eq!(classify(&r, &n).unwrap(), ChangeKind::AddMembers);
        assert!(check_change(&r, &n, &approve(&n, &[1]), None, live(false)).is_err());
        assert_eq!(
            check_change(&r, &n, &approve(&n, &[1, 2]), None, live(false)).unwrap(),
            ChangeKind::AddMembers
        );
        // The same admin twice is one approval.
        assert!(check_change(&r, &n, &approve(&n, &[1, 1, 1]), None, live(false)).is_err());
        // Approvals over a different roster do not carry over.
        let other = add_member(&r, 0x12, 0x42);
        assert!(check_change(&r, &n, &approve(&other, &[1, 2]), None, live(false)).is_err());
    }

    #[test]
    fn single_admin_add_needs_physical_confirm() {
        let g = Roster::genesis([0xaa; 16], vec![ed_admin(1, 1)], 1).unwrap();
        let n = add_member(&g, 0x11, 0x41);
        let a = approve(&n, &[1]);
        assert!(check_change(&g, &n, &a, None, live(false)).is_err());
        assert!(check_change(&g, &n, &a, None, live(true)).is_ok());
    }

    #[test]
    fn revoke_by_any_one_admin_or_by_holder() {
        let r = three_admins();
        let mut n = r.successor().unwrap();
        n.members.clear();
        assert_eq!(classify(&r, &n).unwrap(), ChangeKind::RevokeMembers);
        assert!(check_change(&r, &n, &[], None, live(false)).is_err());
        assert!(check_change(&r, &n, &approve(&n, &[3]), None, live(false)).is_ok());
        // The member's own credential (p256 seed 0x31) revokes it.
        let mk = p256::ecdsa::SigningKey::from_slice(&[0x31; 32]).unwrap();
        let proof = sign_holder_es256(&mk, &[0x10; 16], &n.digest().unwrap());
        assert!(check_change(&r, &n, &[], Some(&proof), live(false)).is_ok());
        // Someone else's credential does not.
        let other = p256::ecdsa::SigningKey::from_slice(&[0x32; 32]).unwrap();
        let bad = sign_holder_es256(&other, &[0x10; 16], &n.digest().unwrap());
        assert!(check_change(&r, &n, &[], Some(&bad), live(false)).is_err());
    }

    #[test]
    fn fido2_member_revokes_itself_with_an_up_assertion() {
        // Spec §5.2, the "member revoking itself, fido2-es256" row.
        const RP: &str = "hk-member.example";
        let mut r = three_admins();
        r.members
            .push(fido2(0x12, 0x33, CredentialClass::Intent, RP));
        r.sort();
        let mut n = r.successor().unwrap();
        n.members.retain(|m| m.handle != [0x12; 16]);
        assert_eq!(classify(&r, &n).unwrap(), ChangeKind::RevokeMembers);
        let d = n.digest().unwrap();
        let fk = p256::ecdsa::SigningKey::from_slice(&[0x33; 32]).unwrap();
        let up = sign_holder_fido2(&fk, &[0x12; 16], RP, FLAG_UP, &d);
        assert_eq!(
            check_change(&r, &n, &[], Some(&up), live(false)).unwrap(),
            ChangeKind::RevokeMembers
        );
        // Without UP, under another rp, over the admin domain, or from the
        // phone member's key: refused.
        let phone_key = p256::ecdsa::SigningKey::from_slice(&[0x31; 32]).unwrap();
        for bad in [
            sign_holder_fido2(&fk, &[0x12; 16], RP, 0, &d),
            sign_holder_fido2(&fk, &[0x12; 16], "other.example", FLAG_UP, &d),
            sign_fido2_admin(&fk, &[0x12; 16], RP, FLAG_UP | FLAG_UV, &d),
            sign_holder_fido2(&phone_key, &[0x12; 16], RP, FLAG_UP, &d),
        ] {
            assert!(check_change(&r, &n, &[], Some(&bad), live(false)).is_err());
        }
        // The assertion revokes its own member only, not the phone.
        let mut other = r.successor().unwrap();
        other.members.retain(|m| m.handle != [0x10; 16]);
        let proof = sign_holder_fido2(&fk, &[0x12; 16], RP, FLAG_UP, &other.digest().unwrap());
        assert!(check_change(&r, &other, &[], Some(&proof), live(false)).is_err());
    }

    #[test]
    fn holder_proof_cannot_revoke_someone_else_or_two() {
        let mut r = three_admins();
        r.members.push(phone(0x11, 0x41, CredentialClass::Intent));
        r.sort();
        let mut n = r.successor().unwrap();
        n.members.retain(|m| m.handle != [0x11; 16]);
        // Member 0x10 tries to revoke member 0x11 with its own credential.
        let mk = p256::ecdsa::SigningKey::from_slice(&[0x31; 32]).unwrap();
        let proof = sign_holder_es256(&mk, &[0x10; 16], &n.digest().unwrap());
        assert!(check_change(&r, &n, &[], Some(&proof), live(false)).is_err());
        // A holder proof never covers a two-member revoke.
        let mut both = r.successor().unwrap();
        both.members.clear();
        let proof = sign_holder_es256(&mk, &[0x10; 16], &both.digest().unwrap());
        assert!(check_change(&r, &both, &[], Some(&proof), live(false)).is_err());
    }

    #[test]
    fn admin_takeover_through_the_revoke_path_is_refused() {
        // Admin 1 alone tries to remove admins 2 and 3 (the "cheap revoke"
        // takeover). It is an admin-set change, so it needs threshold
        // approvals and 24 hours, whatever it calls itself.
        let r = three_admins();
        let mut n = r.successor().unwrap();
        n.admins.retain(|a| a.handle == [1; 16]);
        n.threshold = 1;
        assert_eq!(classify(&r, &n).unwrap(), ChangeKind::AdminSet);
        let mode = Mode::Live {
            now_unix: NOW + ADMIN_CHANGE_PENDING_SECS * 10,
            proposed_at: NOW,
            physical_confirm: true,
        };
        assert!(check_change(&r, &n, &approve(&n, &[1]), None, mode).is_err());
        // Lowering the threshold alone is the same kind of change.
        let mut t = r.successor().unwrap();
        t.threshold = 1;
        assert_eq!(classify(&r, &t).unwrap(), ChangeKind::AdminSet);
        assert!(check_change(&r, &t, &approve(&t, &[1]), None, mode).is_err());
        // An admin change smuggled in with a member revoke is refused outright.
        let mut mixed = r.successor().unwrap();
        mixed.members.clear();
        mixed.admins.retain(|a| a.handle == [1; 16]);
        mixed.threshold = 1;
        assert!(classify(&r, &mixed).is_err());
    }

    #[test]
    fn admin_set_change_needs_24h_pending() {
        let r = three_admins();
        let mut n = r.successor().unwrap();
        n.admins.push(ed_admin(4, 4));
        n.sort();
        let a = approve(&n, &[1, 2]);
        let at = |now| Mode::Live {
            now_unix: now,
            proposed_at: NOW,
            physical_confirm: false,
        };
        assert!(check_change(&r, &n, &a, None, at(NOW)).is_err());
        assert!(check_change(&r, &n, &a, None, at(NOW + ADMIN_CHANGE_PENDING_SECS - 1)).is_err());
        assert!(check_change(&r, &n, &a, None, at(NOW + ADMIN_CHANGE_PENDING_SECS)).is_ok());
        // Approvals from the NEW admin do not count toward its own admission.
        let a = approve(&n, &[1, 4]);
        assert!(check_change(&r, &n, &a, None, at(NOW + ADMIN_CHANGE_PENDING_SECS)).is_err());
    }

    #[test]
    fn chain_links_are_checked() {
        let r = three_admins();
        let mut n = add_member(&r, 0x11, 0x41);
        n.epoch += 1;
        assert!(classify(&r, &n).is_err(), "epoch skip accepted");
        let mut n = add_member(&r, 0x11, 0x41);
        n.prev = [0; 32];
        assert!(classify(&r, &n).is_err(), "broken prev accepted");
        let mut n = add_member(&r, 0x11, 0x41);
        n.household = [0xbb; 16];
        assert!(classify(&r, &n).is_err(), "household change accepted");
        let n = r.successor().unwrap();
        assert!(classify(&r, &n).is_err(), "no-op accepted");
        let mut n = r.successor().unwrap();
        n.members[0].class = CredentialClass::Possession;
        assert!(classify(&r, &n).is_err(), "in-place edit accepted");
        let mut n = r.successor().unwrap();
        n.members.clear();
        n.members.push(phone(0x11, 0x41, CredentialClass::Intent));
        assert!(classify(&r, &n).is_err(), "add+revoke accepted");
    }

    #[test]
    fn rollback_and_fork_refused() {
        let r = three_admins();
        let n = add_member(&r, 0x11, 0x41);
        let hw = HighWater {
            epoch: n.epoch,
            digest: n.digest().unwrap(),
        };
        assert!(check_not_rollback(Some(hw), None, &n).is_ok());
        assert!(
            check_not_rollback(Some(hw), None, &r).is_err(),
            "rollback accepted"
        );
        let fork = add_member(&r, 0x12, 0x42);
        assert!(
            check_not_rollback(Some(hw), None, &fork).is_err(),
            "fork accepted"
        );
        assert!(check_not_rollback(None, Some(n.epoch + 1), &n).is_err());
        let next = add_member(&n, 0x12, 0x42);
        assert!(check_not_rollback(Some(hw), None, &next).is_ok());
    }

    #[test]
    fn chain_replay_verifies_and_catches_a_forged_link() {
        let hub = ed(0x77);
        let sign = |r: &Roster, a: Vec<Approval>| SignedRoster {
            roster: r.clone(),
            approvals: a,
            holder_proof: None,
            hub_sig: admin::hub_sign(&hub, &r.digest().unwrap()),
        };
        let g = Roster::genesis([0xaa; 16], vec![ed_admin(1, 1), ed_admin(2, 2)], 2).unwrap();
        let s0 = sign(&g, approve(&g, &[1, 2]));
        let r1 = add_member(&g, 0x11, 0x41);
        let s1 = sign(&r1, approve(&r1, &[1, 2]));
        let chain = vec![s0.clone(), s1.clone()];
        assert_eq!(verify_chain(&chain, &hub.verifying_key()).unwrap(), &r1);
        // JSON round trip.
        let back = SignedRoster::from_value(&s1.to_value()).unwrap();
        assert_eq!(back, s1);
        // A link admitted with one approval fails replay even though the hub
        // signed it.
        let s1_bad = sign(&r1, approve(&r1, &[1]));
        assert!(verify_chain(&[s0.clone(), s1_bad], &hub.verifying_key()).is_err());
        // A different hub key fails.
        assert!(verify_chain(&chain, &ed(0x78).verifying_key()).is_err());
    }
}
