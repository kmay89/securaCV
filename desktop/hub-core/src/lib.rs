//! hub-core — the pure, footgun-critical logic behind the securaCV Home
//! Assistant hub flasher.
//!
//! Writing a whole-OS image to a raw disk is the one thing the flasher can do
//! that isn't can't-brick-safe like an ESP32 flash: a wrong-disk write destroys
//! data, possibly the operator's own boot disk. The decisions that keep that
//! from happening — *what is a legal write target* ([`hub_disk`]) and *how to
//! read the machine's disks and tell the system disk from an external one*
//! ([`hub_enumerate`]) — live here, in their own crate, deliberately free of any
//! Tauri / UI / networking dependency.
//!
//! Why a separate crate: the desktop app (`../src-tauri`) only builds on release
//! tags and needs the webkit/gtk stack, so nothing in PR CI would catch a break
//! in this logic if it lived there. As pure `std` with no dependencies, this
//! crate `cargo test`s on any runner — so the safety layer is verified on every
//! change, and the writer that will call it (in `../src-tauri`, via a path
//! dependency) can't be handed a subtly-broken gate.

pub mod hub_disk;
pub mod hub_enumerate;
pub mod hub_image;
pub mod hub_seed;
