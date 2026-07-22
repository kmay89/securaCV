//! hub_image — resolve the hub-image catalog into a concrete "what to write" plan.
//!
//! Step 3's brain. `canary-local/devices/hub_image.json` (the drift-gated single
//! source of truth — see the design doc) says which HAOS-based image goes on
//! which Raspberry Pi, how big a card it needs, and whether the image has been
//! pinned. This module turns that catalog into a typed [`WritePlan`] the writer
//! can act on, with the sanity checks made explicit — which board, which URL,
//! whether we can trust a pinned hash or must fall back to HA's published
//! `.sha256`.
//!
//! It stays pure and dependency-free: the desktop app deserialises the JSON into
//! a [`CatalogView`] (it already has serde for the flasher catalog) and calls
//! [`resolve`]. Keeping the parsing in the app and the *reasoning* here means the
//! resolution logic is unit-tested in `hub-core` on every PR, same as the disk
//! safety gate — the catalog can't hand the writer a nonsensical plan without a
//! test noticing.

/// One flashable board option, mirroring an entry of `base_os.boards[]`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BoardImage {
    /// Catalog board id, e.g. `rpi5-64`.
    pub id: String,
    /// Human name, e.g. `Raspberry Pi 5 (64-bit)`.
    pub name: String,
    /// The recommended default (Pi 5, NVMe/SSD durable path).
    pub recommended: bool,
    /// Where to download the image (derived from the pinned HAOS version).
    pub image_url: String,
    /// Expected SHA-256, or empty until the pin ceremony sets it.
    pub sha256: String,
}

/// The slice of `hub_image.json` the writer needs. A plain struct on purpose:
/// the desktop app fills it from the JSON, `hub-core` stays serde-free.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CatalogView {
    /// `base_os.name`, e.g. `Home Assistant OS`.
    pub os_name: String,
    /// `base_os.version`, e.g. `18.1`.
    pub os_version: String,
    /// `base_os.pinned` — whether the images carry a verified sha256 yet.
    pub pinned: bool,
    /// `card_requirements.min_bytes` — the supported-minimum card size. (Equal
    /// to `hub_disk::MIN_TARGET_BYTES` by the drift gate; carried here so the UI
    /// can state the requirement without re-deriving it.)
    pub min_card_bytes: u64,
    /// `base_os.boards[]`.
    pub boards: Vec<BoardImage>,
}

/// Why a catalog can't be resolved into a write plan for a given board.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ResolveError {
    /// No board with that id — the message lists what's on offer.
    UnknownBoard { id: String, known: Vec<String> },
    /// The board exists but carries no image URL (a malformed catalog).
    NoImageUrl { board: String },
}

impl ResolveError {
    pub fn message(&self) -> String {
        match self {
            ResolveError::UnknownBoard { id, known } => format!(
                "no hub image for board '{id}' — known boards: {}",
                known.join(", ")
            ),
            ResolveError::NoImageUrl { board } => {
                format!("the catalog entry for '{board}' has no image URL")
            }
        }
    }
}

/// A non-fatal note about the plan, surfaced before the write.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PlanWarning {
    /// The image isn't pinned yet (`pinned=false` or an empty sha256), so the
    /// writer must verify the download against HA's published `.sha256` rather
    /// than a hash this repo vouches for.
    ImageNotPinned,
}

impl PlanWarning {
    pub fn message(&self) -> String {
        match self {
            PlanWarning::ImageNotPinned => {
                "this image isn't pinned in the catalog yet — the download will be verified against \
                 Home Assistant's published checksum instead of a repo-vouched hash"
                    .to_string()
            }
        }
    }
}

/// The resolved, sanity-checked instruction set for a hub flash.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WritePlan {
    pub board_id: String,
    /// e.g. `Home Assistant OS 18.1`.
    pub os_label: String,
    pub image_url: String,
    /// `Some(hash)` only when the catalog is pinned AND the board carries a
    /// non-empty sha256; otherwise `None` (verify against HA's `.sha256`).
    pub expected_sha256: Option<String>,
    /// The supported-minimum card size in bytes (the writer still runs the
    /// actual disk through `hub_disk::classify`; this is for the requirement copy).
    pub min_card_bytes: u64,
    pub warnings: Vec<PlanWarning>,
}

/// Resolve the catalog into a [`WritePlan`] for one board. Pure; the only
/// judgement is "is this board real, does it have a URL, and can we trust a
/// pinned hash yet" — everything the writer needs decided in one tested place.
pub fn resolve(catalog: &CatalogView, board_id: &str) -> Result<WritePlan, ResolveError> {
    let board = catalog
        .boards
        .iter()
        .find(|b| b.id == board_id)
        .ok_or_else(|| ResolveError::UnknownBoard {
            id: board_id.to_string(),
            known: catalog.boards.iter().map(|b| b.id.clone()).collect(),
        })?;

    if board.image_url.trim().is_empty() {
        return Err(ResolveError::NoImageUrl {
            board: board_id.to_string(),
        });
    }

    // Trust a pinned hash only when BOTH the catalog is pinned and this board
    // actually carries one; any weaker state falls back to HA's own checksum.
    let (expected_sha256, mut warnings) = if catalog.pinned && !board.sha256.trim().is_empty() {
        (Some(board.sha256.clone()), Vec::new())
    } else {
        (None, vec![PlanWarning::ImageNotPinned])
    };
    warnings.shrink_to_fit();

    Ok(WritePlan {
        board_id: board.id.clone(),
        os_label: format!("{} {}", catalog.os_name, catalog.os_version)
            .trim()
            .to_string(),
        image_url: board.image_url.clone(),
        expected_sha256,
        min_card_bytes: catalog.min_card_bytes,
        warnings,
    })
}

/// The recommended board id, if the catalog marks one — the picker's default.
pub fn recommended_board(catalog: &CatalogView) -> Option<&str> {
    catalog
        .boards
        .iter()
        .find(|b| b.recommended)
        .map(|b| b.id.as_str())
}

#[cfg(test)]
mod tests {
    use super::*;

    // A fixture mirroring the shape of the committed hub_image.json (HAOS 18.1,
    // two boards, not yet pinned). We test the resolver's logic, not the file's
    // contents — the catalog↔code match is enforced separately by the
    // gen_hub_image.py drift gate.
    fn unpinned_catalog() -> CatalogView {
        CatalogView {
            os_name: "Home Assistant OS".to_string(),
            os_version: "18.1".to_string(),
            pinned: false,
            min_card_bytes: 28 * 1024 * 1024 * 1024,
            boards: vec![
                BoardImage {
                    id: "rpi5-64".to_string(),
                    name: "Raspberry Pi 5 (64-bit)".to_string(),
                    recommended: true,
                    image_url:
                        "https://github.com/home-assistant/operating-system/releases/download/18.1/haos_rpi5-64-18.1.img.xz"
                            .to_string(),
                    sha256: String::new(),
                },
                BoardImage {
                    id: "rpi4-64".to_string(),
                    name: "Raspberry Pi 4 (64-bit, 4 GB+)".to_string(),
                    recommended: false,
                    image_url:
                        "https://github.com/home-assistant/operating-system/releases/download/18.1/haos_rpi4-64-18.1.img.xz"
                            .to_string(),
                    sha256: String::new(),
                },
            ],
        }
    }

    #[test]
    fn resolves_the_recommended_board_to_its_image() {
        let cat = unpinned_catalog();
        let plan = resolve(&cat, "rpi5-64").expect("rpi5-64 resolves");
        assert_eq!(plan.board_id, "rpi5-64");
        assert_eq!(plan.os_label, "Home Assistant OS 18.1");
        assert!(plan.image_url.ends_with("haos_rpi5-64-18.1.img.xz"));
        assert_eq!(plan.min_card_bytes, 28 * 1024 * 1024 * 1024);
    }

    #[test]
    fn resolves_the_other_board_too() {
        let plan = resolve(&unpinned_catalog(), "rpi4-64").unwrap();
        assert!(plan.image_url.ends_with("haos_rpi4-64-18.1.img.xz"));
    }

    #[test]
    fn an_unpinned_catalog_yields_no_trusted_hash_and_warns() {
        let plan = resolve(&unpinned_catalog(), "rpi5-64").unwrap();
        assert_eq!(plan.expected_sha256, None);
        assert_eq!(plan.warnings, vec![PlanWarning::ImageNotPinned]);
    }

    #[test]
    fn a_pinned_catalog_yields_the_trusted_hash_and_no_warning() {
        let mut cat = unpinned_catalog();
        cat.pinned = true;
        cat.boards[0].sha256 = "a".repeat(64);
        let plan = resolve(&cat, "rpi5-64").unwrap();
        assert_eq!(plan.expected_sha256, Some("a".repeat(64)));
        assert!(plan.warnings.is_empty());
    }

    #[test]
    fn pinned_catalog_but_empty_board_hash_still_falls_back() {
        // Defence: the catalog says pinned, but this board's sha is blank —
        // don't invent trust; fall back to HA's checksum.
        let mut cat = unpinned_catalog();
        cat.pinned = true; // board sha256 stays ""
        let plan = resolve(&cat, "rpi5-64").unwrap();
        assert_eq!(plan.expected_sha256, None);
        assert_eq!(plan.warnings, vec![PlanWarning::ImageNotPinned]);
    }

    #[test]
    fn an_unknown_board_errors_with_the_known_ones() {
        let err = resolve(&unpinned_catalog(), "rpi9-128").unwrap_err();
        assert_eq!(
            err,
            ResolveError::UnknownBoard {
                id: "rpi9-128".to_string(),
                known: vec!["rpi5-64".to_string(), "rpi4-64".to_string()],
            }
        );
        assert!(err.message().contains("rpi5-64"));
    }

    #[test]
    fn a_board_without_a_url_is_rejected() {
        let mut cat = unpinned_catalog();
        cat.boards[0].image_url = "   ".to_string();
        assert_eq!(
            resolve(&cat, "rpi5-64").unwrap_err(),
            ResolveError::NoImageUrl {
                board: "rpi5-64".to_string()
            }
        );
    }

    #[test]
    fn recommended_board_is_the_pi5() {
        assert_eq!(recommended_board(&unpinned_catalog()), Some("rpi5-64"));
    }

    #[test]
    fn warning_and_error_render_human_text() {
        assert!(!PlanWarning::ImageNotPinned.message().is_empty());
        let e = ResolveError::UnknownBoard {
            id: "x".to_string(),
            known: vec!["rpi5-64".to_string()],
        };
        assert!(e.message().contains("rpi5-64"));
    }
}
