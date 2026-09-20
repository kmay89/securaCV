//! broker_receipt — the one line a flasher's receipt says about the broker
//! TLS mode it SEALED into the image. Sealed, never connected: the firmware
//! decides the transport at connect time (firmware/common/network/
//! mqtt_transport_logic.h) and the boot self-manifest carries no transport
//! field, so a flasher can vouch for the bytes it wrote and nothing more.
//!
//! Four single-line templates keyed by the firmware's own mode byte
//! (mqtt_transport_logic.h `Mode`: 0 plain, 1 CA, 2 fingerprint, 3 lab — do
//! not renumber). The SAME four strings live in
//! canary-local/assets/flash-core.js `MQTT_TLS_RECEIPT` for the browser
//! flasher (the two flashers share no UI code, CLAUDE.md), and
//! canary-local/tests/desktop_parity.test.js reads this file's text and
//! holds them equal — so each literal stays on ONE line.
//!
//! What a line may carry: the mode's name; for the CA mode the byte count of
//! the certificate as sealed (the trimmed PEM plus the newline build_nvs
//! appends — the count, never the PEM); for the pin mode the sealed SHA-256
//! fingerprint verbatim, which is public data (the broker presents that
//! certificate to every client on the LAN, and printing the pin lets the
//! owner compare it against `openssl x509 -noout -fingerprint -sha256`).
//! Never a password, never the host. The formatter takes a length and a
//! fingerprint, so a certificate cannot reach it by accident.
//!
//! std-only on purpose: CI's `tauri-pure-modules` leg compiles it with
//! `rustc --edition 2021 --test`, which also runs on a checkout without the
//! crate's GTK/WebKit dependencies.

/// The receipt table, indexed by the firmware mode byte. `{N}` is the sealed
/// CA's byte count; `{FP}` the sealed fingerprint.
pub(crate) const MODE_LABELS: [&str; 4] = [
    "plain MQTT — not encrypted (the default every Canary shipped with)",
    "TLS, CA-verified — CA certificate sealed, {N} bytes of PEM",
    "TLS, SHA-256 fingerprint pin — {FP}",
    "TLS, lab only — encrypted but NOT verified; the board warns on every connect",
];

/// A mode byte outside the table. validate() refuses one before anything is
/// sealed, so this names the firmware's own answer (Reason::ModeUnknown)
/// rather than borrowing a row. `{M}` is the byte.
pub(crate) const MODE_UNKNOWN: &str =
    "TLS mode byte {M} — not one the firmware knows; it refuses to connect until reprovisioned";

/// The line for what was sealed: `mode` is the NVS `mqtt_tls` byte (0 when
/// none was written), `ca_sealed_len` the byte count of the `mqtt_ca` string
/// as written (0 when none), `fp` the `mqtt_fp` string as written (empty when
/// none).
pub(crate) fn broker_tls_receipt(mode: u8, ca_sealed_len: usize, fp: &str) -> String {
    let Some(template) = MODE_LABELS.get(usize::from(mode)) else {
        return MODE_UNKNOWN.replace("{M}", &mode.to_string());
    };
    template
        .replace("{N}", &ca_sealed_len.to_string())
        .replace("{FP}", fp)
}

#[cfg(test)]
mod tests {
    use super::*;

    // The same fixtures as provisioning.rs and canary-local/tests/flash.test.js:
    // a 58-byte PEM (59 once build_nvs appends its newline) and a 64-hex pin.
    const PEM: &str = "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----";
    const FP: &str = "0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9";

    #[test]
    fn plain_names_the_default_and_says_not_encrypted() {
        let line = broker_tls_receipt(0, 0, "");
        assert_eq!(line, MODE_LABELS[0]);
        assert!(line.contains("not encrypted"));
    }

    #[test]
    fn ca_mode_counts_the_sealed_bytes_and_never_prints_the_pem() {
        let sealed = format!("{}\n", PEM.trim());
        assert_eq!(sealed.len(), 59);
        let line = broker_tls_receipt(1, sealed.len(), "");
        assert_eq!(line, "TLS, CA-verified — CA certificate sealed, 59 bytes of PEM");
        assert!(!line.contains("BEGIN CERTIFICATE") && !line.contains("MIIB"));
        assert!(!line.contains("{N}") && !line.contains("{FP}"));
    }

    #[test]
    fn fingerprint_mode_prints_the_sealed_pin_verbatim() {
        let mixed = FP.to_uppercase().replacen('A', "a", 1);
        let line = broker_tls_receipt(2, 0, &mixed);
        assert_eq!(line, format!("TLS, SHA-256 fingerprint pin — {mixed}"));
        assert!(line.contains(&mixed), "case as sealed, no separators added");
        assert!(!line.contains("{FP}"));
    }

    #[test]
    fn lab_mode_keeps_both_warnings() {
        let line = broker_tls_receipt(3, 0, "");
        assert_eq!(line, MODE_LABELS[3]);
        assert!(line.contains("NOT verified"));
        assert!(line.contains("warns on every connect"));
    }

    #[test]
    fn table_is_four_single_lines_and_an_unknown_byte_says_so() {
        for (i, label) in MODE_LABELS.iter().enumerate() {
            assert!(!label.contains('\n'), "row {i} must stay one line (desktop_parity reads the source)");
            assert!(!label.contains("connected"), "row {i} describes what was sealed, never a connection");
        }
        assert!(MODE_LABELS[1].contains("{N}") && !MODE_LABELS[1].contains("{FP}"));
        assert!(MODE_LABELS[2].contains("{FP}") && !MODE_LABELS[2].contains("{N}"));
        let line = broker_tls_receipt(9, 0, "");
        assert_eq!(
            line,
            "TLS mode byte 9 — not one the firmware knows; it refuses to connect until reprovisioned"
        );
        assert!(!line.contains("{M}"));
    }
}
