//! Native Grove Vision AI V2 (Himax WiseEye2) module flasher.
//!
//! This mirrors the browser Lab's host-tested wire protocol: CH343 serial at
//! 921600 baud, the ROM burn menu, XMODEM/CRC-16, a non-zero-address preamble,
//! and SSCMA AT proof after reboot.

use base64::Engine as _;
use serde::Serialize;
use serde_json::{json, Value};
use serialport::{ClearBuffer, SerialPort};
use std::io::{Read, Write};
use std::time::{Duration, Instant};

pub const USB_VID: u16 = 0x1a86;
pub const USB_PID: u16 = 0x55d3;
pub const BAUD: u32 = 921_600;
pub const MODEL_ADDR: u32 = 0x0040_0000;

const SOH: u8 = 0x01;
const EOT: u8 = 0x04;
const ACK: u8 = 0x06;
const CAN: u8 = 0x18;
const CRC_START: u8 = b'C';
const FILLER: u8 = 0x1a;
const BLOCK: usize = 128;
const MENU_LINE: &str = "Xmodem download and burn FW image";
const PROMPT_LINE: &str = "Do you want to end file transmission and reboot system";

pub fn is_module_usb(vid: Option<u16>, pid: Option<u16>) -> bool {
    vid == Some(USB_VID) && pid == Some(USB_PID)
}

pub fn crc16_xmodem(bytes: &[u8]) -> u16 {
    let mut crc = 0u16;
    for byte in bytes {
        crc ^= u16::from(*byte) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 {
                (crc << 1) ^ 0x1021
            } else {
                crc << 1
            };
        }
    }
    crc
}

pub fn xmodem_packet(block_number: u8, data: &[u8; BLOCK]) -> [u8; 133] {
    let mut packet = [0u8; 133];
    packet[0] = SOH;
    packet[1] = block_number;
    packet[2] = 0xff - block_number;
    packet[3..131].copy_from_slice(data);
    let crc = crc16_xmodem(data);
    packet[131] = (crc >> 8) as u8;
    packet[132] = crc as u8;
    packet
}

pub fn preamble_block(address: u32, offset: u32) -> [u8; BLOCK] {
    let mut block = [0xff; BLOCK];
    block[0] = 0xc0;
    block[1] = 0x5a;
    block[2..6].copy_from_slice(&address.to_le_bytes());
    block[6..10].copy_from_slice(&offset.to_le_bytes());
    block[10] = 0x5a;
    block[11] = 0xc0;
    block
}

#[derive(Serialize)]
pub struct ModuleReceipt {
    pub target: &'static str,
    pub version: String,
    pub sha256: String,
    pub bytes_written: usize,
    pub flash_address: String,
    pub at_version: Option<Value>,
    pub module_id: Option<Value>,
    pub inference_ok: bool,
    pub boxes: Vec<Value>,
    pub preview_image: Option<String>,
}

struct Flasher<L, P>
where
    L: FnMut(String),
    P: FnMut(f64),
{
    port: Box<dyn SerialPort>,
    log: L,
    progress: P,
}

impl<L, P> Flasher<L, P>
where
    L: FnMut(String),
    P: FnMut(f64),
{
    fn new(port_name: &str, log: L, progress: P) -> Result<Self, String> {
        let port = serialport::new(port_name, BAUD)
            .timeout(Duration::from_millis(40))
            .open()
            .map_err(|e| {
                let mut message = format!("could not open the Vision module port: {e}");
                // Linux-only: permission-denied / busy opens have a Linux
                // fix the raw errno string doesn't teach (dialout/udev rule,
                // ModemManager) — append it so the error card is actionable.
                if cfg!(target_os = "linux") {
                    if let Some(hint) = crate::port_hint::linux_open_hint(&message) {
                        message = format!("{message}\n{hint}");
                    }
                }
                message
            })?;
        Ok(Self {
            port,
            log,
            progress,
        })
    }

    fn hard_reset(&mut self) -> Result<(), String> {
        self.port
            .write_request_to_send(false)
            .map_err(|e| format!("could not assert module reset: {e}"))?;
        std::thread::sleep(Duration::from_millis(100));
        self.port
            .write_request_to_send(true)
            .map_err(|e| format!("could not release module reset: {e}"))?;
        Ok(())
    }

    fn write_all(&mut self, bytes: &[u8]) -> Result<(), String> {
        self.port
            .write_all(bytes)
            .map_err(|e| format!("module serial write failed: {e}"))?;
        self.port
            .flush()
            .map_err(|e| format!("module serial flush failed: {e}"))
    }

    fn read_byte_until(&mut self, deadline: Instant) -> Result<Option<u8>, String> {
        let mut byte = [0u8; 1];
        while Instant::now() < deadline {
            match self.port.read(&mut byte) {
                Ok(1) => return Ok(Some(byte[0])),
                Ok(_) => {}
                Err(error) if error.kind() == std::io::ErrorKind::TimedOut => {}
                Err(error) => return Err(format!("module serial read failed: {error}")),
            }
        }
        Ok(None)
    }

    fn read_until(
        &mut self,
        needle: &str,
        timeout: Duration,
        drip: Option<(Duration, u8)>,
    ) -> Result<String, String> {
        let deadline = Instant::now() + timeout;
        let mut next_drip = Instant::now();
        let mut bytes = Vec::new();
        while Instant::now() < deadline {
            if let Some((interval, byte)) = drip {
                if Instant::now() >= next_drip {
                    self.write_all(&[byte])?;
                    next_drip = Instant::now() + interval;
                }
            }
            if let Some(byte) =
                self.read_byte_until((Instant::now() + Duration::from_millis(8)).min(deadline))?
            {
                bytes.push(byte);
                if bytes.ends_with(needle.as_bytes()) {
                    break;
                }
                if bytes.len() > 64 * 1024 {
                    bytes.drain(..32 * 1024);
                }
            }
        }
        Ok(String::from_utf8_lossy(&bytes).into_owned())
    }

    fn enter_bootloader(&mut self) -> Result<(), String> {
        (self.log)("resetting the Grove Vision AI V2 module…".into());
        self.hard_reset()?;
        let _ = self.port.clear(ClearBuffer::Input);
        (self.log)("catching the WiseEye2 ROM burn menu…".into());
        let menu = self.read_until(
            MENU_LINE,
            Duration::from_millis(2500),
            Some((Duration::from_millis(10), b'1')),
        )?;
        if !menu.contains(MENU_LINE) {
            return Err(
                "no WiseEye2 burn menu — use the module's USB-C port (the CH343), not the XIAO port"
                    .into(),
            );
        }
        self.write_all(b"1")?;
        std::thread::sleep(Duration::from_millis(100));
        let _ = self.port.clear(ClearBuffer::Input);
        (self.log)("ROM bootloader ready — burn option selected".into());
        Ok(())
    }

    fn wait_start(&mut self) -> Result<(), String> {
        let deadline = Instant::now() + Duration::from_secs(60);
        while let Some(byte) = self.read_byte_until(deadline)? {
            if byte == CRC_START {
                return Ok(());
            }
            if byte == CAN {
                return Err("module bootloader canceled the transfer".into());
            }
        }
        Err("module bootloader never invited the XMODEM transfer ('C')".into())
    }

    fn xmodem_send(&mut self, bytes: &[u8], base: f64, span: f64) -> Result<(), String> {
        self.wait_start()?;
        let block_count = bytes.len().max(1).div_ceil(BLOCK);
        let mut block_number = 1u8;
        let mut index = 0usize;
        let mut errors = 0u8;
        let mut last_percent = 0usize;
        while index < block_count {
            let mut block = [FILLER; BLOCK];
            let start = index * BLOCK;
            if start < bytes.len() {
                let end = (start + BLOCK).min(bytes.len());
                block[..end - start].copy_from_slice(&bytes[start..end]);
            }
            let packet = xmodem_packet(block_number, &block);
            self.write_all(&packet)?;
            match self.read_byte_until(Instant::now() + Duration::from_secs(60))? {
                Some(ACK) => {
                    index += 1;
                    block_number = block_number.wrapping_add(1);
                    errors = 0;
                    let percent = index * 100 / block_count;
                    if percent != last_percent {
                        last_percent = percent;
                        (self.progress)(base + span * index as f64 / block_count as f64);
                    }
                }
                Some(CAN) => return Err("module bootloader canceled the transfer".into()),
                _ => {
                    errors = errors.saturating_add(1);
                    if errors > 30 {
                        return Err("too many XMODEM errors — check the cable and retry".into());
                    }
                }
            }
        }

        for _ in 0..=10 {
            self.write_all(&[EOT])?;
            if self.read_byte_until(Instant::now() + Duration::from_secs(60))? == Some(ACK) {
                return Ok(());
            }
        }
        Err("module bootloader never acknowledged end of transfer".into())
    }

    fn answer_prompt(&mut self, answer: u8) -> Result<(), String> {
        let prompt = self.read_until(PROMPT_LINE, Duration::from_secs(10), None)?;
        if !prompt.contains(PROMPT_LINE) {
            return Err("module bootloader never produced its finish prompt".into());
        }
        self.write_all(&[answer])
    }

    fn flash_model(&mut self, bytes: &[u8]) -> Result<(), String> {
        self.enter_bootloader()?;
        (self.log)(format!("aiming the burn at 0x{MODEL_ADDR:08x}…"));
        self.xmodem_send(&preamble_block(MODEL_ADDR, 0), 0.0, 0.0)?;
        self.answer_prompt(b'n')?;
        let _ = self.port.clear(ClearBuffer::Input);
        (self.log)(format!("sending {} verified model bytes…", bytes.len()));
        self.xmodem_send(bytes, 0.0, 1.0)?;
        self.answer_prompt(b'y')?;
        (self.log)("model burned — rebooting the module for AT proof…".into());
        std::thread::sleep(Duration::from_secs(2));
        self.hard_reset()?;
        std::thread::sleep(Duration::from_millis(1200));
        let _ = self.port.clear(ClearBuffer::Input);
        Ok(())
    }

    fn at_command(&mut self, body: &str, timeout: Duration) -> Result<Option<Value>, String> {
        self.write_all(format!("AT+{body}\r").as_bytes())?;
        let expected = body.split('=').next().unwrap_or(body);
        self.read_reply(0, expected, body, Instant::now() + timeout)
    }

    // Scan the wire for one matching SSCMA JSON frame. `kind` 0 is a command
    // ack; `kind` 1 is an EVENT — detection results (boxes, and the image when
    // INVOKE's result_only flag is 0) arrive ONLY as type-1 events after the
    // ack (guide §8), so a caller that needs what the model saw must read for
    // type 1: the ack's data never contains it.
    fn read_reply(
        &mut self,
        kind_want: i64,
        expected: &str,
        body: &str,
        deadline: Instant,
    ) -> Result<Option<Value>, String> {
        let mut frame = Vec::new();
        let mut collecting = false;
        while Instant::now() < deadline {
            let Some(byte) = self.read_byte_until(deadline)? else {
                break;
            };
            if !collecting {
                if byte == b'\r' {
                    frame.clear();
                    collecting = true;
                }
                continue;
            }
            if frame.is_empty() && byte != b'{' {
                collecting = byte == b'\r';
                continue;
            }
            frame.push(byte);
            if byte == b'\n' && frame.get(frame.len().saturating_sub(2)) == Some(&b'}') {
                let json_bytes = &frame[..frame.len() - 1];
                if let Ok(value) = serde_json::from_slice::<Value>(json_bytes) {
                    let kind = value.get("type").and_then(Value::as_i64);
                    let name = value.get("name").and_then(Value::as_str).unwrap_or("");
                    if kind == Some(kind_want)
                        && (name == expected || name == body || body.starts_with(name))
                    {
                        return Ok(Some(value));
                    }
                }
                frame.clear();
                collecting = false;
            }
            if frame.len() > 2 * 1024 * 1024 {
                frame.clear();
                collecting = false;
            }
        }
        Ok(None)
    }

    // Run one INVOKE (with image) and return its type-1 detection event.
    // Frame-scans like read_reply, but judges both SSCMA frame types in a
    // single pass — see the call site for why two sequential scans lose.
    fn invoke_event(&mut self, deadline: Instant) -> Result<Value, String> {
        self.write_all(b"AT+INVOKE=1,0,0\r")?;
        let mut frame = Vec::new();
        let mut collecting = false;
        let mut acked = false;
        while Instant::now() < deadline {
            let Some(byte) = self.read_byte_until(deadline)? else {
                break;
            };
            if !collecting {
                if byte == b'\r' {
                    frame.clear();
                    collecting = true;
                }
                continue;
            }
            if frame.is_empty() && byte != b'{' {
                collecting = byte == b'\r';
                continue;
            }
            frame.push(byte);
            if byte == b'\n' && frame.get(frame.len().saturating_sub(2)) == Some(&b'}') {
                let json_bytes = &frame[..frame.len() - 1];
                if let Ok(value) = serde_json::from_slice::<Value>(json_bytes) {
                    let kind = value.get("type").and_then(Value::as_i64);
                    if value.get("name").and_then(Value::as_str) == Some("INVOKE") {
                        match (kind, at_ok(&value)) {
                            (Some(1), true) => return Ok(value),
                            (Some(0), true) => acked = true,
                            (Some(0), false) => {
                                return Err(
                                    "SSCMA answered AT, but the pinned model did not accept INVOKE"
                                        .to_string(),
                                )
                            }
                            _ => {}
                        }
                    }
                }
                frame.clear();
                collecting = false;
            }
            if frame.len() > 2 * 1024 * 1024 {
                frame.clear();
                collecting = false;
            }
        }
        Err(if acked {
            "the module accepted INVOKE but its detection event never arrived".to_string()
        } else {
            "SSCMA answered AT, but the pinned model did not complete an inference".to_string()
        })
    }

    fn prove(
        &mut self,
        version: &str,
        sha256: &str,
    ) -> Result<(Option<Value>, Option<Value>, Value), String> {
        // The proof runs up to ~25 s (VER?/ID?/INFO plus one real inference)
        // with nothing else printing — say so before going quiet.
        (self.log)("asking the module to prove itself — VER/ID/INFO + one live inference (up to ~25 s)…".into());
        let version_reply = self
            .at_command("VER?", Duration::from_secs(4))?
            .filter(at_ok);
        if version_reply.is_none() {
            return Err(
                "model burn completed, but SSCMA did not answer AT+VER? after reboot".into(),
            );
        }
        let id_reply = self
            .at_command("ID?", Duration::from_secs(4))?
            .filter(at_ok);
        let info = json!({
            "name": "Person Detection",
            "version": version,
            "category": "Object Detection",
            "algorithm": "Swift-YOLO (tiny)",
            "description": "SecuraCV-pinned person model (SSCMA model zoo, MIT)",
            "classes": ["person"],
            "model_type": "TFLite (Vela, Ethos-U55)",
            "sha256": sha256,
            "source": "securacv.com/lab — vendored from Seeed SSCMA model zoo"
        });
        let info_base64 = base64::engine::general_purpose::STANDARD.encode(info.to_string());
        let _ = self.at_command(&format!("INFO=\"{info_base64}\""), Duration::from_secs(3))?;
        // INVOKE=<times>,<differed>,<result_only> — the third arg MUST be 0
        // here: 1 means "no image", and this one attended frame is the app's
        // entire detection preview (there is no live bench on the desktop, so
        // with =1 the preview element simply never appeared). The event grows
        // by one base64 JPEG (~tens of KB at 921600 baud), well inside the
        // timeout. Guide: docs/hardware/grove_vision_ai_v2_guide.md §8.
        // The detection itself — boxes and the preview frame — is the type-1
        // EVENT; a type-0 ack MAY precede it, depending on the SSCMA firmware
        // build (the browser bench documents the same: success is EITHER
        // signal, we2-flash.js). One scan, so an event that arrives without
        // an ack is never consumed and discarded while waiting for one: an ok
        // ack means keep reading, an error ack fails fast, the event wins.
        let inference = self.invoke_event(Instant::now() + Duration::from_secs(8))?;
        Ok((version_reply, id_reply, inference))
    }
}

fn at_ok(value: &Value) -> bool {
    value.get("code").and_then(Value::as_i64) == Some(0)
}

pub fn flash_and_prove<L, P>(
    port_name: &str,
    model: &[u8],
    version: String,
    sha256: String,
    log: L,
    progress: P,
) -> Result<ModuleReceipt, String>
where
    L: FnMut(String),
    P: FnMut(f64),
{
    let mut flasher = Flasher::new(port_name, log, progress)?;
    flasher.flash_model(model)?;
    let (at_version, module_id, inference) = flasher.prove(&version, &sha256)?;
    let data = inference.get("data").cloned().unwrap_or(Value::Null);
    let boxes = data
        .get("boxes")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    let preview_image = data
        .get("image")
        .and_then(Value::as_str)
        .map(ToOwned::to_owned);
    Ok(ModuleReceipt {
        target: "grove-vision-ai-v2",
        version,
        sha256,
        bytes_written: model.len(),
        flash_address: format!("0x{MODEL_ADDR:08x}"),
        at_version,
        module_id,
        inference_ok: true,
        boxes,
        preview_image,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn usb_identity_is_exact() {
        assert!(is_module_usb(Some(0x1a86), Some(0x55d3)));
        assert!(!is_module_usb(Some(0x1a86), Some(0x55d4)));
        assert!(!is_module_usb(None, None));
    }

    #[test]
    fn crc_matches_xmodem_check_vector() {
        assert_eq!(crc16_xmodem(b"123456789"), 0x31c3);
    }

    #[test]
    fn packet_and_preamble_match_browser_engine() {
        let data = [0x41; BLOCK];
        let packet = xmodem_packet(7, &data);
        assert_eq!(&packet[..3], &[SOH, 7, 248]);
        assert_eq!(&packet[3..131], &data);
        let preamble = preamble_block(MODEL_ADDR, 0);
        assert_eq!(
            &preamble[..12],
            &[0xc0, 0x5a, 0x00, 0x00, 0x40, 0x00, 0, 0, 0, 0, 0x5a, 0xc0]
        );
        assert!(preamble[12..].iter().all(|b| *b == 0xff));
    }
}
