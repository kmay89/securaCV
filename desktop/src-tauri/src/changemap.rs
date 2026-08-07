//! The change map: what this install will actually touch.
//!
//! The safety copy already read every byte off the board, and the signed
//! image is staged and verified before a byte is written — so at that exact
//! moment both sides of the comparison exist and the app can answer the
//! question people really ask: *do my settings survive this?*
//!
//! Port of the browser's `diffInstall` / `settingsVerdict` (flash-core.js),
//! anchored on `health::parse_partition_table` so both frontends read the
//! same layout the same way. Pure over two byte slices: no I/O, no serial,
//! fully testable without a board.

use crate::health::{self, Partition};

/// One region's fate. `Untouched` and `Wiped` are deliberately distinct from
/// `Identical`: "the image never reaches here" and "this is written back as
/// erased flash" are different promises, and collapsing them would let the
/// map claim a region survives when it is about to be cleared.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Verdict {
    Untouched,
    Identical,
    Wiped,
    Changed,
}

impl Verdict {
    pub fn as_str(self) -> &'static str {
        match self {
            Verdict::Untouched => "untouched",
            Verdict::Identical => "identical",
            Verdict::Wiped => "wiped",
            Verdict::Changed => "changed",
        }
    }
}

#[derive(Debug, Clone)]
pub struct Row {
    pub label: String,
    pub kind: String,
    pub offset: u32,
    pub size: u32,
    pub ptype: u8,
    pub subtype: u8,
    pub verdict: Verdict,
    pub changed_pct: Option<u32>,
    pub before: Option<String>,
    pub after: Option<String>,
}

#[derive(Debug)]
pub struct ChangeMap {
    pub layout_changed: bool,
    pub rows: Vec<Row>,
}

fn table_from(bytes: &[u8]) -> Vec<Partition> {
    let start = 0x8000usize;
    if bytes.len() < start + 32 {
        return Vec::new();
    }
    let end = bytes.len().min(0x8c00);
    health::parse_partition_table(&bytes[start..end])
}

fn region_all_ff(bytes: &[u8], off: usize, end: usize) -> bool {
    bytes[off..end].iter().all(|b| *b == 0xff)
}

fn count_diff(a: &[u8], b: &[u8], off: usize, end: usize) -> usize {
    (off..end).filter(|&i| a[i] != b[i]).count()
}

fn same_layout(a: &[Partition], b: &[Partition]) -> bool {
    a.len() == b.len()
        && a.iter().zip(b).all(|(x, y)| {
            x.ptype == y.ptype
                && x.subtype == y.subtype
                && x.offset == y.offset
                && x.size == y.size
                && x.label == y.label
        })
}

fn describe(
    old: &[u8],
    new: &[u8],
    e: &Partition,
    kind_override: Option<&str>,
    erase_all: bool,
) -> Row {
    let kind = kind_override
        .map(str::to_string)
        .unwrap_or_else(|| health::partition_kind(e.ptype, e.subtype));
    let label = if e.label.is_empty() {
        kind.clone()
    } else {
        e.label.clone()
    };
    let mut row = Row {
        label,
        kind,
        offset: e.offset,
        size: e.size,
        ptype: e.ptype,
        subtype: e.subtype,
        verdict: Verdict::Untouched,
        changed_pct: None,
        before: None,
        after: None,
    };
    // A first-contact install erases the WHOLE chip before writing, so a
    // region the image never reaches does not survive it — it comes back as
    // erased flash. Reporting those as "untouched" would be the exact
    // opposite of what happens, and precisely for the data that only lives
    // outside the image.
    let unreached = if erase_all {
        Verdict::Wiped
    } else {
        Verdict::Untouched
    };
    let off = e.offset as usize;
    if off >= new.len() {
        row.verdict = unreached;
        return row;
    }
    let cmp_end = (off + e.size as usize).min(new.len()).min(old.len());
    if cmp_end <= off {
        row.verdict = unreached;
        return row;
    }
    let diff = count_diff(old, new, off, cmp_end);
    if diff == 0 {
        row.verdict = Verdict::Identical;
        return row;
    }
    if region_all_ff(new, off, cmp_end) {
        row.verdict = Verdict::Wiped;
    } else {
        row.verdict = Verdict::Changed;
        let pct = (diff as f64 / (cmp_end - off) as f64 * 100.0).round() as u32;
        // A single changed byte in a megabyte rounds to 0%, which reads as
        // "nothing happened" next to the word "changed". Floor it at 1.
        row.changed_pct = Some(pct.max(1));
    }
    // App slots: name the change in firmware terms when both descriptors read.
    if e.ptype == 0x00 {
        let read = |bytes: &[u8]| -> Option<String> {
            let o = off + health::APP_DESC_OFFSET as usize;
            if o + 256 > bytes.len() {
                return None;
            }
            health::parse_app_descriptor(&bytes[o..o + 256])
                .map(|d| format!("{} {}", d.project_name, d.version).trim().to_string())
        };
        row.before = read(old);
        row.after = read(new);
    }
    row
}

/// Compare the board's current bytes against the image about to be written.
/// Returns `None` when neither side carries a partition table — with nothing
/// to anchor a map to, saying nothing beats inventing regions.
pub fn diff_install(old: &[u8], new: &[u8], erase_all: bool) -> Option<ChangeMap> {
    let new_pt = table_from(new);
    let old_pt = table_from(old);
    let table = if !new_pt.is_empty() { &new_pt } else { &old_pt };
    if table.is_empty() {
        return None;
    }
    let layout_changed =
        !new_pt.is_empty() && !old_pt.is_empty() && !same_layout(&new_pt, &old_pt);

    let mut rows = Vec::new();
    // The system area first: bootloader + the partition map itself. It sits
    // below the first partition and belongs to no entry, so nothing else
    // would ever report it.
    if let Some(first_off) = table.iter().map(|e| e.offset).min() {
        if first_off > 0 {
            rows.push(describe(
                old,
                new,
                &Partition {
                    ptype: 0xfe,
                    subtype: 0,
                    offset: 0,
                    size: first_off,
                    label: "system".into(),
                },
                Some("bootloader + partition map"),
                erase_all,
            ));
        }
    }
    for e in table {
        rows.push(describe(old, new, e, None, erase_all));
    }
    Some(ChangeMap {
        layout_changed,
        rows,
    })
}

/// The question people actually ask, answered from the diff.
///
/// Three outcomes, not two. When the user typed a network on the way in, the
/// flasher writes it into the replacement NVS before the image is staged — so
/// that region NECESSARILY differs from the backup, and reading that
/// difference as "your Wi-Fi is cleared, the board comes up on its setup
/// network" is precisely backwards: the board boots straight onto the network
/// they just entered. `baked_wifi` is what separates "replaced with what you
/// asked for" from "genuinely reset".
///
/// `had_wifi` (was there a network on the board before?) decides wording only.
pub fn settings_verdict(
    map: &ChangeMap,
    had_wifi: bool,
    baked_wifi: bool,
) -> Option<(bool, String)> {
    let nvs = map
        .rows
        .iter()
        .find(|r| r.ptype == 0x01 && r.subtype == 0x02)?;
    let kept = matches!(nvs.verdict, Verdict::Identical | Verdict::Untouched);
    if kept {
        return Some((
            true,
            if had_wifi {
                "Your settings survive — saved Wi-Fi, device identity and \
                 witness-chain counters stay exactly as they are."
                    .to_string()
            } else {
                "The settings area is untouched.".to_string()
            },
        ));
    }
    if baked_wifi {
        // Replaced, not lost. Reported as kept=false because the OLD contents
        // really are gone — the witness counters and any on-device tuning go
        // with them — but the headline must not claim the board is about to
        // ask for a network it was just given.
        return Some((
            false,
            "Your settings are replaced with the ones you entered here — the board \
             comes up already on that network. Anything it had stored itself \
             (its own tuning, witness-chain counters) starts fresh."
                .to_string(),
        ));
    }
    Some((
        false,
        if had_wifi {
            "Your settings are reset — the saved Wi-Fi is cleared, so the board \
             comes up with its setup network again, like the first day."
                .to_string()
        } else {
            "The settings area is reset to factory-fresh.".to_string()
        },
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    // A minimal image: partition table at 0x8000 with an nvs and an app slot.
    fn image(nvs_byte: u8, app_byte: u8, len: usize) -> Vec<u8> {
        let mut img = vec![0xff; len];
        let mut write_entry = |i: usize, ptype: u8, subtype: u8, off: u32, size: u32, label: &str| {
            let base = 0x8000 + i * 32;
            img[base] = 0xaa;
            img[base + 1] = 0x50;
            img[base + 2] = ptype;
            img[base + 3] = subtype;
            img[base + 4..base + 8].copy_from_slice(&off.to_le_bytes());
            img[base + 8..base + 12].copy_from_slice(&size.to_le_bytes());
            for (slot, b) in img[base + 12..base + 28].iter_mut().zip(label.bytes()) {
                *slot = b;
            }
        };
        write_entry(0, 0x01, 0x02, 0x9000, 0x1000, "nvs");
        write_entry(1, 0x00, 0x00, 0x10000, 0x1000, "factory");
        for b in img[0x9000..0xa000].iter_mut() {
            *b = nvs_byte;
        }
        for b in img[0x10000..0x11000].iter_mut() {
            *b = app_byte;
        }
        img
    }

    #[test]
    fn identical_settings_region_means_settings_survive() {
        let old = image(0x11, 0x22, 0x20000);
        let new = image(0x11, 0x33, 0x20000); // app differs, nvs identical
        let map = diff_install(&old, &new, false).unwrap();
        let nvs = map.rows.iter().find(|r| r.label == "nvs").unwrap();
        assert_eq!(nvs.verdict, Verdict::Identical);
        let (kept, text) = settings_verdict(&map, true, false).unwrap();
        assert!(kept);
        assert!(text.contains("survive"));
        // …and the app slot is correctly reported as changed.
        let app = map.rows.iter().find(|r| r.label == "factory").unwrap();
        assert_eq!(app.verdict, Verdict::Changed);
    }

    #[test]
    fn an_erased_settings_region_is_wiped_not_merely_changed() {
        // The distinction that matters: a new image writing 0xff over NVS
        // resets the board to its setup network. Calling that "changed" would
        // bury the one consequence the user needs to know about.
        let old = image(0x11, 0x22, 0x20000);
        let mut new = image(0x11, 0x22, 0x20000);
        for b in new[0x9000..0xa000].iter_mut() {
            *b = 0xff;
        }
        let map = diff_install(&old, &new, false).unwrap();
        let nvs = map.rows.iter().find(|r| r.label == "nvs").unwrap();
        assert_eq!(nvs.verdict, Verdict::Wiped);
        let (kept, text) = settings_verdict(&map, true, false).unwrap();
        assert!(!kept);
        assert!(text.contains("setup network"));
    }

    #[test]
    fn a_region_the_image_never_reaches_is_untouched() {
        // A short image (a factory bin that stops before a late partition)
        // leaves everything past its end alone — that is a promise the map
        // must make, not a comparison it should skip silently.
        let old = image(0x11, 0x22, 0x20000);
        let mut new = image(0x11, 0x22, 0x20000);
        new.truncate(0x10500); // a factory bin that stops inside the app slot
        let map = diff_install(&old, &new, false).unwrap();
        let app = map.rows.iter().find(|r| r.label == "factory").unwrap();
        assert!(matches!(app.verdict, Verdict::Identical | Verdict::Changed));
        // Nothing panics on the truncated compare — the real regression risk.
        assert!(map.rows.iter().any(|r| r.label == "nvs"));
    }

    #[test]
    fn a_changed_byte_never_rounds_away_to_zero_percent() {
        let old = image(0x11, 0x22, 0x20000);
        let mut new = image(0x11, 0x22, 0x20000);
        new[0x10000] ^= 0xff; // exactly one byte in a 4 KB slot
        let map = diff_install(&old, &new, false).unwrap();
        let app = map.rows.iter().find(|r| r.label == "factory").unwrap();
        assert_eq!(app.verdict, Verdict::Changed);
        assert_eq!(app.changed_pct, Some(1), "0% next to 'changed' reads as nothing happened");
    }

    #[test]
    fn baked_wifi_is_replaced_not_cleared() {
        // The flasher writes the user's network into the replacement NVS
        // BEFORE the image is staged, so this region always differs. Reading
        // that as "your Wi-Fi is cleared, the board wants its setup network"
        // is exactly backwards -- it boots onto the network they just typed.
        let old = image(0x11, 0x22, 0x20000);
        let mut new = image(0x11, 0x22, 0x20000);
        for b in new[0x9000..0xa000].iter_mut() {
            *b = 0x77; // provisioned NVS: differs, and is not erased flash
        }
        let map = diff_install(&old, &new, false).unwrap();
        let (kept, text) = settings_verdict(&map, true, true).unwrap();
        assert!(!kept, "the old contents really are gone");
        assert!(text.contains("replaced"), "got: {text}");
        assert!(!text.contains("setup network"),
            "must not claim the board will ask for a network it was just given");

        // Same bytes, but nothing baked in: now it IS a reset.
        let (kept, text) = settings_verdict(&map, true, false).unwrap();
        assert!(!kept);
        assert!(text.contains("setup network"), "got: {text}");
    }

    #[test]
    fn a_first_contact_erase_wipes_what_the_image_never_reaches() {
        // erase_first wipes the WHOLE chip before writing, so a region beyond
        // the image does not survive -- calling it "untouched" is the exact
        // opposite of what happens, for precisely the data that lives only
        // outside the image.
        let old = image(0x11, 0x22, 0x20000);
        let mut new = image(0x11, 0x22, 0x20000);
        new.truncate(0x10000); // stops before the app slot

        let plain = diff_install(&old, &new, false).unwrap();
        let app = plain.rows.iter().find(|r| r.label == "factory").unwrap();
        assert_eq!(app.verdict, Verdict::Untouched);

        let erased = diff_install(&old, &new, true).unwrap();
        let app = erased.rows.iter().find(|r| r.label == "factory").unwrap();
        assert_eq!(app.verdict, Verdict::Wiped);
    }

    #[test]
    fn no_partition_table_anywhere_yields_no_map() {
        let blank = vec![0xff; 0x20000];
        assert!(diff_install(&blank, &blank, false).is_none());
    }

    #[test]
    fn the_system_area_below_the_first_partition_is_reported() {
        let old = image(0x11, 0x22, 0x20000);
        let mut new = image(0x11, 0x22, 0x20000);
        new[0x1000] ^= 0xff; // bootloader area
        let map = diff_install(&old, &new, false).unwrap();
        let sys = map.rows.first().unwrap();
        assert_eq!(sys.label, "system");
        assert_eq!(sys.verdict, Verdict::Changed);
    }
}
