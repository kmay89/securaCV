//! The OS's own serial-port list — no Web Serial permission prompt, no
//! Chromium. One wire shape for both apps (the Flasher's `list_ports`, the
//! Lab's `list_serial_ports` and its `list_ports` twin), so a port picker
//! written against either app reads the other's answer unchanged.

use serde::Serialize;

/// A USB serial port as the OS sees it — enough for the UI to show a friendly
/// picker without pretending to know more than it does.
#[derive(Serialize)]
pub struct PortDto {
    /// OS port path, e.g. `/dev/tty.usbmodem1101` or `/dev/ttyACM0`.
    name: String,
    /// "usb" | "bluetooth" | "pci" | "unknown" — USB is what a Canary is.
    kind: String,
    vid: Option<u16>,
    pid: Option<u16>,
    product: Option<String>,
    manufacturer: Option<String>,
}

/// Serial ports the OS can see this instant.
pub fn list_ports() -> Result<Vec<PortDto>, String> {
    let ports =
        serialport::available_ports().map_err(|e| format!("could not list serial ports: {e}"))?;
    let mut out = Vec::new();
    for p in ports {
        use serialport::SerialPortType::*;
        let (kind, vid, pid, product, manufacturer) = match &p.port_type {
            UsbPort(info) => (
                "usb",
                Some(info.vid),
                Some(info.pid),
                info.product.clone(),
                info.manufacturer.clone(),
            ),
            BluetoothPort => ("bluetooth", None, None, None, None),
            PciPort => ("pci", None, None, None, None),
            Unknown => ("unknown", None, None, None, None),
        };
        out.push(PortDto {
            name: p.port_name,
            kind: kind.to_string(),
            vid,
            pid,
            product,
            manufacturer,
        });
    }
    Ok(out)
}
