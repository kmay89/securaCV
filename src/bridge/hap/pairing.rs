//! Pair Setup and Pair Verify — how a controller earns the right to read
//! this accessory, and how it proves on every reconnection that it still has
//! it.
//!
//! # The two exchanges, and why there are two
//!
//! **Pair Setup** happens once, when a human types the eight-digit setup
//! code. It runs SRP-6a over the 3072-bit RFC 5054 group with SHA-512, which
//! turns that short code into a strong shared secret without ever putting it
//! on the wire — then the two sides swap long-term Ed25519 identities under
//! a key derived from it. The code is a *password*, and SRP is what makes a
//! password that short safe to use.
//!
//! **Pair Verify** happens on every connection afterward. It is a fresh
//! X25519 exchange, signed by both long-term keys, so each session gets keys
//! that outlive nothing — a recorded session cannot be replayed, and a
//! compromise of today's session keys does not read yesterday's traffic.
//!
//! # What is deliberately not here
//!
//! No pairing state is written to disk by this module. It hands back a
//! [`Pairing`] and lets the caller decide where that lives, because on a
//! Canary the answer is "NVS, and only if flash encryption is on" while on a
//! hub it is a file — and a long-term pairing key is exactly the kind of
//! secret the hardware-root-of-trust tiering exists to govern.
//!
//! # Honest limits
//!
//! The transcript below follows the HAP specification step by step, and the
//! tests drive it with an independently written controller. That proves the
//! state machine is self-consistent and spec-shaped; it does **not** prove
//! byte-compatibility with Apple's implementation, which only pairing
//! against a real controller can. Where that distinction matters it is
//! called out rather than papered over.

use ed25519_dalek::{Signer, SigningKey, Verifier, VerifyingKey};
use rand::rngs::SysRng;
use rand::TryRng;
use sha2::Sha512;
use srp::{ClientG3072, ServerG3072};
use x25519_dalek::{PublicKey as XPublicKey, StaticSecret};
use zeroize::Zeroize;

use super::crypto::{self, kdf, nonce};
use super::tlv8::{error as tlv_error, ty, Tlv};

/// The SRP username HAP fixes for pair setup. Not a secret and not
/// configurable — the setup code is the only secret in this exchange.
const SRP_USERNAME: &[u8] = b"Pair-Setup";

/// How many controllers may pair with one accessory.
///
/// FR-4: the pairing table is the one structure a remote peer can grow, so
/// it is bounded. Sixteen is well past a household's controller count (each
/// Apple ID in a home adds one) and far below anything that would matter for
/// memory.
pub const MAX_PAIRINGS: usize = 16;

/// How many failed setup attempts before the accessory stops answering.
///
/// HAP mandates this: without it, an eight-digit code is brute-forceable
/// over the network in hours. Once tripped it stays tripped until the
/// accessory is restarted and re-provisioned.
pub const MAX_SETUP_ATTEMPTS: u8 = 100;

/// A controller this accessory has paired with.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Pairing {
    /// The controller's pairing identifier (a UUID string, Apple's choice).
    pub id: String,
    /// Its long-term Ed25519 public key.
    pub ltpk: [u8; 32],
    /// Whether it may add and remove other pairings.
    pub admin: bool,
}

/// The set of controllers allowed to talk to this accessory.
#[derive(Clone, Debug, Default)]
pub struct PairingStore {
    pairings: Vec<Pairing>,
}

impl PairingStore {
    /// An accessory nobody has paired with yet.
    pub fn new() -> Self {
        PairingStore::default()
    }

    /// Rebuild a store from persisted pairings, dropping anything past the
    /// bound rather than growing beyond it.
    pub fn from_pairings(pairings: impl IntoIterator<Item = Pairing>) -> Self {
        PairingStore {
            pairings: pairings.into_iter().take(MAX_PAIRINGS).collect(),
        }
    }

    /// Add or replace a pairing. Re-pairing an existing controller updates
    /// its key rather than adding a duplicate, which is what HAP's
    /// AddPairing semantics require.
    pub fn add(&mut self, pairing: Pairing) -> Result<(), PairError> {
        if let Some(existing) = self.pairings.iter_mut().find(|p| p.id == pairing.id) {
            *existing = pairing;
            return Ok(());
        }
        if self.pairings.len() >= MAX_PAIRINGS {
            return Err(PairError::MaxPeers);
        }
        self.pairings.push(pairing);
        Ok(())
    }

    /// Look up a controller by pairing id.
    pub fn get(&self, id: &str) -> Option<&Pairing> {
        self.pairings.iter().find(|p| p.id == id)
    }

    /// Forget a controller. Returns whether one was there.
    pub fn remove(&mut self, id: &str) -> bool {
        let before = self.pairings.len();
        self.pairings.retain(|p| p.id != id);
        self.pairings.len() != before
    }

    /// Whether any controller is paired. An accessory with none is
    /// discoverable and accepts pair setup; one with any is not.
    pub fn is_empty(&self) -> bool {
        self.pairings.is_empty()
    }

    /// How many controllers are paired.
    pub fn len(&self) -> usize {
        self.pairings.len()
    }

    /// Every pairing, for the ListPairings response.
    pub fn list(&self) -> &[Pairing] {
        &self.pairings
    }
}

/// Why a pairing step failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum PairError {
    /// Wrong setup code, bad proof, or a bad signature — deliberately one
    /// variant, so a controller cannot tell which.
    Authentication,
    /// A message arrived out of order.
    UnexpectedState,
    /// The pairing table is full.
    MaxPeers,
    /// Too many failed attempts; the accessory has stopped answering.
    MaxTries,
    /// The accessory is already paired, or the request is not available in
    /// its current state.
    Unavailable,
    /// The request was malformed.
    Unknown,
}

impl PairError {
    /// The `kTLVError_*` code for this failure.
    fn tlv_code(self) -> u8 {
        match self {
            PairError::Authentication => tlv_error::AUTHENTICATION,
            PairError::UnexpectedState | PairError::Unknown => tlv_error::UNKNOWN,
            PairError::MaxPeers => tlv_error::MAX_PEERS,
            PairError::MaxTries => tlv_error::MAX_TRIES,
            PairError::Unavailable => tlv_error::UNAVAILABLE,
        }
    }
}

impl std::fmt::Display for PairError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let s = match self {
            PairError::Authentication => "pairing authentication failed",
            PairError::UnexpectedState => "pairing message arrived out of order",
            PairError::MaxPeers => "pairing table is full",
            PairError::MaxTries => "too many failed pairing attempts",
            PairError::Unavailable => "pairing is not available in this state",
            PairError::Unknown => "malformed pairing request",
        };
        write!(f, "{s}")
    }
}

impl std::error::Error for PairError {}

/// This accessory's own long-term identity.
#[derive(Clone)]
pub struct AccessoryIdentity {
    pairing_id: String,
    signing: SigningKey,
}

impl std::fmt::Debug for AccessoryIdentity {
    /// Never renders the signing key. An identity that logs its own secret
    /// is a secret in the log forever.
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("AccessoryIdentity")
            .field("pairing_id", &self.pairing_id)
            .field("signing", &"<redacted>")
            .finish()
    }
}

impl AccessoryIdentity {
    /// Build an identity from a stored 32-byte seed. The seed *is* the
    /// accessory's identity: restore it and paired controllers reconnect;
    /// lose it and every controller must pair again.
    pub fn from_seed(pairing_id: impl Into<String>, seed: [u8; 32]) -> Self {
        AccessoryIdentity {
            pairing_id: pairing_id.into(),
            signing: SigningKey::from_bytes(&seed),
        }
    }

    /// Generate a fresh identity.
    pub fn generate(pairing_id: impl Into<String>) -> Result<Self, PairError> {
        let mut seed = [0u8; 32];
        SysRng
            .try_fill_bytes(&mut seed)
            .map_err(|_| PairError::Unknown)?;
        Ok(AccessoryIdentity::from_seed(pairing_id, seed))
    }

    /// The accessory's pairing identifier, as sent to controllers.
    pub fn pairing_id(&self) -> &str {
        &self.pairing_id
    }

    /// The accessory's long-term Ed25519 public key.
    pub fn ltpk(&self) -> [u8; 32] {
        self.signing.verifying_key().to_bytes()
    }

    fn sign(&self, msg: &[u8]) -> [u8; 64] {
        self.signing.sign(msg).to_bytes()
    }
}

/// Verify an Ed25519 signature, treating every failure as one error.
fn verify_signature(ltpk: &[u8; 32], msg: &[u8], sig: &[u8]) -> Result<(), PairError> {
    let key = VerifyingKey::from_bytes(ltpk).map_err(|_| PairError::Authentication)?;
    let sig: [u8; 64] = sig.try_into().map_err(|_| PairError::Authentication)?;
    key.verify(msg, &ed25519_dalek::Signature::from_bytes(&sig))
        .map_err(|_| PairError::Authentication)
}

/// The Pair Setup exchange (M1–M6), one per connection attempt.
pub struct PairSetup {
    setup_code: String,
    state: u8,
    salt: [u8; 16],
    b: [u8; 48],
    verifier: Vec<u8>,
    srp_key: Option<Vec<u8>>,
    attempts: u8,
}

impl std::fmt::Debug for PairSetup {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("PairSetup")
            .field("state", &self.state)
            .field("attempts", &self.attempts)
            .finish_non_exhaustive()
    }
}

impl PairSetup {
    /// Start a pair-setup exchange for an accessory whose setup code is
    /// `setup_code`, formatted `XXX-XX-XXX`.
    pub fn new(setup_code: impl Into<String>) -> Result<Self, PairError> {
        let setup_code = setup_code.into();
        let mut salt = [0u8; 16];
        let mut b = [0u8; 48];
        SysRng
            .try_fill_bytes(&mut salt)
            .map_err(|_| PairError::Unknown)?;
        SysRng
            .try_fill_bytes(&mut b)
            .map_err(|_| PairError::Unknown)?;

        // The SRP verifier is derived from the setup code exactly as a
        // client would derive it, which is what lets the accessory prove
        // knowledge of the code without storing anything the code could be
        // recovered from cheaply.
        let client = ClientG3072::<Sha512>::new();
        let verifier = client.compute_verifier(SRP_USERNAME, setup_code.as_bytes(), &salt);

        Ok(PairSetup {
            setup_code,
            state: 0,
            salt,
            b,
            verifier,
            srp_key: None,
            attempts: 0,
        })
    }

    /// Handle one pair-setup message, returning the TLV8 response body.
    ///
    /// Always returns a well-formed response: a failure becomes a TLV error
    /// for the controller to render, never a dropped connection, because a
    /// silent drop is indistinguishable from a network fault and produces
    /// the worst pairing UX there is.
    pub fn handle(
        &mut self,
        body: &[u8],
        identity: &AccessoryIdentity,
        store: &mut PairingStore,
    ) -> Vec<u8> {
        let request = match Tlv::decode(body) {
            Ok(t) => t,
            Err(_) => return Tlv::error_response(2, tlv_error::UNKNOWN),
        };
        let state = request.get_u8(ty::STATE).unwrap_or(0);
        let reply_state = state.saturating_add(1);

        match self.step(&request, state, identity, store) {
            Ok(tlv) => tlv.encode(),
            Err(e) => {
                if e == PairError::Authentication {
                    self.attempts = self.attempts.saturating_add(1);
                }
                // Reset the exchange on any failure: a controller must start
                // from M1 rather than retry a step in place, so a partially
                // completed transcript can never be resumed with different
                // material.
                self.state = 0;
                self.srp_key = None;
                Tlv::error_response(reply_state, e.tlv_code())
            }
        }
    }

    fn step(
        &mut self,
        request: &Tlv,
        state: u8,
        identity: &AccessoryIdentity,
        store: &mut PairingStore,
    ) -> Result<Tlv, PairError> {
        if self.attempts >= MAX_SETUP_ATTEMPTS {
            return Err(PairError::MaxTries);
        }
        match state {
            1 => self.m1_to_m2(store),
            3 => self.m3_to_m4(request),
            5 => self.m5_to_m6(request, identity, store),
            _ => Err(PairError::UnexpectedState),
        }
    }

    /// M1 → M2: hand back the salt and the SRP public value.
    fn m1_to_m2(&mut self, store: &PairingStore) -> Result<Tlv, PairError> {
        // An accessory that is already paired must refuse to set up again;
        // otherwise anyone within mDNS range who learns the setup code could
        // add themselves to a home that is already someone's.
        if !store.is_empty() {
            return Err(PairError::Unavailable);
        }
        let server = ServerG3072::<Sha512>::new();
        let b_pub = server.compute_public_ephemeral(&self.b, &self.verifier);
        self.state = 2;

        let mut reply = Tlv::new();
        reply
            .push_u8(ty::STATE, 2)
            .push(ty::PUBLIC_KEY, b_pub)
            .push(ty::SALT, self.salt.to_vec());
        Ok(reply)
    }

    /// M3 → M4: check the controller's SRP proof, return ours.
    fn m3_to_m4(&mut self, request: &Tlv) -> Result<Tlv, PairError> {
        if self.state != 2 {
            return Err(PairError::UnexpectedState);
        }
        let a_pub = request.get(ty::PUBLIC_KEY).ok_or(PairError::Unknown)?;
        let proof = request.get(ty::PROOF).ok_or(PairError::Unknown)?;

        let server = ServerG3072::<Sha512>::new();
        let verifier = server
            .process_reply(SRP_USERNAME, &self.salt, &self.b, &self.verifier, a_pub)
            .map_err(|_| PairError::Authentication)?;
        // `verify_client` checks the controller's M1 and hands back the SRP
        // *session key* K = H(S) on success. That — not `verifier.key()`,
        // which is the raw premaster secret S — is what HAP feeds every
        // subsequent HKDF. Using S here yields a self-consistent
        // implementation that no Apple controller can pair with.
        let srp_key = verifier
            .verify_client(proof)
            .map_err(|_| PairError::Authentication)?
            .to_vec();
        // The accessory's own proof M2 is what the controller checks next.
        let server_proof = verifier.proof().to_vec();

        self.srp_key = Some(srp_key);
        self.state = 4;

        let mut reply = Tlv::new();
        reply.push_u8(ty::STATE, 4).push(ty::PROOF, server_proof);
        Ok(reply)
    }

    /// M5 → M6: swap long-term identities under a key derived from the SRP
    /// secret, and record the pairing.
    fn m5_to_m6(
        &mut self,
        request: &Tlv,
        identity: &AccessoryIdentity,
        store: &mut PairingStore,
    ) -> Result<Tlv, PairError> {
        if self.state != 4 {
            return Err(PairError::UnexpectedState);
        }
        let srp_key = self.srp_key.as_deref().ok_or(PairError::UnexpectedState)?;
        let sealed = request.get(ty::ENCRYPTED_DATA).ok_or(PairError::Unknown)?;

        let session_key = crypto::hkdf_sha512(
            srp_key,
            kdf::PAIR_SETUP_ENCRYPT_SALT,
            kdf::PAIR_SETUP_ENCRYPT_INFO,
        );
        let plaintext = crypto::open(&session_key, nonce::PS_MSG05, b"", sealed)
            .map_err(|_| PairError::Authentication)?;
        let sub = Tlv::decode(&plaintext).map_err(|_| PairError::Unknown)?;

        let controller_id = sub.get(ty::IDENTIFIER).ok_or(PairError::Unknown)?;
        let controller_ltpk: [u8; 32] = sub
            .get(ty::PUBLIC_KEY)
            .ok_or(PairError::Unknown)?
            .try_into()
            .map_err(|_| PairError::Unknown)?;
        let controller_sig = sub.get(ty::SIGNATURE).ok_or(PairError::Unknown)?;

        // The controller signs (its derived key material || its id || its
        // public key). Checking it here is what stops a man in the middle
        // who guessed the setup code from substituting their own identity.
        let controller_x = crypto::hkdf_sha512(
            srp_key,
            kdf::PAIR_SETUP_CONTROLLER_SIGN_SALT,
            kdf::PAIR_SETUP_CONTROLLER_SIGN_INFO,
        );
        let mut controller_info = Vec::new();
        controller_info.extend_from_slice(&controller_x);
        controller_info.extend_from_slice(controller_id);
        controller_info.extend_from_slice(&controller_ltpk);
        verify_signature(&controller_ltpk, &controller_info, controller_sig)?;

        let controller_id =
            String::from_utf8(controller_id.to_vec()).map_err(|_| PairError::Unknown)?;

        // The mirror image: we sign our own identity so the controller can
        // pin us on every later Pair Verify.
        let accessory_x = crypto::hkdf_sha512(
            srp_key,
            kdf::PAIR_SETUP_ACCESSORY_SIGN_SALT,
            kdf::PAIR_SETUP_ACCESSORY_SIGN_INFO,
        );
        let accessory_ltpk = identity.ltpk();
        let mut accessory_info = Vec::new();
        accessory_info.extend_from_slice(&accessory_x);
        accessory_info.extend_from_slice(identity.pairing_id().as_bytes());
        accessory_info.extend_from_slice(&accessory_ltpk);
        let accessory_sig = identity.sign(&accessory_info);

        // Record the pairing only once everything has checked out — a
        // half-failed M5 must leave no trace.
        store.add(Pairing {
            id: controller_id,
            ltpk: controller_ltpk,
            admin: true,
        })?;

        let mut sub_reply = Tlv::new();
        sub_reply
            .push(ty::IDENTIFIER, identity.pairing_id().as_bytes().to_vec())
            .push(ty::PUBLIC_KEY, accessory_ltpk.to_vec())
            .push(ty::SIGNATURE, accessory_sig.to_vec());
        let sealed_reply = crypto::seal(&session_key, nonce::PS_MSG06, b"", &sub_reply.encode());

        self.state = 6;
        let mut reply = Tlv::new();
        reply
            .push_u8(ty::STATE, 6)
            .push(ty::ENCRYPTED_DATA, sealed_reply);
        Ok(reply)
    }

    /// The setup code this exchange expects, for rendering the pairing QR.
    pub fn setup_code(&self) -> &str {
        &self.setup_code
    }

    /// Whether the exchange has completed.
    pub fn is_complete(&self) -> bool {
        self.state == 6
    }
}

/// The session keys a completed Pair Verify yields.
#[derive(Clone, Copy)]
pub struct SessionKeys {
    /// Accessory → controller.
    pub read: [u8; 32],
    /// Controller → accessory.
    pub write: [u8; 32],
}

impl std::fmt::Debug for SessionKeys {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SessionKeys").finish_non_exhaustive()
    }
}

/// The Pair Verify exchange (M1–M4), one per connection.
pub struct PairVerify {
    state: u8,
    shared_secret: Option<[u8; 32]>,
    session_key: Option<[u8; 32]>,
    accessory_pk: Option<[u8; 32]>,
    controller_pk: Option<[u8; 32]>,
    /// Set once M4 succeeds — the pairing id of the controller on the other
    /// end, which is what later authorizes a pairing-list change.
    verified: Option<String>,
}

impl std::fmt::Debug for PairVerify {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("PairVerify")
            .field("state", &self.state)
            .field("verified", &self.verified)
            .finish_non_exhaustive()
    }
}

impl Default for PairVerify {
    fn default() -> Self {
        PairVerify::new()
    }
}

impl PairVerify {
    /// A fresh exchange.
    pub fn new() -> Self {
        PairVerify {
            state: 0,
            shared_secret: None,
            session_key: None,
            accessory_pk: None,
            controller_pk: None,
            verified: None,
        }
    }

    /// Handle one pair-verify message, returning the TLV8 response body.
    pub fn handle(
        &mut self,
        body: &[u8],
        identity: &AccessoryIdentity,
        store: &PairingStore,
    ) -> Vec<u8> {
        let request = match Tlv::decode(body) {
            Ok(t) => t,
            Err(_) => return Tlv::error_response(2, tlv_error::UNKNOWN),
        };
        let state = request.get_u8(ty::STATE).unwrap_or(0);
        let reply_state = state.saturating_add(1);

        let result = match state {
            1 => self.m1_to_m2(&request, identity),
            3 => self.m3_to_m4(&request, store),
            _ => Err(PairError::UnexpectedState),
        };

        match result {
            Ok(tlv) => tlv.encode(),
            Err(e) => {
                *self = PairVerify::new();
                Tlv::error_response(reply_state, e.tlv_code())
            }
        }
    }

    fn m1_to_m2(&mut self, request: &Tlv, identity: &AccessoryIdentity) -> Result<Tlv, PairError> {
        let controller_pk: [u8; 32] = request
            .get(ty::PUBLIC_KEY)
            .ok_or(PairError::Unknown)?
            .try_into()
            .map_err(|_| PairError::Unknown)?;

        // A fresh key per connection is what makes the session
        // forward-secret. `StaticSecret` is the type that lets us seed from
        // the repo's fallible OS RNG, but nothing here is static: it is built
        // per connection, used once, and dropped at the end of this function,
        // so it cannot be recovered from the accessory afterward.
        let mut scalar = [0u8; 32];
        SysRng
            .try_fill_bytes(&mut scalar)
            .map_err(|_| PairError::Unknown)?;
        let secret = StaticSecret::from(scalar);
        scalar.zeroize();
        let accessory_pk = XPublicKey::from(&secret).to_bytes();
        let shared = secret.diffie_hellman(&XPublicKey::from(controller_pk));
        // Reject a low-order controller key, which would force the shared
        // secret to a fixed value that the peer knows in advance — the
        // session would encrypt under a key an eavesdropper could predict.
        if !shared.was_contributory() {
            return Err(PairError::Authentication);
        }
        let shared = shared.to_bytes();

        let mut accessory_info = Vec::new();
        accessory_info.extend_from_slice(&accessory_pk);
        accessory_info.extend_from_slice(identity.pairing_id().as_bytes());
        accessory_info.extend_from_slice(&controller_pk);
        let signature = identity.sign(&accessory_info);

        let session_key = crypto::hkdf_sha512(
            &shared,
            kdf::PAIR_VERIFY_ENCRYPT_SALT,
            kdf::PAIR_VERIFY_ENCRYPT_INFO,
        );

        let mut sub = Tlv::new();
        sub.push(ty::IDENTIFIER, identity.pairing_id().as_bytes().to_vec())
            .push(ty::SIGNATURE, signature.to_vec());
        let sealed = crypto::seal(&session_key, nonce::PV_MSG02, b"", &sub.encode());

        self.shared_secret = Some(shared);
        self.session_key = Some(session_key);
        self.accessory_pk = Some(accessory_pk);
        self.controller_pk = Some(controller_pk);
        self.state = 2;

        let mut reply = Tlv::new();
        reply
            .push_u8(ty::STATE, 2)
            .push(ty::PUBLIC_KEY, accessory_pk.to_vec())
            .push(ty::ENCRYPTED_DATA, sealed);
        Ok(reply)
    }

    fn m3_to_m4(&mut self, request: &Tlv, store: &PairingStore) -> Result<Tlv, PairError> {
        if self.state != 2 {
            return Err(PairError::UnexpectedState);
        }
        let session_key = self.session_key.ok_or(PairError::UnexpectedState)?;
        let accessory_pk = self.accessory_pk.ok_or(PairError::UnexpectedState)?;
        let controller_pk = self.controller_pk.ok_or(PairError::UnexpectedState)?;

        let sealed = request.get(ty::ENCRYPTED_DATA).ok_or(PairError::Unknown)?;
        let plaintext = crypto::open(&session_key, nonce::PV_MSG03, b"", sealed)
            .map_err(|_| PairError::Authentication)?;
        let sub = Tlv::decode(&plaintext).map_err(|_| PairError::Unknown)?;

        let controller_id = sub.get(ty::IDENTIFIER).ok_or(PairError::Unknown)?;
        let controller_id = std::str::from_utf8(controller_id).map_err(|_| PairError::Unknown)?;
        let signature = sub.get(ty::SIGNATURE).ok_or(PairError::Unknown)?;

        // An unknown controller fails exactly like a bad signature. Telling
        // the two apart would turn this into an oracle for which controllers
        // a home has paired.
        let pairing = store.get(controller_id).ok_or(PairError::Authentication)?;

        let mut controller_info = Vec::new();
        controller_info.extend_from_slice(&controller_pk);
        controller_info.extend_from_slice(controller_id.as_bytes());
        controller_info.extend_from_slice(&accessory_pk);
        verify_signature(&pairing.ltpk, &controller_info, signature)?;

        self.verified = Some(controller_id.to_string());
        self.state = 4;

        let mut reply = Tlv::new();
        reply.push_u8(ty::STATE, 4);
        Ok(reply)
    }

    /// The pairing id of the verified controller, once M4 has completed.
    pub fn verified_controller(&self) -> Option<&str> {
        self.verified.as_deref()
    }

    /// The session keys, once M4 has completed.
    ///
    /// Returns `None` before verification succeeds, so it is not possible to
    /// start encrypting to an unauthenticated peer by forgetting a check.
    pub fn session_keys(&self) -> Option<SessionKeys> {
        // Refuse to hand out keys until the peer has authenticated — the
        // check that stops "encrypt first, verify later".
        let _authenticated = self.verified.as_ref()?;
        let shared = self.shared_secret?;
        Some(SessionKeys {
            read: crypto::hkdf_sha512(&shared, kdf::CONTROL_SALT, kdf::CONTROL_READ_INFO),
            write: crypto::hkdf_sha512(&shared, kdf::CONTROL_SALT, kdf::CONTROL_WRITE_INFO),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::super::tlv8::method;
    use super::*;

    /// An independently written controller, following the HAP spec from the
    /// other side. Driving the accessory with this rather than with its own
    /// internals is what makes the round-trip tests meaningful.
    struct TestController {
        id: String,
        signing: SigningKey,
    }

    impl TestController {
        fn new(id: &str) -> Self {
            TestController {
                id: id.to_string(),
                signing: SigningKey::from_bytes(&[9u8; 32]),
            }
        }

        fn ltpk(&self) -> [u8; 32] {
            self.signing.verifying_key().to_bytes()
        }

        /// Run pair setup to completion against `setup`, returning the
        /// accessory's advertised identity.
        fn pair_setup(
            &self,
            setup: &mut PairSetup,
            identity: &AccessoryIdentity,
            store: &mut PairingStore,
            code: &str,
        ) -> Result<(String, [u8; 32]), Vec<u8>> {
            // M1
            let mut m1 = Tlv::new();
            m1.push_u8(ty::STATE, 1)
                .push_u8(ty::METHOD, method::PAIR_SETUP);
            let m2 = Tlv::decode(&setup.handle(&m1.encode(), identity, store)).expect("tlv");
            if m2.get(ty::ERROR).is_some() {
                return Err(m2.encode());
            }
            let b_pub = m2.get(ty::PUBLIC_KEY).expect("B").to_vec();
            let salt = m2.get(ty::SALT).expect("salt").to_vec();

            // M3: the controller's own SRP side.
            let a = [5u8; 64];
            let client = ClientG3072::<Sha512>::new();
            let a_pub = client.compute_public_ephemeral(&a);
            let cv = client
                .process_reply(&a, SRP_USERNAME, code.as_bytes(), &salt, &b_pub)
                .expect("client srp");
            let mut m3 = Tlv::new();
            m3.push_u8(ty::STATE, 3)
                .push(ty::PUBLIC_KEY, a_pub)
                .push(ty::PROOF, cv.proof().to_vec());
            let m4 = Tlv::decode(&setup.handle(&m3.encode(), identity, store)).expect("tlv");
            if m4.get(ty::ERROR).is_some() {
                return Err(m4.encode());
            }
            // Same rule on this side: the session key K, not the premaster.
            let srp_key = cv
                .verify_server(m4.get(ty::PROOF).expect("M2 proof"))
                .expect("server proof verifies")
                .to_vec();

            // M5
            let session_key = crypto::hkdf_sha512(
                &srp_key,
                kdf::PAIR_SETUP_ENCRYPT_SALT,
                kdf::PAIR_SETUP_ENCRYPT_INFO,
            );
            let x = crypto::hkdf_sha512(
                &srp_key,
                kdf::PAIR_SETUP_CONTROLLER_SIGN_SALT,
                kdf::PAIR_SETUP_CONTROLLER_SIGN_INFO,
            );
            let mut info = Vec::new();
            info.extend_from_slice(&x);
            info.extend_from_slice(self.id.as_bytes());
            info.extend_from_slice(&self.ltpk());
            let sig = self.signing.sign(&info).to_bytes();
            let mut sub = Tlv::new();
            sub.push(ty::IDENTIFIER, self.id.as_bytes().to_vec())
                .push(ty::PUBLIC_KEY, self.ltpk().to_vec())
                .push(ty::SIGNATURE, sig.to_vec());
            let sealed = crypto::seal(&session_key, nonce::PS_MSG05, b"", &sub.encode());
            let mut m5 = Tlv::new();
            m5.push_u8(ty::STATE, 5).push(ty::ENCRYPTED_DATA, sealed);
            let m6 = Tlv::decode(&setup.handle(&m5.encode(), identity, store)).expect("tlv");
            if m6.get(ty::ERROR).is_some() {
                return Err(m6.encode());
            }

            let opened = crypto::open(
                &session_key,
                nonce::PS_MSG06,
                b"",
                m6.get(ty::ENCRYPTED_DATA).expect("M6 data"),
            )
            .expect("M6 opens");
            let sub = Tlv::decode(&opened).expect("tlv");
            let acc_id =
                String::from_utf8(sub.get(ty::IDENTIFIER).expect("id").to_vec()).expect("utf8");
            let acc_ltpk: [u8; 32] = sub.get(ty::PUBLIC_KEY).expect("ltpk").try_into().unwrap();

            // The accessory's signature must check out, or a controller
            // would pin an identity it never authenticated.
            let acc_x = crypto::hkdf_sha512(
                &srp_key,
                kdf::PAIR_SETUP_ACCESSORY_SIGN_SALT,
                kdf::PAIR_SETUP_ACCESSORY_SIGN_INFO,
            );
            let mut acc_info = Vec::new();
            acc_info.extend_from_slice(&acc_x);
            acc_info.extend_from_slice(acc_id.as_bytes());
            acc_info.extend_from_slice(&acc_ltpk);
            verify_signature(&acc_ltpk, &acc_info, sub.get(ty::SIGNATURE).expect("sig"))
                .expect("accessory signature verifies");

            Ok((acc_id, acc_ltpk))
        }

        /// Run pair verify to completion, returning the session keys.
        fn pair_verify(
            &self,
            verify: &mut PairVerify,
            identity: &AccessoryIdentity,
            store: &PairingStore,
            accessory_ltpk: &[u8; 32],
        ) -> Result<SessionKeys, Vec<u8>> {
            let mut scalar = [0u8; 32];
            SysRng.try_fill_bytes(&mut scalar).expect("OS RNG");
            let secret = StaticSecret::from(scalar);
            let my_pk = XPublicKey::from(&secret).to_bytes();

            let mut m1 = Tlv::new();
            m1.push_u8(ty::STATE, 1)
                .push(ty::PUBLIC_KEY, my_pk.to_vec());
            let m2 = Tlv::decode(&verify.handle(&m1.encode(), identity, store)).expect("tlv");
            if m2.get(ty::ERROR).is_some() {
                return Err(m2.encode());
            }
            let acc_pk: [u8; 32] = m2.get(ty::PUBLIC_KEY).expect("pk").try_into().unwrap();
            let shared = secret.diffie_hellman(&XPublicKey::from(acc_pk)).to_bytes();
            let session_key = crypto::hkdf_sha512(
                &shared,
                kdf::PAIR_VERIFY_ENCRYPT_SALT,
                kdf::PAIR_VERIFY_ENCRYPT_INFO,
            );

            let opened = crypto::open(
                &session_key,
                nonce::PV_MSG02,
                b"",
                m2.get(ty::ENCRYPTED_DATA).expect("data"),
            )
            .expect("M2 opens");
            let sub = Tlv::decode(&opened).expect("tlv");
            let acc_id = sub.get(ty::IDENTIFIER).expect("id");
            let mut acc_info = Vec::new();
            acc_info.extend_from_slice(&acc_pk);
            acc_info.extend_from_slice(acc_id);
            acc_info.extend_from_slice(&my_pk);
            verify_signature(
                accessory_ltpk,
                &acc_info,
                sub.get(ty::SIGNATURE).expect("sig"),
            )
            .expect("accessory proves itself");

            let mut info = Vec::new();
            info.extend_from_slice(&my_pk);
            info.extend_from_slice(self.id.as_bytes());
            info.extend_from_slice(&acc_pk);
            let sig = self.signing.sign(&info).to_bytes();
            let mut sub = Tlv::new();
            sub.push(ty::IDENTIFIER, self.id.as_bytes().to_vec())
                .push(ty::SIGNATURE, sig.to_vec());
            let sealed = crypto::seal(&session_key, nonce::PV_MSG03, b"", &sub.encode());
            let mut m3 = Tlv::new();
            m3.push_u8(ty::STATE, 3).push(ty::ENCRYPTED_DATA, sealed);
            let m4 = Tlv::decode(&verify.handle(&m3.encode(), identity, store)).expect("tlv");
            if m4.get(ty::ERROR).is_some() {
                return Err(m4.encode());
            }
            assert_eq!(m4.get_u8(ty::STATE), Some(4));
            Ok(verify.session_keys().expect("session keys after M4"))
        }
    }

    const CODE: &str = "123-45-678";

    fn identity() -> AccessoryIdentity {
        AccessoryIdentity::from_seed("AA:BB:CC:DD:EE:FF", [3u8; 32])
    }

    #[test]
    fn a_full_pair_setup_completes_and_records_the_controller() {
        let id = identity();
        let mut store = PairingStore::new();
        let mut setup = PairSetup::new(CODE).expect("setup");
        let ctrl = TestController::new("controller-uuid-1");

        let (acc_id, acc_ltpk) = ctrl
            .pair_setup(&mut setup, &id, &mut store, CODE)
            .expect("pair setup completes");

        assert_eq!(acc_id, id.pairing_id());
        assert_eq!(acc_ltpk, id.ltpk());
        assert!(setup.is_complete());
        assert_eq!(store.len(), 1);
        assert_eq!(
            store.get("controller-uuid-1").map(|p| p.ltpk),
            Some(ctrl.ltpk())
        );
    }

    /// The whole point of SRP here: a wrong setup code must fail, and must
    /// fail at the proof step without ever revealing the right one.
    #[test]
    fn a_wrong_setup_code_is_refused() {
        let id = identity();
        let mut store = PairingStore::new();
        let mut setup = PairSetup::new(CODE).expect("setup");
        let ctrl = TestController::new("controller-uuid-1");

        let err = ctrl
            .pair_setup(&mut setup, &id, &mut store, "999-99-999")
            .expect_err("wrong code must fail");
        let tlv = Tlv::decode(&err).expect("tlv");
        assert_eq!(tlv.get_u8(ty::ERROR), Some(tlv_error::AUTHENTICATION));
        assert!(store.is_empty(), "a failed setup must record nothing");
    }

    #[test]
    fn pair_verify_round_trips_after_setup() {
        let id = identity();
        let mut store = PairingStore::new();
        let mut setup = PairSetup::new(CODE).expect("setup");
        let ctrl = TestController::new("controller-uuid-1");
        let (_, acc_ltpk) = ctrl
            .pair_setup(&mut setup, &id, &mut store, CODE)
            .expect("setup");

        let mut verify = PairVerify::new();
        let keys = ctrl
            .pair_verify(&mut verify, &id, &store, &acc_ltpk)
            .expect("verify completes");

        assert_eq!(verify.verified_controller(), Some("controller-uuid-1"));
        assert_ne!(keys.read, keys.write);
    }

    /// A controller that never paired must not be able to verify, even with
    /// a perfectly well-formed transcript.
    #[test]
    fn an_unpaired_controller_cannot_verify() {
        let id = identity();
        let store = PairingStore::new();
        let ctrl = TestController::new("stranger");
        let mut verify = PairVerify::new();

        let err = ctrl
            .pair_verify(&mut verify, &id, &store, &id.ltpk())
            .expect_err("stranger must be refused");
        let tlv = Tlv::decode(&err).expect("tlv");
        assert_eq!(tlv.get_u8(ty::ERROR), Some(tlv_error::AUTHENTICATION));
        assert_eq!(verify.verified_controller(), None);
    }

    /// Session keys must be unreachable until the peer has actually proved
    /// itself — the check that stops "encrypt first, authenticate later".
    #[test]
    fn session_keys_are_unavailable_before_verification() {
        let verify = PairVerify::new();
        assert!(verify.session_keys().is_none());
    }

    /// Two connections from the same controller must not share session keys.
    #[test]
    fn each_session_gets_fresh_keys() {
        let id = identity();
        let mut store = PairingStore::new();
        let mut setup = PairSetup::new(CODE).expect("setup");
        let ctrl = TestController::new("controller-uuid-1");
        let (_, acc_ltpk) = ctrl
            .pair_setup(&mut setup, &id, &mut store, CODE)
            .expect("setup");

        let mut v1 = PairVerify::new();
        let k1 = ctrl
            .pair_verify(&mut v1, &id, &store, &acc_ltpk)
            .expect("v1");
        let mut v2 = PairVerify::new();
        let k2 = ctrl
            .pair_verify(&mut v2, &id, &store, &acc_ltpk)
            .expect("v2");

        assert_ne!(k1.read, k2.read, "sessions must not share keys");
        assert_ne!(k1.write, k2.write);
    }

    /// An accessory that is already paired must refuse a second setup.
    #[test]
    fn a_paired_accessory_refuses_pair_setup() {
        let id = identity();
        let mut store = PairingStore::new();
        store
            .add(Pairing {
                id: "existing".into(),
                ltpk: [1u8; 32],
                admin: true,
            })
            .expect("add");

        let mut setup = PairSetup::new(CODE).expect("setup");
        let mut m1 = Tlv::new();
        m1.push_u8(ty::STATE, 1)
            .push_u8(ty::METHOD, method::PAIR_SETUP);
        let reply = Tlv::decode(&setup.handle(&m1.encode(), &id, &mut store)).expect("tlv");
        assert_eq!(reply.get_u8(ty::ERROR), Some(tlv_error::UNAVAILABLE));
    }

    /// Messages must arrive in order; M5 without M3 cannot mint a pairing.
    #[test]
    fn out_of_order_messages_are_refused() {
        let id = identity();
        let mut store = PairingStore::new();
        let mut setup = PairSetup::new(CODE).expect("setup");

        let mut m5 = Tlv::new();
        m5.push_u8(ty::STATE, 5)
            .push(ty::ENCRYPTED_DATA, vec![0u8; 64]);
        let reply = Tlv::decode(&setup.handle(&m5.encode(), &id, &mut store)).expect("tlv");
        assert!(reply.get(ty::ERROR).is_some());
        assert!(store.is_empty());
    }

    #[test]
    fn garbage_input_produces_an_error_not_a_panic() {
        let id = identity();
        let mut store = PairingStore::new();
        let mut setup = PairSetup::new(CODE).expect("setup");
        for junk in [vec![0xFF; 3], vec![0x06, 0x01, 0x01, 0x03], vec![]] {
            let out = setup.handle(&junk, &id, &mut store);
            assert!(!out.is_empty(), "must always answer with a TLV");
        }
        let mut verify = PairVerify::new();
        for junk in [vec![0xFF; 3], vec![0x06, 0x01, 0x63]] {
            let out = verify.handle(&junk, &id, &store);
            assert!(!out.is_empty());
        }
    }

    #[test]
    fn the_pairing_table_is_bounded() {
        let mut store = PairingStore::new();
        for i in 0..MAX_PAIRINGS {
            store
                .add(Pairing {
                    id: format!("c{i}"),
                    ltpk: [i as u8; 32],
                    admin: false,
                })
                .expect("within bound");
        }
        assert_eq!(
            store.add(Pairing {
                id: "one-too-many".into(),
                ltpk: [0u8; 32],
                admin: false
            }),
            Err(PairError::MaxPeers)
        );
        // Re-pairing an existing controller updates in place rather than
        // failing against the bound.
        assert!(store
            .add(Pairing {
                id: "c0".into(),
                ltpk: [42u8; 32],
                admin: true
            })
            .is_ok());
        assert_eq!(store.len(), MAX_PAIRINGS);
        assert_eq!(store.get("c0").map(|p| p.admin), Some(true));
    }

    #[test]
    fn removing_a_pairing_forgets_it() {
        let mut store = PairingStore::new();
        store
            .add(Pairing {
                id: "c".into(),
                ltpk: [1u8; 32],
                admin: true,
            })
            .expect("add");
        assert!(store.remove("c"));
        assert!(!store.remove("c"));
        assert!(store.is_empty());
    }

    /// Brute force is the realistic attack on an eight-digit code, so the
    /// attempt counter must actually latch.
    #[test]
    fn repeated_failures_trip_max_tries() {
        let id = identity();
        let mut store = PairingStore::new();
        let mut setup = PairSetup::new(CODE).expect("setup");
        let ctrl = TestController::new("attacker");

        for _ in 0..MAX_SETUP_ATTEMPTS {
            let _ = ctrl.pair_setup(&mut setup, &id, &mut store, "000-00-000");
        }
        let mut m1 = Tlv::new();
        m1.push_u8(ty::STATE, 1)
            .push_u8(ty::METHOD, method::PAIR_SETUP);
        let reply = Tlv::decode(&setup.handle(&m1.encode(), &id, &mut store)).expect("tlv");
        assert_eq!(reply.get_u8(ty::ERROR), Some(tlv_error::MAX_TRIES));
    }

    /// The identity seed is the accessory: restoring it must reproduce the
    /// same public key, or every controller would have to re-pair after a
    /// restart.
    #[test]
    fn identity_is_reproducible_from_its_seed() {
        let a = AccessoryIdentity::from_seed("id", [7u8; 32]);
        let b = AccessoryIdentity::from_seed("id", [7u8; 32]);
        assert_eq!(a.ltpk(), b.ltpk());
        let c = AccessoryIdentity::from_seed("id", [8u8; 32]);
        assert_ne!(a.ltpk(), c.ltpk());
    }

    /// A secret that shows up in a debug log is a secret that leaks.
    #[test]
    fn secrets_are_not_in_debug_output() {
        let id = identity();
        let rendered = format!("{id:?}");
        assert!(rendered.contains("redacted"));
        assert!(!rendered.contains(&hex::encode(id.signing.to_bytes())));

        let keys = SessionKeys {
            read: [1u8; 32],
            write: [2u8; 32],
        };
        let rendered = format!("{keys:?}");
        assert!(!rendered.contains('1'), "session keys must not render");
    }
}
