//! hub-io — the I/O half of the securaCV Home Assistant hub writer.
//!
//! `hub-core` decides; this crate *does*. It carries the four acts of the
//! one-flash Pi hub pipeline (docs/design/raspberry_pi_hub_flashing.md):
//!
//!   1. [`fetch`] — stream the HAOS image download to disk, computing its
//!      SHA-256 as it arrives, and fetch HA's published `.sha256` for the
//!      unpinned fallback. The hash it returns feeds `hub_core::hub_image::
//!      verify_download`, which is the ONLY authority on whether the bytes are
//!      trusted.
//!   2. [`xz`] — stream-decompress the `.img.xz` into the raw image, hashing
//!      the decompressed bytes — that hash is what the post-write read-back
//!      must reproduce from the disk itself.
//!   3. [`mod@write`] — the destructive write. Its entry point takes
//!      `hub_core::hub_flash::WriteAuthorization` **by value**: unverified
//!      image, refused disk, or missing operator confirmation isn't a code
//!      path here, it's a compile error over in the caller. Every write is
//!      followed by a full read-back, re-hashed and compared.
//!   4. [`seed`] — drop the NetworkManager Wi-Fi keyfile (built by
//!      `hub_core::hub_seed`) into `CONFIG/network/` on the freshly written
//!      boot partition, so the Pi joins the operator's Wi-Fi on first boot.
//!
//! Why a separate crate instead of code in `src-tauri`: the desktop app only
//! builds on release tags (it needs webkit/gtk), so nothing there is exercised
//! by PR CI. Everything here builds and tests on a bare runner — the streaming
//! hash math, the xz plumbing, the read-back verify, and the seed layout are
//! all verified on every change against plain files and a loopback HTTP
//! server. Only the thin platform edges (opening a raw device, mounting a
//! partition, ejecting) touch a real disk, and each of those funnels through
//! the tested core.
//!
//! Like the rest of the hub path: HARDWARE VALIDATION IS STILL REQUIRED before
//! this ships in a tagged release — these tests prove the logic, not a boot.

pub mod account;
pub mod fetch;
pub mod onboarding;
pub mod provision;
pub mod seed;
pub mod write;
pub mod xz;

/// A cooperative stop signal for the long stages (download, decompress,
/// write, read-back). Cloneable and thread-safe; the UI's Stop button flips
/// it, and every chunk loop calls [`CancelToken::checkpoint`] between chunks.
///
/// Canceling is always SAFE for the hardware — mid-download/decompress
/// nothing has touched the card, and mid-write the card was being erased
/// anyway; it simply needs a fresh flash. The checkpoint error carries the
/// [`CANCELED`] marker so the UI can present a calm "stopped, nothing hurt"
/// instead of a failure.
#[derive(Debug, Clone, Default)]
pub struct CancelToken(std::sync::Arc<std::sync::atomic::AtomicBool>);

/// Prefix of every cancellation error — lets callers distinguish "the
/// operator said stop" (neutral) from "something went wrong" (an error).
pub const CANCELED: &str = "canceled:";

impl CancelToken {
    pub fn cancel(&self) {
        self.0.store(true, std::sync::atomic::Ordering::Relaxed);
    }
    pub fn is_canceled(&self) -> bool {
        self.0.load(std::sync::atomic::Ordering::Relaxed)
    }
    /// `Err` with the [`CANCELED`] marker once [`cancel`](Self::cancel) has
    /// been called — the chunk loops bubble this out between chunks.
    pub fn checkpoint(&self) -> Result<(), String> {
        if self.is_canceled() {
            Err(format!("{CANCELED} stopped at your request"))
        } else {
            Ok(())
        }
    }
}

/// One progress tick. `done`/`total` are bytes within the current stage;
/// `total` is `None` when the stage's size isn't knowable up front (e.g. a
/// download with no Content-Length).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Progress {
    pub stage: Stage,
    pub done: u64,
    pub total: Option<u64>,
}

/// The pipeline stages, in the order the user watches them happen.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Stage {
    Download,
    Decompress,
    Write,
    Verify,
    Seed,
}

impl Stage {
    /// Stable machine name for progress events (the UI maps these to copy).
    pub fn name(&self) -> &'static str {
        match self {
            Stage::Download => "download",
            Stage::Decompress => "decompress",
            Stage::Write => "write",
            Stage::Verify => "verify",
            Stage::Seed => "seed",
        }
    }
}

/// Finish a running SHA-256 into lowercase hex — the one spelling the whole
/// trust chain (catalog pins, HA's `.sha256`, `verify_download`) compares.
pub(crate) fn sha256_hex(hasher: sha2::Sha256) -> String {
    use sha2::Digest;
    let digest = hasher.finalize();
    let mut out = String::with_capacity(64);
    for b in digest {
        out.push_str(&format!("{b:02x}"));
    }
    out
}
