//! The facts both apps derive from the bundled flasher catalog
//! (`canary-local/devices/flash.json`, embedded at build time by each app's
//! build.rs): the chip spellings behind the "wrong image" guard, the one
//! release origin downloads may come from, and the closed set of manifest URLs
//! the app will ever fetch.
//!
//! Every one of them DERIVES from the catalog rather than restating it — the
//! desktop-parity test used to diff hardcoded copies against the catalog, and
//! deriving removed the copies. `canary-local/tests/desktop_parity.test.js`
//! asserts the derivations stay (and no literal creeps back).

use serde_json::Value;

/// The dev channel's one stable address: the rolling fw-dev-latest prerelease
/// that CI re-points on every fw-v*-dev.*/-rc.* tag. This is a fixed
/// first-party constant, deliberately NOT a general manifest-URL override —
/// the dev toggle can only ever mean this URL. It is the ONE alternative the
/// flash pipeline accepts to the catalog's pinned manifest_url; everything
/// downstream (chip guard, release origin, size/SHA, signature policy) is
/// identical. Mirrors canary-local/assets/flash-core.js DEV_FLASH_MANIFEST_URL
/// and desktop/src/app.js — the desktop-parity test fails if they drift.
pub const DEV_FLASH_MANIFEST_URL: &str =
    "https://github.com/kmay89/securaCV/releases/download/fw-dev-latest/manifest-flash.json";

/// The bundled catalog plus what derives from it, computed once. Each app
/// keeps one in a `OnceLock` over its `include_str!` of flash.json.
pub struct Catalog {
    raw: &'static str,
    chips: Vec<(String, String)>,
    origin: Option<String>,
}

impl Catalog {
    /// Derive everything from the embedded catalog text. A corrupt catalog
    /// still yields a value: an empty chip table and no release origin, so
    /// every catalog flash path fails closed behind its own parse.
    pub fn new(raw: &'static str) -> Self {
        Catalog {
            raw,
            chips: chip_table(raw),
            origin: release_origin(raw),
        }
    }

    /// The catalog text exactly as embedded.
    pub fn raw(&self) -> &'static str {
        self.raw
    }

    /// The (folded token, canonical spelling) pairs derived from `chips`.
    pub fn chip_table(&self) -> &[(String, String)] {
        &self.chips
    }

    /// See [`canonical_chip()`].
    pub fn canonical_chip(&self, raw: &str) -> Option<String> {
        canonical_chip(&self.chips, raw)
    }

    /// The one origin release assets may be downloaded from (everything up to
    /// and including `/releases/download/` of the catalog's manifest_url).
    /// None = fail closed: nothing downloads.
    pub fn release_origin(&self) -> Option<&str> {
        self.origin.as_deref()
    }

    /// See [`manifest_url_allowed()`].
    pub fn manifest_url_allowed(&self, url: &str) -> bool {
        manifest_url_allowed(self.raw, url)
    }
}

/// The catalog's chip spellings, derived from the catalog's `chips` keys
/// instead of a hardcoded copy of them. Each canonical spelling ("ESP32-S3")
/// yields its folded token ("esp32s3"); [`canonical_chip`] looks tokens up
/// here by exact match, so the catalog's exact spelling wins for chips it
/// ships, and falls back to spelling the token itself for ESP32-family chips
/// it doesn't. A corrupt catalog yields an empty table; detection still names
/// chips (rescue is catalog-independent), while every catalog flash path stays
/// behind its own catalog parse.
fn chip_table(catalog: &str) -> Vec<(String, String)> {
    let Ok(catalog) = serde_json::from_str::<Value>(catalog) else {
        return Vec::new();
    };
    let mut table: Vec<(String, String)> = catalog
        .get("chips")
        .and_then(Value::as_object)
        .map(|chips| {
            chips
                .keys()
                .map(|canon| {
                    let needle = canon.to_lowercase().replace(['-', ' ', '_'], "");
                    (needle, canon.clone())
                })
                .collect()
        })
        .unwrap_or_default();
    table.sort(); // deterministic order; lookup is by exact token
    table
}

/// The one origin the apps download release assets from, derived from the
/// catalog's own pinned manifest_url (everything up to and including
/// `/releases/download/`) — a repo move edits flash.json and every guard
/// follows, instead of a literal in each flash path. None (fail closed:
/// nothing downloads) if the catalog is corrupt or its manifest_url is not
/// a releases/download URL — states the browser Lab cannot reach either.
fn release_origin(catalog: &str) -> Option<String> {
    let catalog = serde_json::from_str::<Value>(catalog).ok()?;
    let url = catalog.get("manifest_url")?.as_str()?;
    let marker = "/releases/download/";
    let end = url.find(marker)? + marker.len();
    Some(url[..end].to_string())
}

/// Normalize whatever `espflash board-info` calls the chip into a canonical
/// spelling ("ESP32-S3", "ESP32-C3", …). The chip token is extracted from the
/// output ("esp32" plus an optional variant suffix — one letter, then digits:
/// s3, c6, p4 — preferring an occurrence that names a variant, so a bare
/// "esp32" elsewhere in the output can't hide one). A token the catalog ships
/// gets the catalog's spelling (`table`); an ESP32-family variant the
/// catalog does NOT ship still gets its real name (ESP32-S2, ESP32-H2), never
/// bare "ESP32" and never None — the catalog is a product list, not a
/// detection whitelist: espflash already talked to the chip, and the
/// catalog-independent rescue/local-file operations need it identified, while
/// the catalog flash paths refuse the (now truthful) chip mismatch. Only
/// output naming no ESP32-family chip at all answers None.
pub fn canonical_chip(table: &[(String, String)], raw: &str) -> Option<String> {
    let s = raw.to_lowercase().replace(['-', ' ', '_'], "");
    let mut token: Option<&str> = None;
    let mut at = 0;
    while let Some(i) = s[at..].find("esp32") {
        let start = at + i;
        let rest = &s.as_bytes()[start + 5..];
        let mut suffix_len = 0;
        if rest.first().is_some_and(u8::is_ascii_lowercase) {
            let digits = rest[1..].iter().take_while(|b| b.is_ascii_digit()).count();
            if digits > 0 {
                suffix_len = 1 + digits;
            }
        }
        if suffix_len > 0 {
            token = Some(&s[start..start + 5 + suffix_len]);
            break;
        }
        token.get_or_insert("esp32");
        at = start + 5;
    }
    let token = token?;
    if let Some((_, canon)) = table.iter().find(|(needle, _)| needle == token) {
        return Some(canon.clone());
    }
    let suffix = &token[5..];
    Some(if suffix.is_empty() {
        "ESP32".to_string()
    } else {
        format!("ESP32-{}", suffix.to_uppercase())
    })
}

/// The manifest URLs an app will ever fetch: the catalog's pinned stable
/// release, the fixed fw-dev-latest dev constant, and the catalog's pinned
/// Vision-module manifest. `fetch_manifest` is reachable straight from the
/// webview, so the gate lives HERE, not only in the flash paths that happen
/// to call it — the same closed-set discipline the flash paths already apply
/// before a byte moves.
pub fn manifest_url_allowed(catalog: &str, url: &str) -> bool {
    if url == DEV_FLASH_MANIFEST_URL {
        return true;
    }
    let Ok(catalog) = serde_json::from_str::<Value>(catalog) else {
        return false;
    };
    let bundled = |v: Option<&Value>| v.and_then(Value::as_str) == Some(url);
    bundled(catalog.get("manifest_url"))
        || bundled(
            catalog
                .get("we2_module")
                .and_then(|module| module.get("manifest_url")),
        )
}

#[cfg(test)]
mod tests {
    use super::*;

    // The real catalog both apps embed. These used to be hardcoded copies of
    // catalog facts, diffed against the catalog by desktop_parity.test.js.
    // Now they derive; the tests pin the derivation against the catalog
    // itself, so a regression back to a literal (or a broken parse) fails here
    // first.
    const REPO_CATALOG: &str = include_str!("../../../canary-local/devices/flash.json");

    #[test]
    fn chips_derive_from_the_catalog_and_variants_win() {
        let bundled = Catalog::new(REPO_CATALOG);
        let canonical_chip = |raw: &str| bundled.canonical_chip(raw);
        let catalog: Value = serde_json::from_str(REPO_CATALOG).unwrap();
        let chips = catalog["chips"].as_object().expect("catalog has chips");
        // Every catalog chip canonicalizes to itself, from espflash-ish
        // spellings too.
        for canon in chips.keys() {
            assert_eq!(canonical_chip(canon).as_deref(), Some(canon.as_str()));
            let sloppy = canon.to_lowercase().replace('-', "_");
            assert_eq!(
                canonical_chip(&format!("Chip type: {sloppy} (rev 0)")).as_deref(),
                Some(canon.as_str()),
                "espflash-style spelling of {canon}"
            );
        }
        // A variant token must never fold to bare ESP32.
        assert_eq!(canonical_chip("esp32-s3").as_deref(), Some("ESP32-S3"));
        // The table carries exactly the catalog's chips — no leftovers of
        // the old hardcoded list.
        assert_eq!(bundled.chip_table().len(), chips.len());
        // An ESP32-family variant the catalog does NOT ship still gets its
        // real name — never bare "ESP32" (that would defeat the flash chip
        // guard), never None (rescue/local-file operations are
        // catalog-independent and need the chip identified).
        assert_eq!(
            canonical_chip("Chip type: esp32s2 (revision v0.0)").as_deref(),
            Some("ESP32-S2")
        );
        assert_eq!(canonical_chip("esp32-h2").as_deref(), Some("ESP32-H2"));
        assert_eq!(canonical_chip("esp32c2").as_deref(), Some("ESP32-C2"));
        // A variant named anywhere wins over a bare esp32 mention earlier on.
        assert_eq!(
            canonical_chip("esp32 family: esp32s2").as_deref(),
            Some("ESP32-S2")
        );
        // Output naming no ESP32-family chip at all answers None.
        assert_eq!(canonical_chip("rp2040"), None);
    }

    #[test]
    fn release_origin_derives_from_the_catalog_manifest() {
        let bundled = Catalog::new(REPO_CATALOG);
        let catalog: Value = serde_json::from_str(REPO_CATALOG).unwrap();
        let manifest_url = catalog["manifest_url"].as_str().unwrap();
        let origin = bundled
            .release_origin()
            .expect("catalog carries a releases/download manifest_url");
        assert!(origin.ends_with("/releases/download/"));
        assert!(manifest_url.starts_with(origin));
        // The guard the flash paths apply: catalog-origin assets pass, a
        // foreign host does not.
        assert!(!("https://example.com/releases/download/x.bin").starts_with(origin));
    }

    #[test]
    fn a_corrupt_catalog_fails_closed() {
        let bundled = Catalog::new("{ not json");
        assert!(bundled.chip_table().is_empty());
        assert_eq!(bundled.release_origin(), None);
        // Detection still names the chip (rescue is catalog-independent)…
        assert_eq!(
            bundled.canonical_chip("esp32s3").as_deref(),
            Some("ESP32-S3")
        );
        // …but only the dev constant survives the manifest gate.
        assert!(bundled.manifest_url_allowed(DEV_FLASH_MANIFEST_URL));
        assert!(!bundled.manifest_url_allowed("https://example.com/manifest-flash.json"));
    }

    #[test]
    fn only_the_bundled_manifest_urls_are_fetchable() {
        // The closed set: catalog stable pin, dev constant, Vision-module pin.
        let bundled = Catalog::new(REPO_CATALOG);
        let manifest_url_allowed = |url: &str| bundled.manifest_url_allowed(url);
        let catalog: Value = serde_json::from_str(REPO_CATALOG).unwrap();
        let stable = catalog
            .get("manifest_url")
            .and_then(|v| v.as_str())
            .unwrap();
        assert!(manifest_url_allowed(stable));
        assert!(manifest_url_allowed(DEV_FLASH_MANIFEST_URL));
        if let Some(we2) = catalog
            .get("we2_module")
            .and_then(|m| m.get("manifest_url"))
            .and_then(|v| v.as_str())
        {
            assert!(manifest_url_allowed(we2));
        }
        // Anything else — including lookalikes — is refused before a socket.
        assert!(!manifest_url_allowed(
            "https://example.com/manifest-flash.json"
        ));
        assert!(!manifest_url_allowed(&format!(
            "{DEV_FLASH_MANIFEST_URL}.evil"
        )));
        assert!(!manifest_url_allowed(""));
    }
}
