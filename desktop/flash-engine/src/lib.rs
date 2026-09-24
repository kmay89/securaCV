//! The ESP32 flash engine both SecuraCV desktop apps share.
//!
//! The Flasher (`desktop/src-tauri`) was the only native app that could put
//! firmware on a Canary; the Lab (`desktop-lab/src-tauri`) needed the same
//! path, and copying ~5k lines of flash logic into a second crate would have
//! given the project two engines to drift apart. So the logic lives here,
//! tauri-free, in the hub-core / hub-io pattern: a plain crate that PR CI
//! builds and tests on a bare runner (`desktop-hub-core.yml`), while each app
//! keeps only a thin Tauri command layer over it.
//!
//! What lives here:
//!   * the pure decisions — [`catalog`] (chip guard, release origin, the
//!     manifest allow-list), [`release`] (size/SHA-256/Ed25519),
//!     [`provisioning`] (NVS sealed into the image) and [`broker_receipt`]
//!     (the one line a receipt says about the broker TLS it sealed —
//!     std-only, held equal to the browser flasher's table), [`changemap`] +
//!     [`health`] (what an install touches), [`intake`] (is this board what it
//!     claims), [`rescue`] (espflash argv builders), [`port_hint`] (Linux port
//!     diagnostics), [`image`] (the offset-0 guard + private staging);
//!   * the orchestration — [`flash`] (detect, fetch, verify, provision, write,
//!     receipt) and [`monitor`] (the boot-receipt serial monitor), written
//!     against the [`host`] seam so the app supplies the sidecar spawn and the
//!     event sink and nothing else;
//!   * the I/O both apps would otherwise duplicate — [`net`] (manifest fetch
//!     and download) and [`ports`] (the OS port list).
//!
//! What does NOT live here: anything Tauri. The sidecar spawn (Tauri's shell
//! plugin), the managed state, the event emitter and the command registration
//! stay in each app, which is also where each app records its sidecar PIDs.
//! The command NAMES, argument names, DTOs and event names the two apps expose
//! are held identical by `canary-local/tests/desktop_parity.test.js`.

pub mod broker_receipt;
pub mod catalog;
pub mod changemap;
pub mod flash;
pub mod health;
pub mod host;
pub mod image;
pub mod intake;
pub mod monitor;
pub mod net;
pub mod port_hint;
pub mod ports;
pub mod provisioning;
pub mod release;
pub mod rescue;
pub mod sidecar;
