//! Pure decision logic for the native **health check** — the read-only triage
//! that reads a board's own story straight off flash (partition map, the
//! firmware in each slot, update history, crash dumps, the witness chain) and
//! turns it into a plain-language report with self-heal guidance. Nothing here
//! writes a byte.
//!
//! Every parser mirrors the browser Lab's `flash-core.js` byte-for-byte — same
//! offsets, magics, endianness, and the same "skip non-printable, break on NUL"
//! string rule — so the two surfaces read a chip identically. Kept dependency
//! free (std only) so it unit-tests WITHOUT the desktop stack
//! (`rustc --test src/health.rs`), exactly like `rescue`/`hub-core`. The Tauri
//! `health_check` command in `lib.rs` reads the flash regions with the espflash
//! sidecar and feeds their bytes to these parsers.
//!
//! PRIVACY: the NVS parser extracts VALUES only for integer keys and an explicit
//! allow-list of blob keys (the health check passes only `"chain"`). Everything
//! else — the private key, saved Wi-Fi — is reported as presence + size only,
//! never read. This mirrors the browser's guarantee.

use std::collections::{HashMap, HashSet};

// ── little-endian primitives + the fixed-field string reader ──────────────────
fn u16le(b: &[u8], o: usize) -> u16 {
    (b[o] as u16) | ((b[o + 1] as u16) << 8)
}
fn u32le(b: &[u8], o: usize) -> u32 {
    (b[o] as u32) | ((b[o + 1] as u32) << 8) | ((b[o + 2] as u32) << 16) | ((b[o + 3] as u32) << 24)
}

/// A NUL-terminated fixed-length field. Matches the browser's `cstr`: stop at a
/// NUL or the end of the buffer, but SKIP (don't stop on) non-printable bytes,
/// so garbage flash renders the same on both surfaces.
fn cstr(b: &[u8], off: usize, len: usize) -> String {
    let mut s = String::new();
    for i in 0..len {
        match b.get(off + i) {
            None | Some(&0) => break,
            Some(&c) if (0x20..0x7f).contains(&c) => s.push(c as char),
            Some(_) => {} // non-printable → skip, keep going
        }
    }
    s
}

fn hex(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len() * 2);
    for byte in b {
        s.push_str(&format!("{byte:02x}"));
    }
    s
}

// ── partition table (flashed at 0x8000) ──────────────────────────────────────
pub const PARTITION_MAGIC: u16 = 0x50aa; // bytes 0xAA,0x50 → u16le

#[derive(Clone, Debug)]
pub struct Partition {
    pub ptype: u8,
    pub subtype: u8,
    pub offset: u32,
    pub size: u32,
    pub label: String,
}

/// 32-byte entries: magic 0xAA50 @0 | type @2 | subtype @3 | offset u32 @4 |
/// size u32 @8 | label[16] @12. Loop stops at the first non-magic word (0xFFFF
/// padding or the MD5 marker).
pub fn parse_partition_table(bytes: &[u8]) -> Vec<Partition> {
    let mut entries = Vec::new();
    let mut o = 0;
    while o + 32 <= bytes.len() {
        if u16le(bytes, o) != PARTITION_MAGIC {
            break;
        }
        entries.push(Partition {
            ptype: bytes[o + 2],
            subtype: bytes[o + 3],
            offset: u32le(bytes, o + 4),
            size: u32le(bytes, o + 8),
            label: cstr(bytes, o + 12, 16),
        });
        o += 32;
    }
    entries
}

/// The app slots (type 0x00) in table order.
pub fn app_partitions(entries: &[Partition]) -> Vec<Partition> {
    entries
        .iter()
        .filter(|e| e.ptype == 0x00)
        .cloned()
        .collect()
}

/// The OTA slots among the app partitions (subtype 0x10..0x1f = ota_0..ota_15).
pub fn ota_slots(apps: &[Partition]) -> Vec<Partition> {
    apps.iter()
        .filter(|a| (0x10..0x20).contains(&a.subtype))
        .cloned()
        .collect()
}

/// The slot the bootloader will actually run, given parsed otadata: fresh
/// otadata boots factory (else ota_0); otherwise ota_<active_ota>.
///
/// Judging by the wrong slot is not a cosmetic error. A board that OTA'd into
/// ota_1 still has the older image sitting in ota_0, so reading ota_0's
/// descriptor reports a version the board is not running — and the install
/// verdict then calls a downgrade an update, which is the one direction the
/// user most needs named out loud. Mirrors the browser's
/// pickBootedAppPartition (flash-core.js); the two must agree.
pub fn pick_booted_app_partition<'a>(
    apps: &'a [Partition],
    otadata: Option<&OtaInfo>,
) -> Option<&'a Partition> {
    if apps.is_empty() {
        return None;
    }
    let factory = || apps.iter().find(|a| a.subtype == 0x00);
    let ota_n = |n: u32| apps.iter().find(|a| a.subtype == 0x10 + (n as u8));
    match otadata {
        // No otadata read at all: prefer ota_0, else factory, else first —
        // the plain "which app is this" fallback.
        None => ota_n(0).or_else(factory).or_else(|| apps.first()),
        Some(o) if o.fresh => factory().or_else(|| ota_n(0)).or_else(|| apps.first()),
        Some(o) => ota_n(o.active_ota)
            .or_else(|| ota_n(0))
            .or_else(factory)
            .or_else(|| apps.first()),
    }
}

pub fn is_ota_data(e: &Partition) -> bool {
    e.ptype == 0x01 && e.subtype == 0x00
}
pub fn is_nvs(e: &Partition) -> bool {
    e.ptype == 0x01 && e.subtype == 0x02
}
pub fn is_coredump(e: &Partition) -> bool {
    e.ptype == 0x01 && e.subtype == 0x03
}
pub fn is_witness_log(e: &Partition) -> bool {
    e.ptype == 0x01 && e.label.to_ascii_lowercase().contains("witness")
}

/// A human name for a partition, e.g. `app · factory`, `app · ota_0`,
/// `data · nvs`. Middle-dot separator + lowercase hex, matching the browser.
pub fn partition_kind(ptype: u8, subtype: u8) -> String {
    if ptype == 0x00 {
        return match subtype {
            0x00 => "app · factory".into(),
            s if (0x10..0x20).contains(&s) => format!("app · ota_{}", s - 0x10),
            0x20 => "app · test".into(),
            s => format!("app · 0x{s:x}"),
        };
    }
    if ptype == 0x01 {
        return match data_subtype_name(subtype) {
            Some(name) => format!("data · {name}"),
            None => format!("data · 0x{subtype:x}"),
        };
    }
    format!("0x{ptype:x} · 0x{subtype:x}")
}

fn data_subtype_name(subtype: u8) -> Option<&'static str> {
    Some(match subtype {
        0x00 => "otadata",
        0x01 => "phy_init",
        0x02 => "nvs",
        0x03 => "coredump",
        0x04 => "nvs_keys",
        0x05 => "efuse",
        0x06 => "undefined",
        0x81 => "fat",
        0x82 => "spiffs",
        0x83 => "littlefs",
        _ => return None,
    })
}

// ── app descriptor (esp_app_desc_t, at app_offset + 0x20) ─────────────────────
pub const APP_DESC_MAGIC: u32 = 0xabcd_5432;
pub const APP_DESC_OFFSET: u32 = 0x20;

#[derive(Clone, Debug, PartialEq)]
pub struct AppDesc {
    pub version: String,
    pub project_name: String,
    pub time: String,
    pub date: String,
    pub idf_ver: String,
}

/// 256-byte `esp_app_desc_t`: magic 0xABCD5432 @0, version @16(32),
/// project_name @48(32), time @80(16), date @96(16), idf_ver @112(32).
pub fn parse_app_descriptor(bytes: &[u8]) -> Option<AppDesc> {
    if bytes.len() < 128 || u32le(bytes, 0) != APP_DESC_MAGIC {
        return None;
    }
    Some(AppDesc {
        version: cstr(bytes, 16, 32),
        project_name: cstr(bytes, 48, 32),
        time: cstr(bytes, 80, 16),
        date: cstr(bytes, 96, 16),
        idf_ver: cstr(bytes, 112, 32),
    })
}

// ── otadata (which slot is booted, how many updates) ──────────────────────────
/// `esp_rom_crc32_le(0, buf, len)` — reflected 0xEDB88320, zero init, final
/// complement. Bit-for-bit the algorithm esptool/esp-idf use for otadata.
pub fn crc32_esp_rom(bytes: &[u8]) -> u32 {
    let mut c: u32 = 0;
    for &b in bytes {
        c ^= b as u32;
        for _ in 0..8 {
            let mask = if c & 1 == 1 { 0xedb8_8320 } else { 0 };
            c = (c >> 1) ^ mask;
        }
    }
    !c
}

#[derive(Clone, Debug, PartialEq)]
pub struct OtaInfo {
    pub fresh: bool,
    pub active_ota: u32,
    pub updates_seen: u32,
    pub state_text: String,
    pub pending_verify: bool,
}

fn ota_state_text(state: u32) -> String {
    match state {
        0x0 => "new (never booted)".into(),
        0x1 => "pending verify".into(),
        0x2 => "valid".into(),
        0x3 => "invalid (rolled back)".into(),
        0x4 => "aborted".into(),
        0xffff_ffff => "normal".into(),
        s => format!("0x{s:x}"),
    }
}

/// Two `esp_ota_select_entry_t` records at buffer 0x0 and 0x1000: seq u32 @0,
/// state u32 @24, crc u32 @28. CRC validates over the first 4 bytes only.
///
/// The booted slot is the highest-seq entry the bootloader will actually START:
/// CRC-valid AND not marked invalid(0x3)/aborted(0x4). When the newest update
/// rolled back, the bootloader runs the previous good image — so `active_ota`
/// must exclude the rolled-back entry, even though `state_text`/`updates_seen`
/// still report the newest so the rollback is surfaced.
pub fn parse_ota_data(bytes: &[u8], ota_slot_count: u32) -> OtaInfo {
    let fresh = || OtaInfo {
        fresh: true,
        active_ota: 0,
        updates_seen: 0,
        state_text: "factory default".into(),
        pending_verify: false,
    };
    let mut valid: Vec<(u32, u32)> = Vec::new(); // (seq, state): CRC-ok, not erased
    for &off in &[0x0usize, 0x1000] {
        if off + 32 > bytes.len() {
            break;
        }
        let seq = u32le(bytes, off);
        let state = u32le(bytes, off + 24);
        let crc = u32le(bytes, off + 28);
        if seq != 0xffff_ffff && crc == crc32_esp_rom(&bytes[off..off + 4]) {
            valid.push((seq, state));
        }
    }
    if ota_slot_count == 0 {
        return fresh();
    }
    let newest = match valid.iter().max_by_key(|(seq, _)| *seq) {
        Some(&n) => n,
        None => return fresh(),
    };
    // Rollback states (INVALID / ABORTED) don't boot — pick from the rest.
    let bootable = valid
        .iter()
        .copied()
        .filter(|(_, st)| *st != 0x3 && *st != 0x4)
        .max_by_key(|(seq, _)| *seq);
    match bootable {
        Some((bseq, _)) => OtaInfo {
            fresh: false,
            active_ota: (bseq - 1) % ota_slot_count,
            updates_seen: newest.0,
            state_text: ota_state_text(newest.1),
            pending_verify: newest.1 == 0x1,
        },
        // Every update rolled back → the bootloader falls back to the factory app.
        None => OtaInfo {
            fresh: true,
            active_ota: 0,
            updates_seen: newest.0,
            state_text: ota_state_text(newest.1),
            pending_verify: false,
        },
    }
}

// ── coredump (was there a crash?) ─────────────────────────────────────────────
#[derive(Clone, Debug, PartialEq)]
pub struct Coredump {
    pub present: bool,
    pub size: Option<u32>,
}

/// The first word of the coredump partition is the dump length; erased flash
/// reads 0xFFFFFFFF. Present iff `len != 0xFFFFFFFF && 0 < len <= partition size`.
pub fn parse_coredump_header(bytes: &[u8], partition_size: u32) -> Coredump {
    if bytes.len() < 4 {
        return Coredump {
            present: false,
            size: None,
        };
    }
    let len = u32le(bytes, 0);
    let cap = if partition_size != 0 {
        partition_size
    } else {
        0x10000
    };
    if len != 0xffff_ffff && len > 0 && len <= cap {
        Coredump {
            present: true,
            size: Some(len),
        }
    } else {
        Coredump {
            present: false,
            size: None,
        }
    }
}

// ── NVS (the witness state lives here) ────────────────────────────────────────
const NVS_WRITTEN: u8 = 2; // 2-bit entry state 0b10

#[derive(Clone, Debug)]
pub struct NvsItem {
    pub ns_index: u8,
    pub namespace: String,
    pub key: String,
    pub itype: u8,
    pub value: Option<u64>,
    pub size: Option<u32>,
    pub bytes: Option<Vec<u8>>,
}

fn nvs_int_len(t: u8) -> Option<usize> {
    match t {
        0x01 | 0x11 => Some(1),
        0x02 | 0x12 => Some(2),
        0x04 | 0x14 => Some(4),
        0x08 | 0x18 => Some(8),
        _ => None,
    }
}

/// ESP-IDF NVS: 4 KB pages, each a page-state word @0, a 32-byte 2-bit entry
/// bitmap @32, then 126 × 32-byte entries @64. Entry: ns u8 @0 | type u8 @1 |
/// span u8 @2 | chunk u8 @3 | crc @4 | key[16] @8 | data[8] @24. Values are
/// decoded only for integer types and allow-listed blob keys.
pub fn parse_nvs(bytes: &[u8], allow_blob_keys: &[&str]) -> Vec<NvsItem> {
    let allow: HashSet<&str> = allow_blob_keys.iter().copied().collect();
    let mut ns_names: HashMap<u8, String> = HashMap::new();
    let mut items: Vec<NvsItem> = Vec::new();
    let mut blob_chunks: HashMap<(u8, String), Vec<(u8, Vec<u8>)>> = HashMap::new();

    let mut page = 0;
    while page + 4096 <= bytes.len() {
        if u32le(bytes, page) == 0xffff_ffff {
            page += 4096; // uninitialized page
            continue;
        }
        let mut idx = 0usize;
        while idx < 126 {
            let st = (bytes[page + 32 + (idx >> 2)] >> ((idx & 3) * 2)) & 0x3;
            if st != NVS_WRITTEN {
                idx += 1;
                continue;
            }
            let o = page + 64 + idx * 32;
            let ns = bytes[o];
            let ttype = bytes[o + 1];
            let span = if bytes[o + 2] == 0 { 1 } else { bytes[o + 2] };
            let chunk_idx = bytes[o + 3];
            let key = cstr(bytes, o + 8, 16);
            if key.is_empty() {
                idx += span as usize;
                continue;
            }
            // How many payload bytes this entry's span can hold, clamped to buf.
            let span_cap = (span as usize - 1) * 32;
            let blob_slice = |declared: usize| -> Vec<u8> {
                let take = declared.min(span_cap);
                let end = (o + 32 + take).min(bytes.len());
                bytes[(o + 32).min(end)..end].to_vec()
            };

            if ns == 0 && ttype == 0x01 {
                ns_names.insert(bytes[o + 24], key.clone()); // namespace definition
            } else if let Some(len) = nvs_int_len(ttype) {
                let mut v: u64 = 0;
                for i in (0..len).rev() {
                    v = v * 256 + bytes[o + 24 + i] as u64;
                }
                items.push(NvsItem {
                    ns_index: ns,
                    namespace: String::new(),
                    key,
                    itype: ttype,
                    value: Some(v),
                    size: None,
                    bytes: None,
                });
            } else if ttype == 0x21 || ttype == 0x41 || ttype == 0x42 {
                let size = u16le(bytes, o + 24) as u32;
                if ttype == 0x42 && allow.contains(key.as_str()) {
                    blob_chunks
                        .entry((ns, key.clone()))
                        .or_default()
                        .push((chunk_idx, blob_slice(size as usize)));
                }
                if ttype == 0x41 {
                    let bytes_field = allow
                        .contains(key.as_str())
                        .then(|| blob_slice(size as usize));
                    items.push(NvsItem {
                        ns_index: ns,
                        namespace: String::new(),
                        key,
                        itype: 0x42,
                        value: None,
                        size: Some(size),
                        bytes: bytes_field,
                    });
                } else if ttype == 0x21 {
                    let bytes_field = allow
                        .contains(key.as_str())
                        .then(|| blob_slice(size.saturating_sub(1) as usize));
                    items.push(NvsItem {
                        ns_index: ns,
                        namespace: String::new(),
                        key,
                        itype: 0x21,
                        value: None,
                        size: Some(size),
                        bytes: bytes_field,
                    });
                }
            } else if ttype == 0x48 {
                // v2 blob index: authoritative total size (u32).
                items.push(NvsItem {
                    ns_index: ns,
                    namespace: String::new(),
                    key,
                    itype: 0x42,
                    value: None,
                    size: Some(u32le(bytes, o + 24)),
                    bytes: None,
                });
            }
            idx += span as usize;
        }
        page += 4096;
    }

    for item in items.iter_mut() {
        item.namespace = ns_names
            .get(&item.ns_index)
            .cloned()
            .unwrap_or_else(|| item.ns_index.to_string());
        if item.itype == 0x42 && allow.contains(item.key.as_str()) {
            if let Some(chunks) = blob_chunks.get(&(item.ns_index, item.key.clone())) {
                let mut chunks = chunks.clone();
                chunks.sort_by_key(|(ci, _)| *ci);
                let mut buf = Vec::new();
                for (_, data) in &chunks {
                    buf.extend_from_slice(data);
                }
                if let Some(sz) = item.size {
                    buf.truncate(sz as usize);
                }
                item.bytes = Some(buf);
            }
        }
    }
    items
}

// ── the witness summary (SecuraCV's own NVS namespace) ────────────────────────
pub const WITNESS_NVS_NAMESPACE: &str = "securacv";
pub const WITNESS_CHAIN_BLOB_KEY: &str = "chain";

#[derive(Clone, Debug, PartialEq)]
pub struct Witness {
    pub seq: Option<u64>,
    pub boots: Option<u64>,
    pub tamper: Option<u64>,
    pub log_seq: Option<u64>,
    pub chain_head_fp: Option<String>,
    pub provisioned: bool,
    pub wifi_configured: bool,
}

/// Reduce the parsed NVS items to the witness facts, presence-only for secrets.
pub fn witness_summary(items: &[NvsItem]) -> Option<Witness> {
    let ours: Vec<&NvsItem> = items
        .iter()
        .filter(|i| i.namespace == WITNESS_NVS_NAMESPACE)
        .collect();
    if ours.is_empty() {
        return None;
    }
    let get = |k: &str| ours.iter().find(|i| i.key == k).copied();
    Some(Witness {
        seq: get("seq").and_then(|i| i.value),
        boots: get("boots").and_then(|i| i.value),
        tamper: get("tamper").and_then(|i| i.value),
        log_seq: get("logseq").and_then(|i| i.value),
        chain_head_fp: get(WITNESS_CHAIN_BLOB_KEY)
            .and_then(|i| i.bytes.as_ref())
            .map(|b| hex(&b[..b.len().min(8)])),
        provisioned: get("privkey").is_some(), // presence only — key never read
        wifi_configured: get("wifi_ssid").is_some(), // presence only — value never read
    })
}

// ── the self-heal verdict: turn the report into plain-language findings ────────
/// One finding: a severity, what it means, and what to do about it.
#[derive(Clone, Debug, PartialEq)]
pub struct Finding {
    pub severity: &'static str, // "ok" | "warn" | "attn"
    pub title: String,
    pub fix: String,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Verdict {
    pub level: &'static str, // worst severity present
    pub headline: String,
    pub findings: Vec<Finding>,
}

/// The inputs the verdict reasons over — a flattened view of the report so this
/// stays pure and host-testable without the JSON layer.
#[derive(Clone, Debug, Default)]
pub struct VerdictInput {
    pub blank: bool,
    pub coredump_present: bool,
    pub ota_pending_verify: bool,
    pub ota_rolled_back: bool,
    pub tamper: Option<u64>,
    pub has_running_slot: bool,
    pub provisioned: bool,
}

fn worst(a: &'static str, b: &'static str) -> &'static str {
    let rank = |s: &str| match s {
        "attn" => 2,
        "warn" => 1,
        _ => 0,
    };
    if rank(b) > rank(a) {
        b
    } else {
        a
    }
}

/// Translate the report into a verdict + a list of "here's what's off and how to
/// fix it" findings. Mirrors the browser report's "worrying" heuristic
/// (blank / crash dump / pending-verify / tamper) and adds the fix for each.
pub fn report_verdict(inp: &VerdictInput) -> Verdict {
    let mut findings: Vec<Finding> = Vec::new();

    if inp.blank {
        findings.push(Finding {
            severity: "attn",
            title: "This chip looks blank — no partition table.".into(),
            fix: "Flash a factory image: \"Flash my Canary\", or Advanced → Install a local file."
                .into(),
        });
    }
    if inp.coredump_present {
        findings.push(Finding {
            severity: "warn",
            title: "A crash dump is stored — the board hard-crashed at some point.".into(),
            fix:
                "Not an emergency: flashing fresh firmware or a full erase clears it. If it keeps \
                  crashing, mention this dump in a bug report."
                    .into(),
        });
    }
    if inp.ota_pending_verify {
        findings.push(Finding {
            severity: "warn",
            title: "The last update booted but never confirmed itself (pending verify).".into(),
            fix: "Let it run a little; if it rolls back on its own, reflash the newest firmware."
                .into(),
        });
    }
    if inp.ota_rolled_back {
        findings.push(Finding {
            severity: "warn",
            title: "The last update was rolled back — it failed its self-check and reverted."
                .into(),
            fix:
                "The board is running the previous image. Reflash the newest firmware to try again."
                    .into(),
        });
    }
    if matches!(inp.tamper, Some(t) if t != 0) {
        findings.push(Finding {
            severity: "attn",
            title: format!("The tamper flag is set ({}).", inp.tamper.unwrap_or(0)),
            fix: "The board is flagging physical interference. Investigate before trusting it; \
                  back it up first if you want the evidence."
                .into(),
        });
    }
    if !inp.blank && !inp.has_running_slot {
        findings.push(Finding {
            severity: "warn",
            title: "No bootable app slot is marked as running.".into(),
            fix: "If the board won't boot, reflash firmware (Flash my Canary) or restore a backup."
                .into(),
        });
    }

    let level = findings.iter().fold("ok", |acc, f| worst(acc, f.severity));
    let headline = match level {
        "attn" => "Needs attention",
        "warn" => "Up, with a couple of things to know",
        _ if inp.blank => "Blank board",
        _ => "Everything reads healthy",
    }
    .to_string();

    if findings.is_empty() {
        findings.push(Finding {
            severity: "ok",
            title: if inp.provisioned {
                "Provisioned, no crashes, no tamper flags — all good.".into()
            } else {
                "No crashes, no tamper flags — all good.".into()
            },
            fix:
                "After any install or restore, a health check like this is the quickest confidence \
                  test."
                    .into(),
        });
    }

    Verdict {
        level,
        headline,
        findings,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Build a single 32-byte partition-table entry.
    fn pt_entry(ptype: u8, subtype: u8, offset: u32, size: u32, label: &str) -> Vec<u8> {
        let mut e = vec![0u8; 32];
        e[0] = 0xaa;
        e[1] = 0x50; // magic 0x50AA
        e[2] = ptype;
        e[3] = subtype;
        e[4..8].copy_from_slice(&offset.to_le_bytes());
        e[8..12].copy_from_slice(&size.to_le_bytes());
        let lb = label.as_bytes();
        e[12..12 + lb.len().min(16)].copy_from_slice(&lb[..lb.len().min(16)]);
        e
    }

    #[test]
    fn partition_table_parses_and_stops_on_padding() {
        let mut t = Vec::new();
        t.extend(pt_entry(0x00, 0x00, 0x10000, 0x100000, "factory"));
        t.extend(pt_entry(0x01, 0x02, 0x9000, 0x6000, "nvs"));
        t.extend(pt_entry(0x01, 0x03, 0xf000, 0x10000, "coredump"));
        t.extend(vec![0xff; 32]); // padding → loop stops here
        t.extend(pt_entry(0x00, 0x10, 0x110000, 0x100000, "ota_0")); // must be ignored

        let entries = parse_partition_table(&t);
        assert_eq!(entries.len(), 3);
        assert_eq!(entries[0].label, "factory");
        assert_eq!(entries[0].offset, 0x10000);
        assert!(is_nvs(&entries[1]));
        assert!(is_coredump(&entries[2]));
        assert_eq!(app_partitions(&entries).len(), 1);
        assert_eq!(partition_kind(0x00, 0x00), "app · factory");
        assert_eq!(partition_kind(0x00, 0x11), "app · ota_1");
        assert_eq!(partition_kind(0x01, 0x02), "data · nvs");
    }

    #[test]
    fn app_descriptor_reads_project_and_version() {
        let mut d = vec![0u8; 256];
        d[0..4].copy_from_slice(&APP_DESC_MAGIC.to_le_bytes());
        d[16..23].copy_from_slice(b"v1.2.3\0");
        d[48..56].copy_from_slice(b"canary\0\0");
        d[96..107].copy_from_slice(b"2026-07-26\0");
        let a = parse_app_descriptor(&d).unwrap();
        assert_eq!(a.version, "v1.2.3");
        assert_eq!(a.project_name, "canary");
        assert_eq!(a.date, "2026-07-26");
        // Wrong magic or too-short → None.
        assert!(parse_app_descriptor(&[0u8; 256]).is_none());
        assert!(parse_app_descriptor(&d[..100]).is_none());
    }

    #[test]
    fn cstr_skips_nonprintable_and_stops_at_nul() {
        // \x07 (bell) is skipped, not a terminator; \0 stops.
        let b = b"ab\x07c\0defgh";
        assert_eq!(cstr(b, 0, 10), "abc");
    }

    #[test]
    fn crc32_matches_esp_rom_and_ota_picks_highest_seq() {
        // Build two otadata records; the second has the higher valid seq.
        let mut buf = vec![0xffu8; 0x2000];
        let rec = |seq: u32, state: u32| -> Vec<u8> {
            let mut r = vec![0u8; 32];
            r[0..4].copy_from_slice(&seq.to_le_bytes());
            r[24..28].copy_from_slice(&state.to_le_bytes());
            let crc = crc32_esp_rom(&seq.to_le_bytes());
            r[28..32].copy_from_slice(&crc.to_le_bytes());
            r
        };
        buf[0..32].copy_from_slice(&rec(1, 0x2)); // seq 1, valid
        buf[0x1000..0x1000 + 32].copy_from_slice(&rec(2, 0x1)); // seq 2, pending-verify
        let ota = parse_ota_data(&buf, 2);
        assert!(!ota.fresh);
        assert_eq!(ota.updates_seen, 2);
        assert_eq!(ota.active_ota, (2 - 1) % 2); // = 1
        assert!(ota.pending_verify);
        assert_eq!(ota.state_text, "pending verify");
        // All-erased otadata → factory default.
        assert!(parse_ota_data(&vec![0xff; 0x2000], 2).fresh);

        // Rollback: newest (seq 3) is INVALID → the bootloader runs the previous
        // good image (seq 2), so the running slot must exclude the rolled-back one.
        let mut rb = vec![0xffu8; 0x2000];
        rb[0..32].copy_from_slice(&rec(2, 0x2)); // seq 2, valid → slot (2-1)%2 = 1
        rb[0x1000..0x1000 + 32].copy_from_slice(&rec(3, 0x3)); // seq 3, invalid (rolled back)
        let ro = parse_ota_data(&rb, 2);
        assert!(!ro.fresh);
        assert_eq!(ro.updates_seen, 3); // the attempt is still counted
        assert_eq!(ro.active_ota, (2 - 1) % 2); // = 1, the previous good slot — NOT (3-1)%2 = 0
        assert!(ro.state_text.contains("rolled back"));
        assert!(!ro.pending_verify);
    }

    #[test]
    fn coredump_presence_by_length_word() {
        assert!(!parse_coredump_header(&0xffff_ffffu32.to_le_bytes(), 0x10000).present);
        assert!(!parse_coredump_header(&0u32.to_le_bytes(), 0x10000).present);
        let c = parse_coredump_header(&1234u32.to_le_bytes(), 0x10000);
        assert!(c.present && c.size == Some(1234));
        // Bigger than the partition → not a real dump.
        assert!(!parse_coredump_header(&0x20000u32.to_le_bytes(), 0x10000).present);
    }

    // Build a one-page NVS image: namespace def + a couple of entries.
    fn nvs_page(entries: &[(u8, u8, &str, &[u8])]) -> Vec<u8> {
        // (ns, type, key, data[8]) — caller writes the namespace def as ns=0,type=0x01.
        let mut page = vec![0xffu8; 4096];
        page[0..4].copy_from_slice(&0u32.to_le_bytes()); // page active (not 0xFFFFFFFF)
                                                         // bitmap starts all-0xFF (erased); we set each used entry to "written" (0b10).
        for (i, (ns, ttype, key, data)) in entries.iter().enumerate() {
            let bm = 32 + (i >> 2);
            // clear the 2 bits then set 0b10
            page[bm] &= !(0b11 << ((i & 3) * 2));
            page[bm] |= NVS_WRITTEN << ((i & 3) * 2);
            let o = 64 + i * 32;
            page[o] = *ns;
            page[o + 1] = *ttype;
            page[o + 2] = 1; // span
            let kb = key.as_bytes();
            page[o + 8..o + 8 + kb.len().min(16)].copy_from_slice(&kb[..kb.len().min(16)]);
            page[o + 24..o + 24 + data.len().min(8)].copy_from_slice(&data[..data.len().min(8)]);
        }
        page
    }

    #[test]
    fn nvs_and_witness_summary_presence_only_for_secrets() {
        let page = nvs_page(&[
            (0, 0x01, "securacv", &[7]),      // namespace def → index 7
            (7, 0x04, "seq", &[42, 0, 0, 0]), // u32 = 42
            (7, 0x04, "boots", &[9, 0, 0, 0]),
            (7, 0x01, "tamper", &[0]),       // u8 = 0
            (7, 0x21, "wifi_ssid", &[5, 0]), // string, size 5 — presence only
            (7, 0x21, "privkey", &[32, 0]),  // string — presence only
        ]);
        let items = parse_nvs(&page, &[WITNESS_CHAIN_BLOB_KEY]);
        let w = witness_summary(&items).unwrap();
        assert_eq!(w.seq, Some(42));
        assert_eq!(w.boots, Some(9));
        assert_eq!(w.tamper, Some(0));
        assert!(w.provisioned); // privkey present
        assert!(w.wifi_configured); // wifi_ssid present
        assert!(w.chain_head_fp.is_none()); // no chain blob written
                                            // wifi_ssid/privkey values were NOT extracted — only presence.
        let ssid = items.iter().find(|i| i.key == "wifi_ssid").unwrap();
        assert!(ssid.bytes.is_none());
    }

    #[test]
    fn verdict_flags_blank_crash_and_tamper_with_fixes() {
        let blank = report_verdict(&VerdictInput {
            blank: true,
            ..Default::default()
        });
        assert_eq!(blank.level, "attn");
        assert!(blank.findings[0].fix.contains("factory image"));

        let crash = report_verdict(&VerdictInput {
            coredump_present: true,
            has_running_slot: true,
            ..Default::default()
        });
        assert_eq!(crash.level, "warn");

        let tamper = report_verdict(&VerdictInput {
            tamper: Some(3),
            has_running_slot: true,
            ..Default::default()
        });
        assert_eq!(tamper.level, "attn");
        assert!(tamper.findings.iter().any(|f| f.title.contains("tamper")));

        let healthy = report_verdict(&VerdictInput {
            has_running_slot: true,
            provisioned: true,
            ..Default::default()
        });
        assert_eq!(healthy.level, "ok");
        assert_eq!(healthy.findings.len(), 1);
        assert_eq!(healthy.findings[0].severity, "ok");
    }

    fn part(subtype: u8, label: &str) -> Partition {
        Partition {
            ptype: 0x00,
            subtype,
            offset: 0x10000,
            size: 0x100000,
            label: label.into(),
        }
    }
    fn ota(fresh: bool, active_ota: u32) -> OtaInfo {
        OtaInfo {
            fresh,
            active_ota,
            updates_seen: 0,
            state_text: "valid".into(),
            pending_verify: false,
        }
    }

    #[test]
    fn booted_slot_follows_otadata_not_table_order() {
        let apps = vec![part(0x00, "factory"), part(0x10, "ota_0"), part(0x11, "ota_1")];
        assert_eq!(ota_slots(&apps).len(), 2, "factory is not an OTA slot");

        // Fresh otadata → the bootloader runs factory, whatever else exists.
        let booted = pick_booted_app_partition(&apps, Some(&ota(true, 0))).unwrap();
        assert_eq!(booted.label, "factory");

        // The case that makes this function worth having: a board that has
        // OTA'd into ota_1 must be judged by ota_1, never by the older image
        // still sitting in ota_0 — that misread turns a downgrade into an
        // "update" and the user is never warned.
        let booted = pick_booted_app_partition(&apps, Some(&ota(false, 1))).unwrap();
        assert_eq!(booted.label, "ota_1");

        let booted = pick_booted_app_partition(&apps, Some(&ota(false, 0))).unwrap();
        assert_eq!(booted.label, "ota_0");
    }

    #[test]
    fn booted_slot_degrades_without_otadata_and_on_odd_layouts() {
        let apps = vec![part(0x00, "factory"), part(0x10, "ota_0")];
        // otadata unreadable: prefer ota_0 — the plain "which app is this".
        assert_eq!(
            pick_booted_app_partition(&apps, None).unwrap().label,
            "ota_0"
        );
        // A factory-only layout has no ota_N to point at; the answer is the
        // factory app, not None (an ESP32 with no otadata boots factory).
        let only_factory = vec![part(0x00, "factory")];
        assert_eq!(
            pick_booted_app_partition(&only_factory, Some(&ota(false, 1)))
                .unwrap()
                .label,
            "factory"
        );
        // An active_ota pointing past the slots that exist still resolves to
        // something bootable rather than dropping the passport entirely.
        let two = vec![part(0x10, "ota_0"), part(0x11, "ota_1")];
        assert_eq!(
            pick_booted_app_partition(&two, Some(&ota(false, 7))).unwrap().label,
            "ota_0"
        );
        assert!(pick_booted_app_partition(&[], Some(&ota(true, 0))).is_none());
    }
}
