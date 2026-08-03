//! hub_enumerate_macos — the macOS half of disk enumeration: `diskutil`'s
//! plist output, turned into classified hub targets, safely.
//!
//! Same shape as the Linux enumerator (`hub_enumerate`): all the *reasoning* is
//! pure functions over already-captured `diskutil … -plist` XML, host-tested on
//! any runner without a Mac in sight. Only the thin `enumerate()` wrapper (built
//! for macOS alone) actually runs `diskutil`, and it hands every byte it reads
//! to these tested functions. It never decides eligibility itself — it builds
//! [`crate::hub_disk::TargetDisk`]s and the caller runs them through
//! `hub_disk::classify`.
//!
//! The safety-critical judgments, and where macOS hides them:
//!
//! - *which disk backs the running OS*: on APFS the root volume lives on a
//!   **synthesized** disk (e.g. `disk3s1`) whose real storage is named by
//!   `APFSPhysicalStores` in `diskutil info -plist /` — following that pointer
//!   to the physical whole disk (`disk0s2` → `disk0`) is the difference between
//!   refusing the operator's boot SSD and offering it;
//! - *which non-removable disks are external*: `diskutil info`'s `Internal`
//!   flag. Only an explicit `Internal = false` counts as external — a missing
//!   flag leaves the disk internal (→ refused), the safe direction;
//! - *physical vs synthesized*: only `diskutil list -plist physical` names
//!   candidate whole disks, and `VirtualOrPhysical` is re-checked per disk, so
//!   an APFS container device can never be offered as a raw write target.
//!
//! The plist parser here is deliberately minimal: the handful of element kinds
//! `diskutil` emits, parsed conservatively — anything unexpected is a loud
//! parse error, never a guessed value. No dependency, same as the rest of the
//! crate, so PR CI keeps testing this logic even though the desktop app only
//! builds on release tags.

use crate::hub_disk::TargetDisk;

// ── a minimal plist reader (exactly what diskutil emits) ────────────────────

/// One parsed plist value. `Other` keeps the raw text of kinds we never act on
/// (`real`, `date`, `data`) so parsing doesn't fail on them, but nothing here
/// will ever interpret one as a size, a flag, or a name.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Plist {
    Dict(Vec<(String, Plist)>),
    Array(Vec<Plist>),
    String(String),
    Integer(i64),
    Bool(bool),
    Other(String),
}

impl Plist {
    /// Dict lookup by key; `None` on anything that isn't a dict.
    pub fn get(&self, key: &str) -> Option<&Plist> {
        match self {
            Plist::Dict(pairs) => pairs.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Plist::String(s) => Some(s),
            _ => None,
        }
    }
    pub fn as_u64(&self) -> Option<u64> {
        match self {
            Plist::Integer(i) => u64::try_from(*i).ok(),
            _ => None,
        }
    }
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Plist::Bool(b) => Some(*b),
            _ => None,
        }
    }
    pub fn as_array(&self) -> Option<&[Plist]> {
        match self {
            Plist::Array(items) => Some(items),
            _ => None,
        }
    }
}

/// Parse a `diskutil … -plist` document into its root value. Conservative on
/// purpose: malformed XML, an element kind we don't know, or a missing root is
/// an `Err` — the enumerator then reports "couldn't read the disks" instead of
/// acting on a half-parsed picture.
pub fn parse_plist(xml: &str) -> Result<Plist, String> {
    let mut p = Parser { s: xml, pos: 0 };
    p.skip_prolog();
    // The document root is <plist …> wrapping exactly one value.
    let (name, self_closing) = p.open_tag()?;
    if name != "plist" {
        return Err(format!("expected <plist>, found <{name}>"));
    }
    if self_closing {
        return Err("empty <plist/> document".to_string());
    }
    let value = p.parse_value()?;
    p.close_tag("plist")?;
    Ok(value)
}

struct Parser<'a> {
    s: &'a str,
    pos: usize,
}

impl<'a> Parser<'a> {
    fn rest(&self) -> &'a str {
        &self.s[self.pos..]
    }

    fn skip_ws(&mut self) {
        let trimmed = self.rest().trim_start();
        self.pos = self.s.len() - trimmed.len();
    }

    /// Skip the XML declaration, DOCTYPE, and any comments before/between tags.
    fn skip_prolog(&mut self) {
        loop {
            self.skip_ws();
            let r = self.rest();
            if r.starts_with("<?") {
                match r.find("?>") {
                    Some(i) => self.pos += i + 2,
                    None => return,
                }
            } else if r.starts_with("<!--") {
                match r.find("-->") {
                    Some(i) => self.pos += i + 3,
                    None => return,
                }
            } else if r.starts_with("<!") {
                match r.find('>') {
                    Some(i) => self.pos += i + 1,
                    None => return,
                }
            } else {
                return;
            }
        }
    }

    /// Consume an opening tag, returning (name, was_self_closing).
    fn open_tag(&mut self) -> Result<(String, bool), String> {
        self.skip_prolog();
        let r = self.rest();
        if !r.starts_with('<') {
            return Err(format!("expected a tag at …{:?}", head(r)));
        }
        let end = r.find('>').ok_or("unterminated tag")?;
        let inner = &r[1..end];
        let self_closing = inner.ends_with('/');
        let inner = inner.strip_suffix('/').unwrap_or(inner).trim();
        // Tag attributes (only <plist version="1.0"> has any) are irrelevant.
        let name = inner.split_whitespace().next().unwrap_or("").to_string();
        if name.is_empty() || name.starts_with('/') {
            return Err(format!("expected an opening tag at …{:?}", head(r)));
        }
        self.pos += end + 1;
        Ok((name, self_closing))
    }

    /// Consume exactly `</name>`.
    fn close_tag(&mut self, name: &str) -> Result<(), String> {
        self.skip_ws();
        let want = format!("</{name}>");
        if self.rest().starts_with(&want) {
            self.pos += want.len();
            Ok(())
        } else {
            Err(format!("expected {want} at …{:?}", head(self.rest())))
        }
    }

    /// Text content up to the element's closing tag, entity-decoded.
    fn text_until_close(&mut self, name: &str) -> Result<String, String> {
        let want = format!("</{name}>");
        let r = self.rest();
        let i = r
            .find(&want)
            .ok_or_else(|| format!("unterminated <{name}>"))?;
        let text = decode_entities(&r[..i]);
        self.pos += i + want.len();
        Ok(text)
    }

    fn parse_value(&mut self) -> Result<Plist, String> {
        let (name, self_closing) = self.open_tag()?;
        match (name.as_str(), self_closing) {
            ("true", true) => Ok(Plist::Bool(true)),
            ("false", true) => Ok(Plist::Bool(false)),
            // Self-closed empties: <string/>, <dict/>, <array/>, <data/>…
            ("string", true) => Ok(Plist::String(String::new())),
            ("dict", true) => Ok(Plist::Dict(Vec::new())),
            ("array", true) => Ok(Plist::Array(Vec::new())),
            ("integer", true) => Err("empty <integer/> has no value".to_string()),
            (_, true) => Ok(Plist::Other(String::new())),
            ("string", false) => Ok(Plist::String(self.text_until_close("string")?)),
            ("integer", false) => {
                let t = self.text_until_close("integer")?;
                t.trim()
                    .parse::<i64>()
                    .map(Plist::Integer)
                    .map_err(|_| format!("bad <integer> value {t:?}"))
            }
            ("dict", false) => {
                let mut pairs = Vec::new();
                loop {
                    self.skip_ws();
                    if self.rest().starts_with("</dict>") {
                        self.pos += "</dict>".len();
                        return Ok(Plist::Dict(pairs));
                    }
                    let (kname, self_closed) = self.open_tag()?;
                    if kname != "key" {
                        return Err(format!("expected <key> in dict, found <{kname}>"));
                    }
                    let key = if self_closed {
                        String::new()
                    } else {
                        self.text_until_close("key")?
                    };
                    let value = self.parse_value()?;
                    pairs.push((key, value));
                }
            }
            ("array", false) => {
                let mut items = Vec::new();
                loop {
                    self.skip_ws();
                    if self.rest().starts_with("</array>") {
                        self.pos += "</array>".len();
                        return Ok(Plist::Array(items));
                    }
                    items.push(self.parse_value()?);
                }
            }
            // Kinds we keep but never interpret.
            ("real", false) | ("date", false) | ("data", false) => {
                Ok(Plist::Other(self.text_until_close(&name)?))
            }
            (other, false) => Err(format!("unsupported plist element <{other}>")),
        }
    }
}

/// A short excerpt for error messages, so a parse failure names where.
fn head(s: &str) -> &str {
    &s[..s.len().min(40)]
}

/// The five XML entities plist emits. Anything else passes through verbatim —
/// this feeds display strings and device names, never security decisions.
fn decode_entities(s: &str) -> String {
    s.replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&apos;", "'")
        .replace("&amp;", "&")
        .trim()
        .to_string()
}

// ── pure judgments over diskutil's answers ─────────────────────────────────

/// Map any macOS device node to its whole disk:
///
/// ```text
///   disk0s2      -> disk0       /dev/disk3s1s1 -> disk3
///   disk4        -> disk4       rdisk2s1       -> disk2
/// ```
///
/// The whole disk is `disk<N>`; everything after (`s2`, `s1s1` for a sealed
/// snapshot) is slice structure. A name that isn't `disk<N>…` is returned
/// unchanged — never guessed at.
pub fn parent_disk(dev: &str) -> String {
    let d = dev.strip_prefix("/dev/").unwrap_or(dev);
    let d = d
        .strip_prefix('r')
        .filter(|r| r.starts_with("disk"))
        .unwrap_or(d);
    match d.strip_prefix("disk") {
        Some(tail) => {
            let digits: String = tail.chars().take_while(|c| c.is_ascii_digit()).collect();
            if digits.is_empty() {
                d.to_string()
            } else {
                format!("disk{digits}")
            }
        }
        None => d.to_string(),
    }
}

/// The candidate whole-disk names from `diskutil list -plist physical` — the
/// listing that already excludes APFS synthesized (virtual) disks.
pub fn whole_disks(list: &Plist) -> Vec<String> {
    let mut out: Vec<String> = list
        .get("WholeDisks")
        .and_then(Plist::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(Plist::as_str)
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default();
    out.sort();
    out.dedup();
    out
}

/// The physical whole disks backing the running OS, from `diskutil info
/// -plist /`. On APFS the root's device is a synthesized disk; its real
/// storage is listed in `APFSPhysicalStores` (each entry naming e.g.
/// `disk0s2`). Older HFS+ roots sit directly on the physical disk, covered by
/// `ParentWholeDisk` / `DeviceIdentifier`. All three are collected — an extra
/// (virtual) name is harmless because it matches no physical candidate, while a
/// missed one could offer the boot disk. Deduplicated and sorted.
pub fn system_whole_disks(root_info: &Plist) -> Vec<String> {
    let mut out = Vec::new();
    if let Some(stores) = root_info
        .get("APFSPhysicalStores")
        .and_then(Plist::as_array)
    {
        for store in stores {
            // Seen as dicts keyed APFSPhysicalStore (macOS 11+) or
            // DeviceIdentifier; tolerate a bare string too.
            let dev = store
                .get("APFSPhysicalStore")
                .or_else(|| store.get("DeviceIdentifier"))
                .and_then(Plist::as_str)
                .or_else(|| store.as_str());
            if let Some(dev) = dev {
                out.push(parent_disk(dev));
            }
        }
    }
    for key in ["ParentWholeDisk", "DeviceIdentifier"] {
        if let Some(dev) = root_info.get(key).and_then(Plist::as_str) {
            out.push(parent_disk(dev));
        }
    }
    out.sort();
    out.dedup();
    out
}

/// Is this `diskutil info -plist <disk>` describing a physical device? An APFS
/// synthesized container says `Virtual`. Conservative: only an explicit
/// `Physical` (or a pre-APFS output with no such key) passes — anything else is
/// not a raw write target.
pub fn is_physical(info: &Plist) -> bool {
    match info.get("VirtualOrPhysical").and_then(Plist::as_str) {
        Some(v) => v == "Physical",
        None => true,
    }
}

/// Does the full `diskutil list -plist` show anything of `physical_disk`
/// mounted right now? Direct partitions carry their own `MountPoint`; volumes
/// of an APFS container count when the container's physical stores sit on this
/// disk. Advisory only (feeds the "this has data on it" warning, not a
/// refusal), so a shape we don't recognize reads as unmounted rather than
/// failing enumeration.
pub fn disk_has_mounts(list: &Plist, physical_disk: &str) -> bool {
    let Some(entries) = list.get("AllDisksAndPartitions").and_then(Plist::as_array) else {
        return false;
    };
    let mounted = |vols: Option<&Plist>| {
        vols.and_then(Plist::as_array).is_some_and(|vs| {
            vs.iter().any(|v| {
                v.get("MountPoint")
                    .and_then(Plist::as_str)
                    .is_some_and(|m| !m.is_empty())
            })
        })
    };
    for entry in entries {
        let dev = entry.get("DeviceIdentifier").and_then(Plist::as_str);
        if dev == Some(physical_disk) {
            if mounted(entry.get("Partitions")) || mounted(entry.get("APFSVolumes")) {
                return true;
            }
            // The rare whole-disk filesystem (no partition table).
            if entry
                .get("MountPoint")
                .and_then(Plist::as_str)
                .is_some_and(|m| !m.is_empty())
            {
                return true;
            }
        }
        // A synthesized container whose backing store is on this disk: its
        // mounted volumes are this disk's data.
        if let Some(stores) = entry.get("APFSPhysicalStores").and_then(Plist::as_array) {
            let backs_this = stores.iter().any(|s| {
                s.get("DeviceIdentifier")
                    .or_else(|| s.get("APFSPhysicalStore"))
                    .and_then(Plist::as_str)
                    .or_else(|| s.as_str())
                    .is_some_and(|d| parent_disk(d) == physical_disk)
            });
            if backs_this && (mounted(entry.get("APFSVolumes")) || mounted(entry.get("Partitions")))
            {
                return true;
            }
        }
    }
    false
}

/// Turn one disk's `diskutil info -plist` into a [`TargetDisk`] observation.
/// Pure and conservative, field by field:
///
/// - size: `TotalSize` in bytes (`Size` as the older fallback); missing or
///   nonsensical reads as 0, which `classify` refuses as unknown-size;
/// - removable: `RemovableMedia` (or the older `Removable`) — SD cards and
///   USB sticks;
/// - external: ONLY an explicit `Internal = false`. A missing flag leaves the
///   disk internal → refused, so the failure mode of a strange output is a
///   hidden candidate, never an offered internal disk;
/// - model: `MediaName`, falling back to `IORegistryEntryName`, then the
///   device name — display only.
pub fn from_diskutil_info(name: &str, info: &Plist, system: bool, has_mounts: bool) -> TargetDisk {
    let size_bytes = info
        .get("TotalSize")
        .or_else(|| info.get("Size"))
        .and_then(Plist::as_u64)
        .unwrap_or(0);
    let removable = info
        .get("RemovableMedia")
        .or_else(|| info.get("Removable"))
        .and_then(Plist::as_bool)
        .unwrap_or(false);
    let external = info.get("Internal").and_then(Plist::as_bool) == Some(false);
    let model = info
        .get("MediaName")
        .or_else(|| info.get("IORegistryEntryName"))
        .and_then(Plist::as_str)
        .map(str::trim)
        .filter(|m| !m.is_empty())
        .unwrap_or(name)
        .to_string();
    TargetDisk {
        path: format!("/dev/{name}"),
        model,
        size_bytes,
        removable,
        external,
        system,
        has_mounts,
    }
}

/// Enumerate this Mac's disks as hub-target candidates. Read-only: four
/// `diskutil` reads (`list -plist physical`, `list -plist`, `info -plist /`,
/// and `info -plist <disk>` per candidate), every byte handed to the pure
/// functions above. Makes no eligibility decision — the caller runs the result
/// through `hub_disk::classify`.
#[cfg(target_os = "macos")]
pub fn enumerate() -> Result<Vec<TargetDisk>, String> {
    fn diskutil(args: &[&str]) -> Result<String, String> {
        let out = std::process::Command::new("diskutil")
            .args(args)
            .output()
            .map_err(|e| format!("couldn't run diskutil: {e}"))?;
        if !out.status.success() {
            return Err(format!(
                "diskutil {} failed: {}",
                args.join(" "),
                out.status
            ));
        }
        String::from_utf8(out.stdout).map_err(|e| format!("diskutil output wasn't UTF-8: {e}"))
    }

    let physical = parse_plist(&diskutil(&["list", "-plist", "physical"])?)?;
    let full = parse_plist(&diskutil(&["list", "-plist"])?)?;
    let root = parse_plist(&diskutil(&["info", "-plist", "/"])?)?;
    let system_disks = system_whole_disks(&root);

    let mut out = Vec::new();
    for name in whole_disks(&physical) {
        let info = parse_plist(&diskutil(&["info", "-plist", &name])?)?;
        if !is_physical(&info) {
            continue; // belt-and-suspenders; `physical` shouldn't list these
        }
        let system = system_disks.iter().any(|d| d == &name);
        let has_mounts = disk_has_mounts(&full, &name);
        out.push(from_diskutil_info(&name, &info, system, has_mounts));
    }
    out.sort_by(|a, b| a.path.cmp(&b.path));
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hub_disk::{classify, Eligibility, Refusal};

    // Fixtures shaped exactly like diskutil's output (element order, the
    // envelope, entity escapes) so the parser is tested against the real
    // grammar, not a convenient one.

    /// `diskutil list -plist physical` on a Mac with one internal SSD, a USB
    /// SD-card reader with a card in it, and a Samsung T7 external SSD.
    const LIST_PHYSICAL: &str = r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>AllDisks</key>
	<array>
		<string>disk0</string>
		<string>disk0s1</string>
		<string>disk0s2</string>
		<string>disk4</string>
		<string>disk4s1</string>
		<string>disk5</string>
		<string>disk5s1</string>
		<string>disk5s2</string>
	</array>
	<key>WholeDisks</key>
	<array>
		<string>disk0</string>
		<string>disk4</string>
		<string>disk5</string>
	</array>
</dict>
</plist>"#;

    /// The full `diskutil list -plist` for the same machine: adds the
    /// synthesized APFS container disk3 (backed by disk0s2) carrying the
    /// mounted system volumes, and shows disk4's FAT partition mounted.
    const LIST_FULL: &str = r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>AllDisksAndPartitions</key>
	<array>
		<dict>
			<key>DeviceIdentifier</key>
			<string>disk0</string>
			<key>Partitions</key>
			<array>
				<dict>
					<key>DeviceIdentifier</key>
					<string>disk0s1</string>
					<key>Content</key>
					<string>Apple_APFS_ISC</string>
				</dict>
				<dict>
					<key>DeviceIdentifier</key>
					<string>disk0s2</string>
					<key>Content</key>
					<string>Apple_APFS</string>
				</dict>
			</array>
		</dict>
		<dict>
			<key>DeviceIdentifier</key>
			<string>disk3</string>
			<key>APFSPhysicalStores</key>
			<array>
				<dict>
					<key>DeviceIdentifier</key>
					<string>disk0s2</string>
				</dict>
			</array>
			<key>APFSVolumes</key>
			<array>
				<dict>
					<key>DeviceIdentifier</key>
					<string>disk3s1</string>
					<key>MountPoint</key>
					<string>/System/Volumes/Data</string>
				</dict>
			</array>
		</dict>
		<dict>
			<key>DeviceIdentifier</key>
			<string>disk4</string>
			<key>Partitions</key>
			<array>
				<dict>
					<key>DeviceIdentifier</key>
					<string>disk4s1</string>
					<key>MountPoint</key>
					<string>/Volumes/UNTITLED</string>
				</dict>
			</array>
		</dict>
		<dict>
			<key>DeviceIdentifier</key>
			<string>disk5</string>
			<key>Partitions</key>
			<array>
				<dict>
					<key>DeviceIdentifier</key>
					<string>disk5s2</string>
				</dict>
			</array>
		</dict>
	</array>
</dict>
</plist>"#;

    /// `diskutil info -plist /` — the APFS root volume on synthesized disk3,
    /// physically stored on disk0s2.
    const INFO_ROOT: &str = r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>APFSPhysicalStores</key>
	<array>
		<dict>
			<key>APFSPhysicalStore</key>
			<string>disk0s2</string>
		</dict>
	</array>
	<key>DeviceIdentifier</key>
	<string>disk3s1s1</string>
	<key>MountPoint</key>
	<string>/</string>
	<key>ParentWholeDisk</key>
	<string>disk3</string>
	<key>VirtualOrPhysical</key>
	<string>Virtual</string>
</dict>
</plist>"#;

    /// `diskutil info -plist disk0` — the internal Apple SSD.
    const INFO_DISK0: &str = r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>DeviceIdentifier</key>
	<string>disk0</string>
	<key>Internal</key>
	<true/>
	<key>MediaName</key>
	<string>APPLE SSD AP0512Z</string>
	<key>RemovableMedia</key>
	<false/>
	<key>TotalSize</key>
	<integer>494384795648</integer>
	<key>VirtualOrPhysical</key>
	<string>Physical</string>
</dict>
</plist>"#;

    /// `diskutil info -plist disk4` — a 64 GB SD card in a USB reader.
    const INFO_DISK4: &str = r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>DeviceIdentifier</key>
	<string>disk4</string>
	<key>Internal</key>
	<false/>
	<key>MediaName</key>
	<string>SD Card Reader &amp; Writer</string>
	<key>RemovableMedia</key>
	<true/>
	<key>TotalSize</key>
	<integer>63864569856</integer>
	<key>VirtualOrPhysical</key>
	<string>Physical</string>
</dict>
</plist>"#;

    /// `diskutil info -plist disk5` — a Samsung T7 external SSD: NOT
    /// removable, but explicitly not internal (the Pi 5 durable default).
    const INFO_DISK5: &str = r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>DeviceIdentifier</key>
	<string>disk5</string>
	<key>Internal</key>
	<false/>
	<key>MediaName</key>
	<string>Samsung Portable SSD T7 Media</string>
	<key>RemovableMedia</key>
	<false/>
	<key>TotalSize</key>
	<integer>500107862016</integer>
	<key>VirtualOrPhysical</key>
	<string>Physical</string>
</dict>
</plist>"#;

    #[test]
    fn parses_the_physical_listing() {
        let list = parse_plist(LIST_PHYSICAL).expect("parses");
        assert_eq!(whole_disks(&list), vec!["disk0", "disk4", "disk5"]);
    }

    #[test]
    fn parser_handles_entities_booleans_and_integers() {
        let info = parse_plist(INFO_DISK4).expect("parses");
        assert_eq!(
            info.get("MediaName").and_then(Plist::as_str),
            Some("SD Card Reader & Writer")
        );
        assert_eq!(
            info.get("RemovableMedia").and_then(Plist::as_bool),
            Some(true)
        );
        assert_eq!(
            info.get("TotalSize").and_then(Plist::as_u64),
            Some(63_864_569_856)
        );
    }

    #[test]
    fn parser_refuses_what_it_does_not_know() {
        assert!(parse_plist("<plist><wat>1</wat></plist>").is_err());
        assert!(parse_plist("<plist><dict><key>K</key>").is_err());
        assert!(parse_plist("<plist><integer>NaN</integer></plist>").is_err());
        assert!(parse_plist("not xml at all").is_err());
    }

    #[test]
    fn parent_disk_walks_slices_and_snapshots_back_to_the_whole_disk() {
        assert_eq!(parent_disk("disk0s2"), "disk0");
        assert_eq!(parent_disk("disk3s1s1"), "disk3"); // sealed system snapshot
        assert_eq!(parent_disk("disk4"), "disk4");
        assert_eq!(parent_disk("/dev/disk10s1"), "disk10");
        assert_eq!(parent_disk("rdisk2s1"), "disk2");
        assert_eq!(parent_disk("sda1"), "sda1"); // not a macOS name — untouched
    }

    #[test]
    fn the_apfs_root_resolves_to_its_physical_store_disk() {
        let root = parse_plist(INFO_ROOT).expect("parses");
        // The naive answer (ParentWholeDisk) is the synthesized disk3; the
        // safety-critical answer is the physical disk0 behind it. Both are
        // collected — disk3 matches no physical candidate, disk0 refuses the
        // boot SSD.
        assert_eq!(system_whole_disks(&root), vec!["disk0", "disk3"]);
    }

    #[test]
    fn virtual_disks_are_not_physical() {
        assert!(!is_physical(&parse_plist(INFO_ROOT).unwrap()));
        assert!(is_physical(&parse_plist(INFO_DISK0).unwrap()));
        // Pre-APFS output without the key: treated as physical.
        assert!(is_physical(
            &parse_plist("<plist><dict></dict></plist>").unwrap()
        ));
    }

    #[test]
    fn mounts_are_seen_directly_and_through_an_apfs_container() {
        let full = parse_plist(LIST_FULL).expect("parses");
        // disk4's FAT partition is mounted at /Volumes/UNTITLED.
        assert!(disk_has_mounts(&full, "disk4"));
        // disk0 has no directly-mounted partition, but the synthesized disk3
        // container it backs has mounted volumes — that's disk0's data.
        assert!(disk_has_mounts(&full, "disk0"));
        // disk5's partition carries no MountPoint.
        assert!(!disk_has_mounts(&full, "disk5"));
    }

    // The payoff: the three disks compose with hub_disk::classify to exactly
    // the verdicts the picker must show.

    fn observed(name: &str, info_xml: &str) -> TargetDisk {
        let root = parse_plist(INFO_ROOT).unwrap();
        let full = parse_plist(LIST_FULL).unwrap();
        let info = parse_plist(info_xml).unwrap();
        let system = system_whole_disks(&root).iter().any(|d| d == name);
        from_diskutil_info(name, &info, system, disk_has_mounts(&full, name))
    }

    #[test]
    fn the_boot_ssd_is_refused_as_the_system_disk() {
        let d = observed("disk0", INFO_DISK0);
        assert!(
            d.system,
            "disk0 must be flagged system via APFSPhysicalStores"
        );
        assert_eq!(classify(&d), Eligibility::Refused(Refusal::SystemDisk));
    }

    #[test]
    fn the_sd_card_is_an_eligible_target_with_its_data_warned() {
        let d = observed("disk4", INFO_DISK4);
        assert_eq!(d.path, "/dev/disk4");
        assert_eq!(d.model, "SD Card Reader & Writer");
        assert!(d.removable && d.external && !d.system);
        match classify(&d) {
            Eligibility::Eligible { warnings } => {
                // Mounted (auto-mounted FAT volume) and just under the 64 GB
                // recommendation — both advisories, neither a veto.
                assert!(!warnings.is_empty());
            }
            other => panic!("expected eligible, got {other:?}"),
        }
    }

    #[test]
    fn the_external_t7_is_eligible_though_not_removable() {
        let d = observed("disk5", INFO_DISK5);
        assert!(!d.removable && d.external);
        assert!(classify(&d).is_eligible());
    }

    #[test]
    fn a_missing_internal_flag_leaves_a_disk_internal_and_refused() {
        // Strange output: no Internal key, not removable → conservative
        // internal fixed disk, hidden from the picker.
        let xml = r#"<plist><dict>
            <key>TotalSize</key><integer>500107862016</integer>
            <key>RemovableMedia</key><false/>
        </dict></plist>"#;
        let info = parse_plist(xml).unwrap();
        let d = from_diskutil_info("disk9", &info, false, false);
        assert!(!d.external);
        assert_eq!(
            classify(&d),
            Eligibility::Refused(Refusal::InternalFixedDisk)
        );
    }

    #[test]
    fn a_missing_size_reads_as_zero_and_is_refused() {
        let info = parse_plist("<plist><dict><key>Internal</key><false/></dict></plist>").unwrap();
        let d = from_diskutil_info("disk9", &info, false, false);
        assert_eq!(d.size_bytes, 0);
        assert_eq!(classify(&d), Eligibility::Refused(Refusal::UnknownSize));
    }

    #[test]
    fn model_falls_back_to_the_device_name() {
        let info =
            parse_plist("<plist><dict><key>MediaName</key><string>  </string></dict></plist>")
                .unwrap();
        assert_eq!(
            from_diskutil_info("disk7", &info, false, false).model,
            "disk7"
        );
    }
}
