//! Native security-fuse intake — the desktop side of the browser's "customs"
//! eFuse check (canary-local/assets/intake.js), which used to be browser-only.
//!
//! The espflash CLI this app drives has no fuse-read command (verified against
//! v3.3.0, the pinned sidecar, and v4.3.0, the latest release: neither lists
//! one), and the flash engine stays espflash-the-CLI on purpose (desktop
//! README: "stable CLI, not a fragile library binding"). But reading eFuse
//! block 0 does not need a flash engine — it is the ROM serial protocol's
//! READ_REG command, six plain register reads. This module speaks exactly that
//! much of the protocol itself, over the same `serialport` crate the WE2
//! flasher and the monitor already use.
//!
//! What this deliberately does NOT do, same as the browser's check:
//!   - It never WRITES anything. The only commands ever sent are SYNC and
//!     READ_REG — no flash writes, no eFuse burns, no stub upload. A probe
//!     that can't reach the ROM reports "not checked", never "clean".
//!   - It does not guess on a chip without a verified bit table. The tables
//!     below cover ESP32-S3/C3/C6, the same set the browser supports; any
//!     other chip answers `supported: false`.
//!
//! The field tables, bit offsets, and the three-state clean/touched/active
//! semantics are a line-for-line port of intake.js, and the desktop-parity
//! test pins the two against each other — one drifting field fails CI.

use serde::Serialize;
use std::io::{Read, Write};
use std::time::{Duration, Instant};

// ── eFuse block 0: where the security bits live ─────────────────────────────
// Block 0 read registers start at EFUSE_BASE + 0x02C and run 6 words — same
// constants as intake.js (which cites esptool's mem_definition.py). The bases
// are the vendored esptool-js chip definitions' EFUSE_BASE values
// (canary-local/assets/vendor/esptool-js/bundle.js), pinned here because the
// desktop has no esploader object to read them from at runtime; the
// desktop-parity test asserts each one appears in the vendored bundle.
pub const EFUSE_BLOCK0_RD_OFFSET: u32 = 0x02c;
pub const EFUSE_BLOCK0_WORDS: usize = 6;

pub fn efuse_base(chip: &str) -> Option<u32> {
    match chip {
        "ESP32-S3" => Some(0x6000_7000),
        "ESP32-C3" => Some(0x6000_8800),
        "ESP32-C6" => Some(0x600B_0800),
        _ => None,
    }
}

pub fn block0_addrs(efuse_base: u32) -> Vec<u32> {
    let base = efuse_base + EFUSE_BLOCK0_RD_OFFSET;
    (0..EFUSE_BLOCK0_WORDS as u32)
        .map(|i| base + 4 * i)
        .collect()
}

// Pull a field out of the block-0 words — bit N of block 0 is bit (N % 32) of
// word N / 32. Written general (a field may straddle a word) like intake.js.
pub fn efuse_field(words: &[u32], bit: u32, width: u32) -> Option<u32> {
    let mut out: u32 = 0;
    for i in 0..width {
        let abs = bit + i;
        let w = *words.get((abs / 32) as usize)?;
        if (w >> (abs % 32)) & 1 == 1 {
            out |= 1 << i;
        }
    }
    Some(out)
}

// Several ESP32 counter eFuses are read by the PARITY of their set bits, not
// their numeric value (ESP-IDF esp_efuse_fields.c walks the bits) — 0b001 and
// 0b111 are "enabled", 0b011 is "disabled again". Same rule as intake.js.
pub fn odd_parity(v: u32) -> bool {
    v.count_ones() % 2 == 1
}

#[derive(Clone, Copy)]
enum Active {
    NonZero,
    OddParity,
}

struct FieldDef {
    key: &'static str,
    bit: u32,
    width: u32,
    severity: &'static str,
    label: &'static str,
    meaning: &'static str,
    active: Active,
    touched: Option<(&'static str, &'static str)>, // (severity, meaning)
}

const fn field(
    key: &'static str,
    bit: u32,
    width: u32,
    severity: &'static str,
    label: &'static str,
    meaning: &'static str,
) -> FieldDef {
    FieldDef {
        key,
        bit,
        width,
        severity,
        label,
        meaning,
        active: Active::NonZero,
        touched: None,
    }
}

// The security fields, bit offsets from ESP-IDF's esp_efuse_table.csv — the
// SAME keys, bits, widths, severities and user-facing copy as intake.js
// COMMON_EFUSES (two flashers, one set of words for one failure). Only fields
// that are zero on a factory-fresh part are listed; a non-zero reading means a
// person burned it, not the factory.
fn common_efuses() -> Vec<FieldDef> {
    vec![
        field("SECURE_BOOT_EN", 116, 1, "stop", "Secure boot",
              "This board is locked to somebody else's signing key. Its ROM will refuse our bootloader, and there is no way to undo it — eFuses are one-way."),
        FieldDef {
            key: "SPI_BOOT_CRYPT_CNT", bit: 82, width: 3, severity: "stop",
            label: "Flash encryption",
            meaning: "Flash is encrypted with a key burned into this chip. Reads come back as ciphertext and a plaintext image won't boot. Not reversible.",
            active: Active::OddParity,
            touched: Some(("attention",
                "Flash encryption was switched on and then off again by a previous owner. It isn't active now, so the board still works — but this chip has been through somebody's provisioning, and the counter can only be burned a couple more times.")),
        },
        field("DIS_DOWNLOAD_MODE", 128, 1, "stop", "Download mode disabled",
              "Somebody burned away the USB recovery path. This is the one state a Canary cannot be flashed out of."),
        field("ENABLE_SECURITY_DOWNLOAD", 133, 1, "stop", "Secure download mode",
              "Download mode is restricted to a reduced command set — no flash reads, no backup, so we can't verify what's on it."),
        field("SECURE_BOOT_AGGRESSIVE_REVOKE", 117, 1, "attention", "Aggressive key revocation",
              "Set alongside secure boot by a previous owner."),
        field("DIS_DOWNLOAD_MANUAL_ENCRYPT", 52, 1, "attention", "Manual encryption disabled",
              "Part of a flash-encryption lockdown someone applied."),
        field("DIS_PAD_JTAG", 51, 1, "attention", "JTAG permanently disabled",
              "Hardware debug was burned off. Harmless to run, but a factory-fresh board never has this set."),
        field("SOFT_DIS_JTAG", 48, 3, "attention", "JTAG lock bits",
              "Somebody burned the JTAG soft-disable bits. A factory-fresh board reads zero here. (We don't claim what state debug is in right now — only that this chip left the factory untouched and isn't untouched any more.)"),
        field("SECURE_VERSION", 142, 16, "attention", "Anti-rollback floor",
              "An anti-rollback minimum was burned in. The board will refuse firmware it considers older — including, possibly, ours."),
    ]
}

// The USB/JTAG switch moved between chip generations — same table as
// intake.js USB_EFUSES.
fn usb_efuses(chip: &str) -> Option<Vec<(&'static str, u32, u32)>> {
    match chip {
        "ESP32-S3" => Some(vec![
            ("DIS_USB_JTAG", 118, 1),
            ("DIS_USB_SERIAL_JTAG", 119, 1),
        ]),
        "ESP32-C3" => Some(vec![("DIS_USB_JTAG", 41, 1)]),
        "ESP32-C6" => Some(vec![
            ("DIS_USB_JTAG", 41, 1),
            ("DIS_USB_SERIAL_JTAG", 43, 1),
        ]),
        _ => None,
    }
}

const USB_META_LABEL: &str = "USB debug disabled";
const USB_META_MEANING: &str =
    "The USB-JTAG path was burned off by a previous owner. A new board never has this set.";

fn security_fields_for(chip: &str) -> Option<Vec<FieldDef>> {
    let usb = usb_efuses(chip)?;
    let mut table = common_efuses();
    for (key, bit, width) in usb {
        table.push(field(
            key,
            bit,
            width,
            "attention",
            USB_META_LABEL,
            USB_META_MEANING,
        ));
    }
    Some(table)
}

// ── the verdict, shaped like intake.js readSecurityEfuses ───────────────────

#[derive(Serialize, Clone)]
pub struct EfuseFieldState {
    pub key: &'static str,
    pub label: &'static str,
    pub value: u32,
    pub state: &'static str, // "clean" | "touched" | "active"
    pub severity: Option<&'static str>,
    pub meaning: Option<&'static str>,
    pub burned: bool,
}

#[derive(Serialize, Clone)]
pub struct EfuseScan {
    pub supported: bool,
    pub chip: String,
    pub words: Vec<u32>,
    pub fields: Vec<EfuseFieldState>,
    pub burned: Vec<EfuseFieldState>,
    pub virgin: Option<bool>,
}

// Decode block 0 into a verdict — three states, not two, exactly as the
// browser reads it: "active" is the dangerous reading, "touched" was burned
// but isn't in force (a person was here; folding it into "clean" would hand a
// used board a clean bill of health).
pub fn decode_security_efuses(chip: &str, words: &[u32]) -> EfuseScan {
    let Some(table) = security_fields_for(chip) else {
        return EfuseScan {
            supported: false,
            chip: chip.to_string(),
            words: Vec::new(),
            fields: Vec::new(),
            burned: Vec::new(),
            virgin: None,
        };
    };
    let fields: Vec<EfuseFieldState> = table
        .iter()
        .map(|f| {
            let value = efuse_field(words, f.bit, f.width).unwrap_or(0);
            let active = match f.active {
                Active::NonZero => value > 0,
                Active::OddParity => odd_parity(value),
            };
            let touched = value > 0;
            let (state, severity, meaning) = if active {
                ("active", Some(f.severity), Some(f.meaning))
            } else if touched {
                match f.touched {
                    Some((sev, why)) => ("touched", Some(sev), Some(why)),
                    None => ("clean", None, None),
                }
            } else {
                ("clean", None, None)
            };
            EfuseFieldState {
                key: f.key,
                label: f.label,
                value,
                state,
                severity,
                meaning,
                burned: state != "clean",
            }
        })
        .collect();
    let burned: Vec<EfuseFieldState> = fields.iter().filter(|f| f.burned).cloned().collect();
    let virgin = Some(burned.is_empty());
    EfuseScan {
        supported: true,
        chip: chip.to_string(),
        words: words[..EFUSE_BLOCK0_WORDS.min(words.len())].to_vec(),
        fields,
        burned,
        virgin,
    }
}

// ── the ROM serial protocol, read-only subset ────────────────────────────────
// SLIP framing and the command header, from the ESP serial-boot protocol the
// vendored esptool-js implements: a frame is 0xC0 <payload> 0xC0 with 0xC0 →
// 0xDB 0xDC and 0xDB → 0xDB 0xDD. A command is <dir=0><cmd><len u16 LE>
// <checksum u32 LE><data>; SYNC (0x08) and READ_REG (0x0A) both use
// checksum 0. A response is <dir=1><cmd><len u16 LE><value u32 LE><status…>,
// where the first status byte is the failure flag (ROM sends 4 status bytes,
// a stub 2 — we never upload a stub, but parsing only byte 0 covers both).

const SLIP_END: u8 = 0xc0;
const SLIP_ESC: u8 = 0xdb;
const SLIP_ESC_END: u8 = 0xdc;
const SLIP_ESC_ESC: u8 = 0xdd;

const CMD_SYNC: u8 = 0x08;
const CMD_READ_REG: u8 = 0x0a;

pub fn slip_encode(payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(payload.len() + 2);
    out.push(SLIP_END);
    for &b in payload {
        match b {
            SLIP_END => out.extend_from_slice(&[SLIP_ESC, SLIP_ESC_END]),
            SLIP_ESC => out.extend_from_slice(&[SLIP_ESC, SLIP_ESC_ESC]),
            _ => out.push(b),
        }
    }
    out.push(SLIP_END);
    out
}

/// Pull every complete SLIP frame out of `buf`, leaving any trailing partial
/// frame (and the noise before it) in place. ROM boot chatter between frames
/// is discarded with the empty frames it produces.
pub fn slip_take_frames(buf: &mut Vec<u8>) -> Vec<Vec<u8>> {
    let mut frames = Vec::new();
    loop {
        let Some(start) = buf.iter().position(|&b| b == SLIP_END) else {
            buf.clear(); // pure noise, no frame boundary yet
            return frames;
        };
        let Some(rel_end) = buf[start + 1..].iter().position(|&b| b == SLIP_END) else {
            // Partial frame: keep from the delimiter on, drop the noise before.
            buf.drain(..start);
            return frames;
        };
        let end = start + 1 + rel_end;
        let mut frame = Vec::with_capacity(end - start);
        let mut esc = false;
        for &b in &buf[start + 1..end] {
            if esc {
                frame.push(match b {
                    SLIP_ESC_END => SLIP_END,
                    SLIP_ESC_ESC => SLIP_ESC,
                    other => other,
                });
                esc = false;
            } else if b == SLIP_ESC {
                esc = true;
            } else {
                frame.push(b);
            }
        }
        buf.drain(..end + 1);
        if !frame.is_empty() {
            frames.push(frame);
        }
    }
}

pub fn command_frame(cmd: u8, data: &[u8]) -> Vec<u8> {
    let mut payload = Vec::with_capacity(8 + data.len());
    payload.push(0x00);
    payload.push(cmd);
    payload.extend_from_slice(&(data.len() as u16).to_le_bytes());
    payload.extend_from_slice(&0u32.to_le_bytes()); // checksum: only data-phase commands use it
    payload.extend_from_slice(data);
    slip_encode(&payload)
}

pub fn sync_frame() -> Vec<u8> {
    let mut data = vec![0x07, 0x07, 0x12, 0x20];
    data.extend_from_slice(&[0x55; 32]);
    command_frame(CMD_SYNC, &data)
}

pub fn read_reg_frame(addr: u32) -> Vec<u8> {
    command_frame(CMD_READ_REG, &addr.to_le_bytes())
}

/// Parse a response frame for `cmd`. Ok(value) on a success status; Err is a
/// frame that isn't ours or reports failure — the caller keeps reading.
pub fn parse_response(frame: &[u8], cmd: u8) -> Option<Result<u32, ()>> {
    if frame.len() < 8 || frame[0] != 0x01 || frame[1] != cmd {
        return None;
    }
    let value = u32::from_le_bytes([frame[4], frame[5], frame[6], frame[7]]);
    let status = &frame[8..];
    if status.first().copied() == Some(0) || (cmd == CMD_SYNC && status.is_empty()) {
        Some(Ok(value))
    } else {
        Some(Err(()))
    }
}

// ── talking to the ROM over a real port ──────────────────────────────────────

trait ResetLines {
    fn dtr(&mut self, level: bool) -> Result<(), String>;
    fn rts(&mut self, level: bool) -> Result<(), String>;
}

struct PortLines<'a> {
    port: &'a mut Box<dyn serialport::SerialPort>,
    dtr_state: bool,
}

impl ResetLines for PortLines<'_> {
    fn dtr(&mut self, level: bool) -> Result<(), String> {
        self.dtr_state = level;
        self.port
            .write_data_terminal_ready(level)
            .map_err(|e| format!("couldn't drive DTR: {e}"))
    }
    // esptool-js re-applies DTR after every RTS change (Transport.setRTS) —
    // a Windows CH34x driver quirk where setting RTS clobbers DTR. Mirror it.
    fn rts(&mut self, level: bool) -> Result<(), String> {
        self.port
            .write_request_to_send(level)
            .map_err(|e| format!("couldn't drive RTS: {e}"))?;
        let dtr = self.dtr_state;
        self.dtr(dtr)
    }
}

fn pause(ms: u64) {
    std::thread::sleep(Duration::from_millis(ms));
}

// The two enter-download-mode sequences, verbatim from the vendored
// esptool-js (ClassicReset with the 50 ms default delay, UsbJtagSerialReset).
fn classic_reset(lines: &mut dyn ResetLines) -> Result<(), String> {
    lines.dtr(false)?;
    lines.rts(true)?;
    pause(100);
    lines.dtr(true)?;
    lines.rts(false)?;
    pause(50);
    lines.dtr(false)
}

fn usb_jtag_serial_reset(lines: &mut dyn ResetLines) -> Result<(), String> {
    lines.rts(false)?;
    lines.dtr(false)?;
    pause(100);
    lines.dtr(true)?;
    lines.rts(false)?;
    pause(100);
    lines.rts(true)?;
    lines.dtr(false)?;
    lines.rts(true)?;
    pause(100);
    lines.rts(false)?;
    lines.dtr(false)
}

// Leave the board running its firmware again, not parked in the ROM.
fn hard_reset(lines: &mut dyn ResetLines) -> Result<(), String> {
    lines.rts(true)?;
    pause(100);
    lines.rts(false)
}

fn drain_input(port: &mut Box<dyn serialport::SerialPort>) {
    let _ = port.clear(serialport::ClearBuffer::Input);
}

/// Send one frame and collect responses to `cmd` until the deadline. Returns
/// the first success value.
fn transact(
    port: &mut Box<dyn serialport::SerialPort>,
    frame: &[u8],
    cmd: u8,
    deadline: Duration,
) -> Option<u32> {
    if port.write_all(frame).is_err() || port.flush().is_err() {
        return None;
    }
    let start = Instant::now();
    let mut pending: Vec<u8> = Vec::new();
    let mut chunk = [0u8; 256];
    while start.elapsed() < deadline {
        match port.read(&mut chunk) {
            Ok(n) if n > 0 => {
                pending.extend_from_slice(&chunk[..n]);
                for f in slip_take_frames(&mut pending) {
                    if let Some(Ok(value)) = parse_response(&f, cmd) {
                        return Some(value);
                    }
                }
            }
            // A timed-out read just means nothing arrived yet.
            _ => {}
        }
    }
    None
}

fn sync(port: &mut Box<dyn serialport::SerialPort>) -> bool {
    let frame = sync_frame();
    for _ in 0..5 {
        if transact(port, &frame, CMD_SYNC, Duration::from_millis(300)).is_some() {
            // The ROM answers a burst of sync echoes; let them land and drop
            // them so they can't be misread as the next command's response.
            pause(50);
            drain_input(port);
            return true;
        }
    }
    false
}

/// Read the six block-0 words off a real board: reset into the ROM (classic
/// bridge sequence first, the USB-Serial/JTAG sequence if that finds nothing),
/// SYNC, six READ_REGs, then hard-reset back into the firmware. Read-only at
/// every step — the two commands used cannot write anything.
fn read_block0_words(port_name: &str, efuse_base: u32) -> Result<Vec<u32>, String> {
    let mut port = serialport::new(port_name, 115_200)
        .timeout(Duration::from_millis(100))
        .open()
        .map_err(|e| format!("couldn't open {port_name}: {e}"))?;

    let mut synced = false;
    for attempt in 0..2 {
        {
            let mut lines = PortLines {
                port: &mut port,
                dtr_state: false,
            };
            let reset = if attempt == 0 {
                classic_reset(&mut lines)
            } else {
                usb_jtag_serial_reset(&mut lines)
            };
            reset?;
        }
        drain_input(&mut port);
        if sync(&mut port) {
            synced = true;
            break;
        }
    }
    if !synced {
        // Leave the board as we found it as best we can before reporting.
        let mut lines = PortLines {
            port: &mut port,
            dtr_state: false,
        };
        let _ = hard_reset(&mut lines);
        return Err(
            "couldn't reach the chip's ROM to read its fuses — the check did NOT run. \
                    Unplug, hold BOOT while plugging back in, and try again."
                .into(),
        );
    }

    let mut words = Vec::with_capacity(EFUSE_BLOCK0_WORDS);
    for addr in block0_addrs(efuse_base) {
        let frame = read_reg_frame(addr);
        match transact(&mut port, &frame, CMD_READ_REG, Duration::from_millis(500)) {
            Some(value) => words.push(value),
            None => {
                let mut lines = PortLines {
                    port: &mut port,
                    dtr_state: false,
                };
                let _ = hard_reset(&mut lines);
                return Err(format!(
                    "the ROM stopped answering at register {addr:#x} — fuses not checked"
                ));
            }
        }
    }

    let mut lines = PortLines {
        port: &mut port,
        dtr_state: false,
    };
    let _ = hard_reset(&mut lines);
    Ok(words)
}

/// The Tauri command: read and decode the chip's security fuses. `chip` is
/// the canonical spelling detect_chip stored; a chip without a verified table
/// answers `supported: false` without touching the port (same posture as the
/// browser, which skips the read when it has no table to decode it with).
#[tauri::command]
pub async fn read_security_efuses(port: String, chip: String) -> Result<EfuseScan, String> {
    let Some(base) = efuse_base(&chip) else {
        return Ok(decode_security_efuses(&chip, &[]));
    };
    tauri::async_runtime::spawn_blocking(move || {
        let words = read_block0_words(&port, base)?;
        Ok(decode_security_efuses(&chip, &words))
    })
    .await
    .map_err(|e| format!("fuse-read worker failed: {e}"))?
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn block0_addrs_start_at_the_read_offset_and_run_six_words() {
        let addrs = block0_addrs(0x6000_7000);
        assert_eq!(addrs.len(), EFUSE_BLOCK0_WORDS);
        assert_eq!(addrs[0], 0x6000_702c);
        assert_eq!(addrs[5], 0x6000_7040);
    }

    #[test]
    fn field_extraction_crosses_word_boundaries() {
        // A 4-bit field at bit 30 straddles words 0 and 1.
        let words = [0b11 << 30, 0b01, 0, 0, 0, 0];
        assert_eq!(efuse_field(&words, 30, 4), Some(0b0111));
        // Out-of-range reads refuse instead of inventing zeros.
        assert_eq!(efuse_field(&words[..1], 30, 4), None);
    }

    #[test]
    fn crypt_counter_reads_by_parity_not_value() {
        // SPI_BOOT_CRYPT_CNT at bit 82 (word 2, bits 18..21).
        let mut words = [0u32; 6];
        words[2] = 0b001 << 18; // one bit set → encryption ON
        let scan = decode_security_efuses("ESP32-S3", &words);
        let f = scan
            .fields
            .iter()
            .find(|f| f.key == "SPI_BOOT_CRYPT_CNT")
            .unwrap();
        assert_eq!(f.state, "active");
        assert_eq!(f.severity, Some("stop"));

        words[2] = 0b011 << 18; // on-then-off → touched, attention, not stop
        let scan = decode_security_efuses("ESP32-S3", &words);
        let f = scan
            .fields
            .iter()
            .find(|f| f.key == "SPI_BOOT_CRYPT_CNT")
            .unwrap();
        assert_eq!(f.state, "touched");
        assert_eq!(f.severity, Some("attention"));
        assert_eq!(scan.virgin, Some(false)); // touched is never folded into clean
    }

    #[test]
    fn a_factory_fresh_board_reads_virgin_and_a_burned_one_does_not() {
        let clean = decode_security_efuses("ESP32-C6", &[0u32; 6]);
        assert!(clean.supported);
        assert_eq!(clean.virgin, Some(true));
        assert!(clean.burned.is_empty());

        // SECURE_BOOT_EN at bit 116 (word 3, bit 20).
        let mut words = [0u32; 6];
        words[3] = 1 << 20;
        let locked = decode_security_efuses("ESP32-S3", &words);
        assert_eq!(locked.virgin, Some(false));
        let f = &locked.burned[0];
        assert_eq!(f.key, "SECURE_BOOT_EN");
        assert_eq!(f.state, "active");
        assert_eq!(f.severity, Some("stop"));
    }

    #[test]
    fn a_chip_without_a_verified_table_is_unsupported_not_guessed() {
        // Plain ESP32 has a different eFuse layout — the browser refuses to
        // guess (securityFieldsFor returns null) and so do we.
        let scan = decode_security_efuses("ESP32", &[0u32; 6]);
        assert!(!scan.supported);
        assert_eq!(scan.virgin, None);
        assert!(efuse_base("ESP32").is_none());
        assert!(efuse_base("ESP32-S3").is_some());
    }

    #[test]
    fn slip_round_trips_and_splits_frames_out_of_boot_noise() {
        let payload = vec![0x01, SLIP_END, 0x02, SLIP_ESC, 0x03];
        let mut wire = b"boot noise".to_vec();
        wire.extend_from_slice(&slip_encode(&payload));
        wire.extend_from_slice(b"more noise, no delimiter");
        let mut buf = wire;
        let frames = slip_take_frames(&mut buf);
        assert_eq!(frames, vec![payload.clone()]);
        // Trailing bytes with no delimiter are noise, not a partial frame.
        assert!(buf.is_empty());

        // A frame still open when the chunk ends waits for the rest.
        let mut partial = slip_encode(&payload);
        partial.extend_from_slice(&[SLIP_END, 0x09, 0x09]); // opened, unfinished
        let mut buf = partial;
        assert_eq!(slip_take_frames(&mut buf), vec![payload]);
        assert_eq!(buf, vec![SLIP_END, 0x09, 0x09]);
        buf.push(SLIP_END); // the rest arrives
        assert_eq!(slip_take_frames(&mut buf), vec![vec![0x09, 0x09]]);
    }

    #[test]
    fn command_frames_match_the_protocol_layout() {
        let f = read_reg_frame(0x6000_702c);
        // 0xC0, dir 0, cmd 0x0A, len 4, checksum 0, addr LE, 0xC0
        assert_eq!(f[0], SLIP_END);
        assert_eq!(&f[1..9], &[0x00, CMD_READ_REG, 0x04, 0x00, 0, 0, 0, 0]);
        assert_eq!(&f[9..13], &0x6000_702cu32.to_le_bytes());
        assert_eq!(*f.last().unwrap(), SLIP_END);

        let s = sync_frame();
        assert_eq!(s[2], CMD_SYNC);
        assert_eq!(s[3], 36); // 4 magic + 32× 0x55
        assert_eq!(&s[9..13], &[0x07, 0x07, 0x12, 0x20]);
    }

    #[test]
    fn responses_parse_value_and_refuse_failure_status() {
        // dir 1, cmd 0x0A, len, value, status ok (ROM sends 4 status bytes).
        let mut ok = vec![0x01, CMD_READ_REG, 0x04, 0x00];
        ok.extend_from_slice(&0xdead_beefu32.to_le_bytes());
        ok.extend_from_slice(&[0, 0, 0, 0]);
        assert_eq!(parse_response(&ok, CMD_READ_REG), Some(Ok(0xdead_beef)));

        let mut bad = ok.clone();
        bad[8] = 1; // failure flag
        assert_eq!(parse_response(&bad, CMD_READ_REG), Some(Err(())));

        // Someone else's frame is not an answer.
        assert_eq!(parse_response(&ok, CMD_SYNC), None);
        // A stub-style 2-byte status parses the same way (byte 0 is the flag).
        let short = [&ok[..8], &[0u8, 0u8][..]].concat();
        assert_eq!(parse_response(&short, CMD_READ_REG), Some(Ok(0xdead_beef)));
    }

    #[test]
    fn reset_sequences_mirror_the_vendored_esptool_js() {
        // Record the line transitions and compare against the bundle's
        // ClassicReset / UsbJtagSerialReset / HardReset sequences verbatim
        // (sleeps elided — order is what the ROM strapping cares about here,
        // and the real delays live in the functions under test).
        #[derive(Default)]
        struct Log(Vec<(char, bool)>);
        impl ResetLines for Log {
            fn dtr(&mut self, level: bool) -> Result<(), String> {
                self.0.push(('D', level));
                Ok(())
            }
            fn rts(&mut self, level: bool) -> Result<(), String> {
                self.0.push(('R', level));
                Ok(())
            }
        }
        let mut log = Log::default();
        classic_reset(&mut log).unwrap();
        assert_eq!(
            log.0,
            vec![
                ('D', false),
                ('R', true),
                ('D', true),
                ('R', false),
                ('D', false)
            ]
        );

        let mut log = Log::default();
        usb_jtag_serial_reset(&mut log).unwrap();
        assert_eq!(
            log.0,
            vec![
                ('R', false),
                ('D', false),
                ('D', true),
                ('R', false),
                ('R', true),
                ('D', false),
                ('R', true),
                ('R', false),
                ('D', false),
            ]
        );

        let mut log = Log::default();
        hard_reset(&mut log).unwrap();
        assert_eq!(log.0, vec![('R', true), ('R', false)]);
    }
}
