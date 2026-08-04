//! hub_enumerate_windows — the Windows half of disk enumeration: PowerShell
//! `Get-Disk`'s JSON, turned into classified hub targets, safely.
//!
//! Same shape as the Linux and macOS enumerators: every *judgment* is a pure
//! function over already-captured `Get-Disk … | ConvertTo-Json` text,
//! host-tested on any runner without a Windows box in sight. Only the thin
//! `enumerate()` wrapper (built for Windows alone) actually spawns PowerShell,
//! and it hands every byte it reads to these tested functions. It never decides
//! eligibility itself — it builds [`crate::hub_disk::TargetDisk`]s and the
//! caller runs them through `hub_disk::classify`.
//!
//! The safety-critical judgments, and where Windows keeps them:
//!
//! - *which disk backs the running OS*: `Get-Disk` reports `IsBoot` / `IsSystem`
//!   per disk, and the wrapper also asks which disk hosts the system drive
//!   (`$env:SystemDrive`, normally `C:`). A disk flagged by ANY of the three is
//!   `system` → refused. Belt-and-suspenders: on Windows the boot disk is
//!   effectively always an internal fixed disk (NVMe/SATA), so `classify`
//!   already refuses it as [`crate::hub_disk::Refusal::InternalFixedDisk`] even
//!   if every system signal were missing — the disks we actively OFFER are the
//!   USB/SD ones, which are never the Windows boot volume in a normal setup;
//! - *which non-removable disks are external*: the `BusType`. Only `USB` counts
//!   as external here — the same conservative stance as the Linux enumerator
//!   (USB/UAS only) and macOS (`Internal = false` only). A USB-SATA/Thunderbolt
//!   enclosure that reports its inner bus (`SATA`/`RAID`/`SCSI`) reads as
//!   internal → refused, which costs a card swap, never a wiped disk;
//! - *which disks are removable*: a `BusType` of `SD` or `MMC` — a built-in card
//!   reader on a real SD/MMC host controller. (Most laptop SD readers are
//!   internally USB-attached and so are already covered by `USB`/external.)
//!
//! The JSON reader here is deliberately minimal — exactly the value kinds
//! `ConvertTo-Json` emits, parsed conservatively: malformed input, an unknown
//! token, or trailing junk is a loud parse error, never a guessed value. It is
//! liberal in ONE place on purpose — the shape, not the values: Windows
//! PowerShell 5.1 collapses a single-element array to the bare element, so a
//! lone disk can arrive as an object instead of a one-element array, and a lone
//! drive letter as a string instead of a one-element array. Both are tolerated.
//! No dependency (no serde), same as the rest of the crate, so PR CI keeps
//! testing this logic even though the desktop app only builds on release tags.

use crate::hub_disk::TargetDisk;

// ── a minimal JSON reader (exactly what ConvertTo-Json emits) ────────────────

/// One parsed JSON value. Numbers keep their raw token so a 64-bit disk size
/// round-trips without any float precision loss — they are only ever read back
/// as integers ([`Json::as_u64`]).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Json {
    Object(Vec<(String, Json)>),
    Array(Vec<Json>),
    Str(String),
    /// The raw numeric token (e.g. `"1000204886016"`), parsed on demand.
    Number(String),
    Bool(bool),
    Null,
}

impl Json {
    /// Object lookup by key; `None` on anything that isn't an object.
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Object(pairs) => pairs.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }
    /// Read a non-negative integer. Accepts both a JSON number and a quoted
    /// number (a `"Size"` that arrived stringified), so the envelope never turns
    /// a real size into `None`. A value that isn't a clean `u64` reads as `None`
    /// — the caller then treats it as unknown size, which `classify` refuses.
    pub fn as_u64(&self) -> Option<u64> {
        match self {
            Json::Number(s) | Json::Str(s) => s.trim().parse::<u64>().ok(),
            _ => None,
        }
    }
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Json::Bool(b) => Some(*b),
            _ => None,
        }
    }
    pub fn as_array(&self) -> Option<&[Json]> {
        match self {
            Json::Array(items) => Some(items),
            _ => None,
        }
    }
}

/// Parse one `ConvertTo-Json` document into its root value. Conservative on
/// purpose: malformed JSON, an unknown token, or trailing junk after the root
/// value is an `Err` — the enumerator then reports "couldn't read the disks"
/// instead of acting on a half-parsed picture. A leading UTF-8 BOM (PowerShell
/// likes to emit one) is skipped.
pub fn parse_json(text: &str) -> Result<Json, String> {
    let text = text.strip_prefix('\u{feff}').unwrap_or(text);
    let mut p = Parser { s: text, pos: 0 };
    p.skip_ws();
    let value = p.parse_value()?;
    p.skip_ws();
    if p.pos != p.s.len() {
        return Err(format!(
            "trailing data after JSON value at …{:?}",
            head(p.rest())
        ));
    }
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

    fn peek(&self) -> Option<char> {
        self.rest().chars().next()
    }

    fn bump(&mut self, c: char) {
        self.pos += c.len_utf8();
    }

    fn skip_ws(&mut self) {
        let trimmed = self.rest().trim_start_matches([' ', '\t', '\n', '\r']);
        self.pos = self.s.len() - trimmed.len();
    }

    fn parse_value(&mut self) -> Result<Json, String> {
        self.skip_ws();
        match self.peek() {
            Some('{') => self.parse_object(),
            Some('[') => self.parse_array(),
            Some('"') => Ok(Json::Str(self.parse_string()?)),
            Some('t') => self.parse_lit("true", Json::Bool(true)),
            Some('f') => self.parse_lit("false", Json::Bool(false)),
            Some('n') => self.parse_lit("null", Json::Null),
            Some(c) if c == '-' || c.is_ascii_digit() => self.parse_number(),
            Some(c) => Err(format!(
                "unexpected character {c:?} at …{:?}",
                head(self.rest())
            )),
            None => Err("unexpected end of JSON".to_string()),
        }
    }

    fn parse_lit(&mut self, word: &str, value: Json) -> Result<Json, String> {
        if self.rest().starts_with(word) {
            self.pos += word.len();
            Ok(value)
        } else {
            Err(format!("expected `{word}` at …{:?}", head(self.rest())))
        }
    }

    /// A JSON number token: a maximal run of the characters a number can contain.
    /// We don't validate the grammar beyond that — the token is stored raw and
    /// only ever re-parsed as a `u64`, where a bad shape simply fails to a
    /// refused (unknown-size) disk.
    fn parse_number(&mut self) -> Result<Json, String> {
        let start = self.pos;
        while let Some(c) = self.peek() {
            if c.is_ascii_digit() || matches!(c, '-' | '+' | '.' | 'e' | 'E') {
                self.bump(c);
            } else {
                break;
            }
        }
        if self.pos == start {
            return Err("empty number".to_string());
        }
        Ok(Json::Number(self.s[start..self.pos].to_string()))
    }

    fn parse_string(&mut self) -> Result<String, String> {
        // Opening quote.
        match self.peek() {
            Some('"') => self.bump('"'),
            _ => return Err(format!("expected a string at …{:?}", head(self.rest()))),
        }
        let mut out = String::new();
        loop {
            let c = self.peek().ok_or("unterminated string")?;
            self.bump(c);
            match c {
                '"' => return Ok(out),
                '\\' => {
                    let e = self.peek().ok_or("unterminated escape")?;
                    self.bump(e);
                    match e {
                        '"' => out.push('"'),
                        '\\' => out.push('\\'),
                        '/' => out.push('/'),
                        'b' => out.push('\u{0008}'),
                        'f' => out.push('\u{000c}'),
                        'n' => out.push('\n'),
                        'r' => out.push('\r'),
                        't' => out.push('\t'),
                        'u' => out.push(self.parse_unicode_escape()?),
                        other => return Err(format!("bad escape \\{other}")),
                    }
                }
                _ => out.push(c),
            }
        }
    }

    /// A `\uXXXX` escape (the leading `\u` already consumed), combining a UTF-16
    /// surrogate pair when it sees one. These feed display strings (a disk's
    /// friendly name) only, so a lone/blank surrogate degrades to U+FFFD rather
    /// than failing the whole parse.
    fn parse_unicode_escape(&mut self) -> Result<char, String> {
        let hi = self.read_hex4()?;
        if (0xD800..=0xDBFF).contains(&hi) {
            if self.rest().starts_with("\\u") {
                self.pos += 2; // consume the second `\u`
                let lo = self.read_hex4()?;
                if (0xDC00..=0xDFFF).contains(&lo) {
                    let c = 0x1_0000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
                    return Ok(char::from_u32(c).unwrap_or('\u{FFFD}'));
                }
                return Ok(char::from_u32(lo).unwrap_or('\u{FFFD}'));
            }
            return Ok('\u{FFFD}');
        }
        Ok(char::from_u32(hi).unwrap_or('\u{FFFD}'))
    }

    fn read_hex4(&mut self) -> Result<u32, String> {
        let r = self.rest();
        let hex: String = r.chars().take(4).collect();
        if hex.len() == 4 {
            if let Ok(v) = u32::from_str_radix(&hex, 16) {
                self.pos += 4;
                return Ok(v);
            }
        }
        Err(format!("bad \\u escape at …{:?}", head(r)))
    }

    fn parse_object(&mut self) -> Result<Json, String> {
        self.bump('{');
        let mut pairs = Vec::new();
        self.skip_ws();
        if self.peek() == Some('}') {
            self.bump('}');
            return Ok(Json::Object(pairs));
        }
        loop {
            self.skip_ws();
            let key = self.parse_string()?;
            self.skip_ws();
            match self.peek() {
                Some(':') => self.bump(':'),
                _ => return Err(format!("expected ':' after key {key:?}")),
            }
            let value = self.parse_value()?;
            pairs.push((key, value));
            self.skip_ws();
            match self.peek() {
                Some(',') => {
                    self.bump(',');
                    continue;
                }
                Some('}') => {
                    self.bump('}');
                    return Ok(Json::Object(pairs));
                }
                other => return Err(format!("expected ',' or '}}' in object, found {other:?}")),
            }
        }
    }

    fn parse_array(&mut self) -> Result<Json, String> {
        self.bump('[');
        let mut items = Vec::new();
        self.skip_ws();
        if self.peek() == Some(']') {
            self.bump(']');
            return Ok(Json::Array(items));
        }
        loop {
            items.push(self.parse_value()?);
            self.skip_ws();
            match self.peek() {
                Some(',') => {
                    self.bump(',');
                    continue;
                }
                Some(']') => {
                    self.bump(']');
                    return Ok(Json::Array(items));
                }
                other => return Err(format!("expected ',' or ']' in array, found {other:?}")),
            }
        }
    }
}

/// A short excerpt for error messages, so a parse failure names where.
fn head(s: &str) -> &str {
    let end = s.char_indices().nth(40).map(|(i, _)| i).unwrap_or(s.len());
    &s[..end]
}

// ── pure judgments over Get-Disk's answers ─────────────────────────────────

/// The Windows raw-disk path for a physical drive number, e.g. `2` →
/// `\\.\PhysicalDrive2` — what the writer opens.
pub fn physical_drive_path(number: u64) -> String {
    format!(r"\\.\PhysicalDrive{number}")
}

/// A disk's `Number` from its `Get-Disk` object, or `None` if it's missing —
/// a disk we can't name a path for is unusable and the caller skips it.
pub fn disk_number(disk: &Json) -> Option<u64> {
    disk.get("Number").and_then(Json::as_u64)
}

/// Is this `BusType` external to the machine? Only `USB` — conservative, the
/// same direction the Linux (USB/UAS-only) and macOS (`Internal=false`-only)
/// enumerators lean. An enclosure that reports its inner bus reads as internal
/// and is refused rather than risked.
pub fn is_external(bus_type: &str) -> bool {
    bus_type.eq_ignore_ascii_case("USB")
}

/// Is this `BusType` a removable-media reader? `SD` / `MMC` — a genuine card
/// host controller. (USB card readers are covered by [`is_external`].)
pub fn is_removable(bus_type: &str) -> bool {
    ["SD", "MMC"]
        .iter()
        .any(|b| bus_type.eq_ignore_ascii_case(b))
}

/// Does this disk back the running OS? True if `Get-Disk` flags it `IsBoot` or
/// `IsSystem`, OR it is the disk hosting the system drive (`system_drive`, the
/// disk number behind `$env:SystemDrive`). Any one is enough — the signals are
/// OR-ed so a gap in one can't offer the boot disk. A missing flag reads as
/// `false`, which is safe here only because such a disk is also non-removable
/// and refused as an internal fixed disk regardless.
pub fn is_system(disk: &Json, system_drive: Option<u64>) -> bool {
    let is_boot = disk.get("IsBoot").and_then(Json::as_bool).unwrap_or(false);
    let is_system = disk
        .get("IsSystem")
        .and_then(Json::as_bool)
        .unwrap_or(false);
    let backs_system_drive = matches!(
        (disk_number(disk), system_drive),
        (Some(n), Some(s)) if n == s
    );
    is_boot || is_system || backs_system_drive
}

/// Does this disk currently have any accessible (drive-lettered) volume? Fed
/// into the advisory "this has data on it" warning, not a refusal. Tolerates the
/// PowerShell 5.1 single-element collapse: `DriveLetters` may be an array, a
/// lone string, or absent.
pub fn has_drive_letters(disk: &Json) -> bool {
    match disk.get("DriveLetters") {
        Some(Json::Array(items)) => items
            .iter()
            .any(|v| v.as_str().is_some_and(|s| !s.trim().is_empty())),
        Some(Json::Str(s)) => !s.trim().is_empty(),
        _ => false,
    }
}

/// Turn one `Get-Disk` object into a [`TargetDisk`] observation. Pure and
/// conservative, field by field:
///
/// - size: `Size` in bytes; missing / non-integer reads as 0, which `classify`
///   refuses as unknown-size;
/// - external / removable: from `BusType` per [`is_external`] / [`is_removable`];
/// - model: `FriendlyName`, falling back to `Disk <n>` — display only;
/// - `system` and `has_mounts` are decided by the caller and passed in.
pub fn from_disk_json(number: u64, disk: &Json, system: bool, has_mounts: bool) -> TargetDisk {
    let size_bytes = disk.get("Size").and_then(Json::as_u64).unwrap_or(0);
    let bus = disk.get("BusType").and_then(Json::as_str).unwrap_or("");
    let model = disk
        .get("FriendlyName")
        .and_then(Json::as_str)
        .map(str::trim)
        .filter(|m| !m.is_empty())
        .map(str::to_string)
        .unwrap_or_else(|| format!("Disk {number}"));
    TargetDisk {
        path: physical_drive_path(number),
        model,
        size_bytes,
        removable: is_removable(bus),
        external: is_external(bus),
        system,
        has_mounts,
    }
}

/// Turn a whole parsed `Get-Disk` document into hub-target candidates. The
/// wrapper emits `{ "SystemDriveDiskNumber": <n|null>, "Disks": [ … ] }`; this
/// reads the system-drive host, then builds a [`TargetDisk`] per disk (skipping
/// any with no `Number`, which can't be written). Makes no eligibility decision
/// — the caller runs each result through `hub_disk::classify`.
///
/// Liberal on shape only: a lone disk may arrive as an object rather than a
/// one-element array (the PowerShell 5.1 collapse), and is accepted as one disk.
pub fn disks_from_output(root: &Json) -> Result<Vec<TargetDisk>, String> {
    let system_drive = root.get("SystemDriveDiskNumber").and_then(Json::as_u64);

    let disks: Vec<&Json> = match root.get("Disks") {
        Some(Json::Array(items)) => items.iter().collect(),
        // Single-element collapse: one disk serialized as a bare object.
        Some(obj @ Json::Object(_)) => vec![obj],
        // An explicit null (no disks) is a legitimate empty machine.
        Some(Json::Null) => vec![],
        None => return Err("Get-Disk output had no 'Disks' field".to_string()),
        Some(other) => return Err(format!("unexpected 'Disks' value: {other:?}")),
    };

    let mut rows: Vec<(u64, TargetDisk)> = Vec::new();
    for disk in disks {
        let Some(number) = disk_number(disk) else {
            continue; // no Number → no device path → not a writable target
        };
        let system = is_system(disk, system_drive);
        let has_mounts = has_drive_letters(disk);
        rows.push((number, from_disk_json(number, disk, system, has_mounts)));
    }
    rows.sort_by_key(|(n, _)| *n);
    Ok(rows.into_iter().map(|(_, d)| d).collect())
}

/// Enumerate this PC's disks as hub-target candidates. Read-only: it runs a
/// single PowerShell that reports each disk's `Get-Disk` facts plus the disk
/// hosting the system drive, and hands the JSON to [`disks_from_output`]. Makes
/// no eligibility decision — the caller runs the result through
/// `hub_disk::classify`.
#[cfg(target_os = "windows")]
pub fn enumerate() -> Result<Vec<TargetDisk>, String> {
    // Emit a stable, minimal JSON: the system-drive host disk, and one row per
    // disk with exactly the fields the pure judgments read. UTF-8 without a BOM
    // so the parser gets clean bytes; every field is coerced to a plain type so
    // ConvertTo-Json can't surprise us with a nested CIM object.
    const SCRIPT: &str = r#"
$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false } catch {}
$sys = $null
try {
    $sysLetter = ($env:SystemDrive).TrimEnd(':')
    $sys = (Get-Partition -DriveLetter $sysLetter -ErrorAction SilentlyContinue).DiskNumber
} catch {}
$disks = @(Get-Disk | ForEach-Object {
    $n = $_.Number
    $letters = @()
    try {
        $letters = @(Get-Partition -DiskNumber $n -ErrorAction SilentlyContinue |
            Where-Object { $_.DriveLetter } |
            ForEach-Object { [string]$_.DriveLetter })
    } catch {}
    [PSCustomObject]@{
        Number       = [int]$n
        FriendlyName = [string]$_.FriendlyName
        Size         = [uint64]$_.Size
        BusType      = [string]$_.BusType
        IsBoot       = [bool]$_.IsBoot
        IsSystem     = [bool]$_.IsSystem
        DriveLetters = $letters
    }
} | Where-Object { $_ })
[PSCustomObject]@{
    SystemDriveDiskNumber = $sys
    Disks                 = $disks
} | ConvertTo-Json -Depth 5
"#;

    let out = std::process::Command::new("powershell")
        .args([
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            SCRIPT,
        ])
        .output()
        .map_err(|e| format!("couldn't run PowerShell (Get-Disk): {e}"))?;
    if !out.status.success() {
        let err = String::from_utf8_lossy(&out.stderr);
        return Err(format!("Get-Disk failed: {}", err.trim()));
    }
    let stdout = String::from_utf8(out.stdout)
        .map_err(|e| format!("PowerShell output wasn't UTF-8: {e}"))?;
    let root = parse_json(&stdout)?;
    disks_from_output(&root)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hub_disk::{classify, Eligibility, Refusal};

    // A fixture shaped exactly like Windows PowerShell 5.1's `ConvertTo-Json`
    // output — two-space-after-colon, the whole envelope — for a laptop with an
    // internal NVMe boot disk, a USB SD-card reader with a 64 GB card, a
    // built-in SD-slot card (32 GB), and an internal SATA data disk.
    const GET_DISK: &str = r#"{
    "SystemDriveDiskNumber":  0,
    "Disks":  [
        {
            "Number":  0,
            "FriendlyName":  "Samsung SSD 980 PRO 1TB",
            "Size":  1000204886016,
            "BusType":  "NVMe",
            "IsBoot":  true,
            "IsSystem":  true,
            "DriveLetters":  [
                "C"
            ]
        },
        {
            "Number":  1,
            "FriendlyName":  "Generic USB SD Reader",
            "Size":  63864569856,
            "BusType":  "USB",
            "IsBoot":  false,
            "IsSystem":  false,
            "DriveLetters":  [
                "E"
            ]
        },
        {
            "Number":  2,
            "FriendlyName":  "SDXC Card",
            "Size":  31914983424,
            "BusType":  "SD",
            "IsBoot":  false,
            "IsSystem":  false,
            "DriveLetters":  [

            ]
        },
        {
            "Number":  3,
            "FriendlyName":  "WD Blue SA510 2TB",
            "Size":  2000398934016,
            "BusType":  "SATA",
            "IsBoot":  false,
            "IsSystem":  false,
            "DriveLetters":  [
                "D"
            ]
        }
    ]
}"#;

    #[test]
    fn parses_the_envelope_and_reads_typed_fields() {
        let root = parse_json(GET_DISK).expect("parses");
        assert_eq!(
            root.get("SystemDriveDiskNumber").and_then(Json::as_u64),
            Some(0)
        );
        let disks = root.get("Disks").and_then(Json::as_array).expect("array");
        assert_eq!(disks.len(), 4);
        assert_eq!(
            disks[0].get("FriendlyName").and_then(Json::as_str),
            Some("Samsung SSD 980 PRO 1TB")
        );
        assert_eq!(
            disks[0].get("Size").and_then(Json::as_u64),
            Some(1_000_204_886_016)
        );
        assert_eq!(disks[0].get("IsBoot").and_then(Json::as_bool), Some(true));
    }

    #[test]
    fn parses_string_escapes_including_unicode() {
        let v = parse_json(r#"{"k":"a\"b\\c\/d\u00e9\tf"}"#).unwrap();
        assert_eq!(v.get("k").and_then(Json::as_str), Some("a\"b\\c/dé\tf"));
    }

    #[test]
    fn parses_a_surrogate_pair() {
        // U+1F426 BIRD, as a UTF-16 surrogate pair — because of course.
        let v = parse_json(r#"{"k":"\ud83d\udc26"}"#).unwrap();
        assert_eq!(v.get("k").and_then(Json::as_str), Some("🐦"));
    }

    #[test]
    fn strips_a_utf8_bom() {
        let v = parse_json("\u{feff}{\"k\":1}").unwrap();
        assert_eq!(v.get("k").and_then(Json::as_u64), Some(1));
    }

    #[test]
    fn parser_refuses_what_it_does_not_know() {
        assert!(parse_json("{\"k\":1").is_err()); // unterminated object
        assert!(parse_json("\"nope").is_err()); // unterminated string
        assert!(parse_json("[1, 2] extra").is_err()); // trailing junk
        assert!(parse_json("nul").is_err()); // bad literal
        assert!(parse_json("{\"k\" 1}").is_err()); // missing colon
        assert!(parse_json("not json").is_err());
        assert!(parse_json("").is_err());
    }

    #[test]
    fn quoted_numbers_still_read_as_u64() {
        // Defensive: even if a size arrives stringified, it's still a size.
        let v = parse_json(r#"{"Size":"63864569856"}"#).unwrap();
        assert_eq!(v.get("Size").and_then(Json::as_u64), Some(63_864_569_856));
    }

    #[test]
    fn physical_drive_paths_are_windows_shaped() {
        assert_eq!(physical_drive_path(0), r"\\.\PhysicalDrive0");
        assert_eq!(physical_drive_path(11), r"\\.\PhysicalDrive11");
    }

    #[test]
    fn external_is_usb_only_and_case_insensitive() {
        assert!(is_external("USB"));
        assert!(is_external("usb"));
        for internal in ["SATA", "NVMe", "RAID", "SCSI", "SAS", "ATA", "Virtual", ""] {
            assert!(
                !is_external(internal),
                "{internal} must not read as external"
            );
        }
    }

    #[test]
    fn removable_is_sd_or_mmc() {
        assert!(is_removable("SD"));
        assert!(is_removable("mmc"));
        for fixed in ["USB", "NVMe", "SATA", ""] {
            assert!(!is_removable(fixed), "{fixed} is not a removable card bus");
        }
    }

    #[test]
    fn system_flag_comes_from_isboot_issystem_or_the_system_drive_host() {
        let boot = parse_json(r#"{"Number":0,"IsBoot":true,"IsSystem":false}"#).unwrap();
        assert!(is_system(&boot, None));
        let sysflag = parse_json(r#"{"Number":5,"IsBoot":false,"IsSystem":true}"#).unwrap();
        assert!(is_system(&sysflag, None));
        // Neither flag set, but it's the disk hosting C: → still system.
        let hosts_c = parse_json(r#"{"Number":2,"IsBoot":false,"IsSystem":false}"#).unwrap();
        assert!(is_system(&hosts_c, Some(2)));
        assert!(!is_system(&hosts_c, Some(3)));
        assert!(!is_system(&hosts_c, None));
    }

    #[test]
    fn drive_letters_seen_as_array_single_and_empty() {
        let arr = parse_json(r#"{"DriveLetters":["C","D"]}"#).unwrap();
        assert!(has_drive_letters(&arr));
        // PowerShell 5.1 collapse: one letter arrives as a bare string.
        let single = parse_json(r#"{"DriveLetters":"E"}"#).unwrap();
        assert!(has_drive_letters(&single));
        let empty = parse_json(r#"{"DriveLetters":[]}"#).unwrap();
        assert!(!has_drive_letters(&empty));
        let missing = parse_json(r#"{"Number":9}"#).unwrap();
        assert!(!has_drive_letters(&missing));
    }

    // The payoff: the whole Get-Disk document composes with hub_disk::classify to
    // exactly the verdicts the picker must show.

    fn verdicts() -> Vec<(String, Eligibility)> {
        let root = parse_json(GET_DISK).expect("parses");
        disks_from_output(&root)
            .expect("builds targets")
            .into_iter()
            .map(|d| (d.path.clone(), classify(&d)))
            .collect()
    }

    #[test]
    fn the_boot_nvme_is_refused_as_the_system_disk() {
        let v = verdicts();
        let (path, elig) = &v[0];
        assert_eq!(path, r"\\.\PhysicalDrive0");
        assert_eq!(*elig, Eligibility::Refused(Refusal::SystemDisk));
    }

    #[test]
    fn the_usb_sd_reader_is_an_eligible_target_with_its_data_warned() {
        let root = parse_json(GET_DISK).unwrap();
        let disk = disks_from_output(&root).unwrap().remove(1);
        assert_eq!(disk.path, r"\\.\PhysicalDrive1");
        assert_eq!(disk.model, "Generic USB SD Reader");
        assert!(disk.external && !disk.removable && !disk.system);
        match classify(&disk) {
            Eligibility::Eligible { warnings } => {
                // Just under 64 GB (below recommended) AND a mounted volume.
                assert!(!warnings.is_empty());
            }
            other => panic!("expected eligible, got {other:?}"),
        }
    }

    #[test]
    fn the_builtin_sd_card_is_eligible_via_the_removable_bus() {
        let root = parse_json(GET_DISK).unwrap();
        let disk = disks_from_output(&root).unwrap().remove(2);
        assert_eq!(disk.path, r"\\.\PhysicalDrive2");
        assert!(disk.removable && !disk.external && !disk.has_mounts);
        assert!(classify(&disk).is_eligible());
    }

    #[test]
    fn the_internal_sata_data_disk_is_refused_as_internal_fixed() {
        let v = verdicts();
        let (path, elig) = &v[3];
        assert_eq!(path, r"\\.\PhysicalDrive3");
        // Not system, but SATA → neither removable nor external.
        assert_eq!(*elig, Eligibility::Refused(Refusal::InternalFixedDisk));
    }

    #[test]
    fn exactly_the_two_card_targets_are_offered() {
        let offered: Vec<String> = verdicts()
            .into_iter()
            .filter(|(_, e)| e.is_eligible())
            .map(|(p, _)| p)
            .collect();
        assert_eq!(
            offered,
            vec![
                r"\\.\PhysicalDrive1".to_string(),
                r"\\.\PhysicalDrive2".to_string()
            ]
        );
    }

    #[test]
    fn a_single_disk_object_is_tolerated() {
        // One disk → PowerShell 5.1 collapses "Disks" to a bare object.
        let json = r#"{
            "SystemDriveDiskNumber": 0,
            "Disks": {
                "Number": 1,
                "FriendlyName": "SanDisk Extreme",
                "Size": 63864569856,
                "BusType": "USB",
                "IsBoot": false,
                "IsSystem": false,
                "DriveLetters": "E"
            }
        }"#;
        let disks = disks_from_output(&parse_json(json).unwrap()).unwrap();
        assert_eq!(disks.len(), 1);
        assert_eq!(disks[0].path, r"\\.\PhysicalDrive1");
        assert!(classify(&disks[0]).is_eligible());
    }

    #[test]
    fn a_disk_that_hosts_the_system_drive_is_refused_even_without_flags() {
        // The boot flags are absent/false, but SystemDriveDiskNumber names it.
        let json = r#"{
            "SystemDriveDiskNumber": 1,
            "Disks": [
                {
                    "Number": 1,
                    "FriendlyName": "USB boot stick",
                    "Size": 63864569856,
                    "BusType": "USB",
                    "IsBoot": false,
                    "IsSystem": false,
                    "DriveLetters": ["C"]
                }
            ]
        }"#;
        let disk = disks_from_output(&parse_json(json).unwrap())
            .unwrap()
            .remove(0);
        assert!(disk.system);
        assert_eq!(classify(&disk), Eligibility::Refused(Refusal::SystemDisk));
    }

    #[test]
    fn a_null_size_reads_as_unknown_and_is_refused() {
        let json = r#"{"SystemDriveDiskNumber":0,"Disks":[
            {"Number":4,"FriendlyName":"Odd reader","Size":null,"BusType":"USB",
             "IsBoot":false,"IsSystem":false,"DriveLetters":[]}
        ]}"#;
        let disk = disks_from_output(&parse_json(json).unwrap())
            .unwrap()
            .remove(0);
        assert_eq!(disk.size_bytes, 0);
        assert_eq!(classify(&disk), Eligibility::Refused(Refusal::UnknownSize));
    }

    #[test]
    fn a_disk_with_no_number_is_skipped() {
        let json = r#"{"SystemDriveDiskNumber":0,"Disks":[
            {"FriendlyName":"nameless","Size":63864569856,"BusType":"USB"}
        ]}"#;
        assert!(disks_from_output(&parse_json(json).unwrap())
            .unwrap()
            .is_empty());
    }

    #[test]
    fn missing_disks_field_is_an_error() {
        let json = r#"{"SystemDriveDiskNumber":0}"#;
        assert!(disks_from_output(&parse_json(json).unwrap()).is_err());
    }

    #[test]
    fn model_falls_back_to_the_disk_number() {
        let disk = parse_json(r#"{"Number":7,"BusType":"USB","Size":63864569856}"#).unwrap();
        let t = from_disk_json(7, &disk, false, false);
        assert_eq!(t.model, "Disk 7");
    }
}
