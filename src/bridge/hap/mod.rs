//! A HomeKit Accessory Protocol server for `witnessd` — bridge site B of
//! `docs/design/apple_home_integration.md`.
//!
//! This is the lane that needs nothing but an Apple hub: `witnessd`
//! advertises itself over mDNS, a controller pairs with it directly, and the
//! fleet appears in the Home app. No Home Assistant, no cloud, no account.
//!
//! # What this module is, and what it is not
//!
//! It is a **transport**. Every value it publishes comes from
//! [`Projection`](super::homekit::Projection), which is where the privacy
//! properties live — the closed signal vocabulary, and the metronome that
//! keeps publication rate independent of event occurrence (Invariant III).
//! Nothing here may read kernel state directly or publish off-tick; a bridge
//! that did would reintroduce the event-timing oracle the projection exists
//! to remove, one layer down.
//!
//! # Why it is hand-rolled
//!
//! The FR-14 dependency gate rejected both published HAP server crates.
//! `hap 0.0.10` does not compile on a current toolchain (it pulls tokio 0.1,
//! hyper 0.12, ring 0.14 and a 2016-era `syntex` codegen crate), and
//! `hap 0.1.0-pre.15` cannot even be resolved — it depends on `libmdns` and
//! `get_if_addrs`, which both link the native `ifaddrs` library, and Cargo
//! refuses two `links` claims on one graph. So the protocol is implemented
//! here on primitives the tree already carries (`ed25519-dalek`, `sha2`,
//! `hkdf`, `chacha20poly1305`, `subtle`) plus three small vetted additions
//! (`srp`, `x25519-dalek`, `mdns-sd`). That is 55 packages against 219, and
//! every one of them is maintained.
//!
//! # Layout
//!
//! | Module | What it owns |
//! |---|---|
//! | [`tlv8`] | The type-length-value codec pairing speaks |
//! | [`accessory`] | Services, characteristics and the stable-iid scheme |
//! | [`config`] | What the wizard writes down, so it is asked once |
//! | [`discover`] | Finding Apple hubs on the network, so the wizard can look instead of ask |
//! | [`crypto`] | HKDF-SHA512, ChaCha20-Poly1305 under HAP's nonce rule |
//! | [`http`] | The narrow HTTP/1.1 subset HAP uses, plus `EVENT/1.0` |
//! | [`qr`] | The setup code as a QR you can scan off your terminal |
//! | [`pairing`] | Pair Setup (SRP-6a) and Pair Verify (X25519), and the pairing table |
//! | [`server`] | Discovery, connections, routing, and the metronome |
//! | [`store`] | Identity, setup code and pairings across restarts |
//! | [`tty`] | Turning terminal echo off, and getting it back on however we leave |
//! | [`wizard`] | The guessing and prompt-parsing behind `hap_bridge setup` |
//! | [`session`] | The encrypted frame transport that follows Pair Verify |

pub mod accessory;
pub mod config;
pub mod crypto;
pub mod discover;
pub mod http;
pub mod pairing;
pub mod qr;
pub mod server;
pub mod session;
pub mod store;
pub mod tlv8;
pub mod tty;
pub mod wizard;
