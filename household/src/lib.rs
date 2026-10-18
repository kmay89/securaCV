//! The SecuraCV household-key checker.
//!
//! A household member presents a **household key** (a phone or watch key
//! behind that device's own user verification, a FIDO2 key, a fob, an NFC
//! card) and the hub turns that deliberate, signed act into a **household
//! posture**: `away`, `home` or `night`. The posture is published as a fixed
//! 112-byte statement signed by the hub's posture key ([`posture`]).
//!
//! This crate is the hub side of that contract, and only the hub side:
//!
//! - [`wire`]: the challenge message every credential signs.
//! - [`posture`]: the posture statement, its encoding, signing, and every
//!   witness-side check, including the `home` lapse.
//! - [`cjson`], [`roster`], [`admin`], [`change`]: the roster (random handles,
//!   public keys, credential classes, no names), its canonical encoding, the
//!   algorithm-tagged admin approvals and the change rules with the
//!   epoch/prev chain and rollback refusal.
//! - [`challenge`]: single-use, session-bound, 10-second challenges drawn from
//!   the OS RNG, all discarded on every roster epoch change.
//! - [`verify`]: the `es256` and `fido2-es256` credential verifiers.
//! - [`ntag424`]: the reader side of NTAG 424 DNA `AuthenticateEV2First`.
//! - [`secrets`]: the card keys (`cards.sealed`) and the hub's two signing
//!   keys, posture and roster (`hub_keys.sealed`), sealed with
//!   XChaCha20-Poly1305 under keys derived from one key-encryption key kept
//!   outside the data directory.
//! - [`checker`]: the state machine tying those together, with the per-door
//!   rejection limiter ([`ratelimit`]).
//! - [`server`]: the bounded LAN API (`POST /v1/challenge`, `POST
//!   /v1/command`, `GET /v1/posture`), loopback by default.
//! - [`client`]: a reference software credential, so the LAN API can be
//!   exercised end to end without a phone.
//!
//! Nothing here is a biometric and nothing here identifies a person. No
//! posture statement, LAN API response body, output seam or server log line
//! carries a member handle or a credential id (sentinel tests hold each of
//! those). The roster itself necessarily lists random handles, and the
//! operator's own `roster` subcommands print them. See `README.md` for the
//! threat model and for what is not built.

pub mod admin;
pub mod challenge;
pub mod change;
pub mod checker;
pub mod cjson;
pub mod client;
pub mod error;
pub mod hex;
pub mod ntag424;
pub mod posture;
pub mod ratelimit;
pub mod rng;
pub mod roster;
pub mod secrets;
pub mod server;
pub mod store;
pub mod verify;
pub mod wire;

pub use error::{Error, Result};
