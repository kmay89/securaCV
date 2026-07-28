//! hub_seed — the "type your Wi-Fi and it just works" seed for the boot partition.
//!
//! Step 5's core. Home Assistant OS reads NetworkManager keyfiles dropped into
//! `CONFIG/network/` on the boot partition at first boot; a valid one there is
//! the whole difference between a headless Pi that joins your Wi-Fi on its own
//! and one that needs a monitor and keyboard. This module builds that keyfile
//! from the SSID + passphrase the flasher already collects — pure and
//! host-tested, so the format is right by construction and the secret is handled
//! locally (it goes onto the card, never to a cloud).
//!
//! Two deliberate robustness choices:
//!   * The SSID is emitted as a NetworkManager **byte array** (decimal bytes), so
//!     any SSID — spaces, punctuation, emoji — is carried exactly, with none of
//!     the escaping ambiguity a bare string value invites.
//!   * The passphrase is validated to the WPA-PSK rules up front (an 8–63
//!     character passphrase, or a 64-hex raw PMK), so we never write a keyfile
//!     that can't authenticate; the one character that needs escaping in a
//!     keyfile value (`\`) and a leading space are handled.
//!
//! Two more choices that make the headless (no monitor, no keyboard) promise
//! hold end-to-end:
//!   * The `[connection]` section carries `llmnr=2` and `mdns=2` — the same
//!     values HAOS's own default wired profile sets — so the hub *answers*
//!     `homeassistant.local` on the Wi-Fi link at the OS-resolver level, not
//!     only once Home Assistant Core's zeroconf is up.
//!   * The caller should pass a freshly minted UUID4 (`hub_io::seed::uuid_v4`
//!     mints one): HA's docs warn that without a stable `uuid=` the profile is
//!     re-imported with a new identity each boot and "the IP address changes
//!     on every boot" — exactly what a headless user can't chase.
//!
//! The placement this feeds is HA-documented, not folklore: the OS
//! configuration docs say a USB stick named CONFIG works, and "Alternative you
//! can create a `CONFIG` folder inside the `boot` partition", read on startup
//! (developers.home-assistant.io/docs/operating-system/configuration). Files
//! must use UNIX (LF) line endings — everything this module emits is LF-only.
//! End-to-end acceptance on a specific HAOS build is still confirmed by a real
//! flash — this crate tests the generation, not the boot.

/// The inputs for a Wi-Fi keyfile. Borrowed strings — the caller owns them.
#[derive(Debug, Clone)]
pub struct WifiSeed<'a> {
    /// The network name. 1–32 bytes (the 802.11 SSID limit).
    pub ssid: &'a str,
    /// The WPA-PSK passphrase (8–63 chars) or a 64-hex raw PMK.
    pub passphrase: &'a str,
    /// The connection id — the keyfile's `id=` and the suggested filename stem.
    pub connection_id: &'a str,
    /// A pre-generated UUID, or `None` to omit it (NetworkManager assigns one on
    /// load). `hub-core` is dependency-free, so it will not mint one itself —
    /// callers should pass one (`hub_io::seed::uuid_v4`): HA's docs warn that a
    /// boot-partition profile without a stable `uuid=` gets a fresh identity on
    /// every boot's re-import, so the hub's IP can change each boot.
    pub uuid: Option<&'a str>,
    /// The network hides its SSID (no beacon) — adds `hidden=true` so the Pi
    /// probes for it actively.
    pub hidden: bool,
}

/// Why a [`WifiSeed`] can't be turned into a keyfile. Every case is a reason the
/// resulting network would silently fail to connect — better a clear error at
/// flash time than a Pi that never appears.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum WifiSeedError {
    EmptySsid,
    /// The SSID exceeds the 32-byte 802.11 limit.
    SsidTooLong {
        bytes: usize,
    },
    EmptyConnectionId,
    /// A passphrase shorter than the 8-character WPA minimum.
    PassphraseTooShort {
        len: usize,
    },
    /// A passphrase longer than 63 chars that also isn't a 64-hex raw PMK.
    PassphraseTooLong {
        len: usize,
    },
    /// A WPA passphrase must be printable ASCII (0x20–0x7e).
    PassphraseNotAscii,
}

impl WifiSeedError {
    pub fn message(&self) -> String {
        match self {
            WifiSeedError::EmptySsid => "the Wi-Fi network name (SSID) is empty".to_string(),
            WifiSeedError::SsidTooLong { bytes } => {
                format!("the SSID is {bytes} bytes — the Wi-Fi limit is 32")
            }
            WifiSeedError::EmptyConnectionId => "the connection id is empty".to_string(),
            WifiSeedError::PassphraseTooShort { len } => {
                format!("the Wi-Fi password is {len} characters — WPA needs at least 8")
            }
            WifiSeedError::PassphraseTooLong { len } => {
                format!("the Wi-Fi password is {len} characters — WPA allows at most 63 (or a 64-hex key)")
            }
            WifiSeedError::PassphraseNotAscii => {
                "the Wi-Fi password has non-ASCII characters, which WPA passphrases can't carry"
                    .to_string()
            }
        }
    }
}

fn is_hex64(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit())
}

/// Validate the passphrase to the WPA-PSK rules: a 64-hex raw PMK, or an 8–63
/// character passphrase of printable ASCII.
fn validate_passphrase(psk: &str) -> Result<(), WifiSeedError> {
    if is_hex64(psk) {
        return Ok(());
    }
    let len = psk.chars().count();
    if len < 8 {
        return Err(WifiSeedError::PassphraseTooShort { len });
    }
    if len > 63 {
        return Err(WifiSeedError::PassphraseTooLong { len });
    }
    // Printable ASCII only (0x20 space .. 0x7e ~). Anything else can't ride a WPA
    // passphrase and would also break the keyfile line.
    if !psk.bytes().all(|b| (0x20..=0x7e).contains(&b)) {
        return Err(WifiSeedError::PassphraseNotAscii);
    }
    Ok(())
}

/// Escape a value for a NetworkManager keyfile line. For our validated
/// printable-ASCII inputs the only cases are a backslash (doubled) and a leading
/// space (which the keyfile format would otherwise swallow), rendered as `\s`.
fn escape_value(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    for (i, c) in s.char_indices() {
        match c {
            '\\' => out.push_str("\\\\"),
            ' ' if i == 0 => out.push_str("\\s"),
            _ => out.push(c),
        }
    }
    out
}

/// The SSID as a NetworkManager byte array: `104;111;109;101;` — each UTF-8 byte
/// in decimal, semicolon-separated, with the trailing semicolon the format wants.
fn ssid_bytes(ssid: &str) -> String {
    let mut out = String::new();
    for b in ssid.as_bytes() {
        out.push_str(&b.to_string());
        out.push(';');
    }
    out
}

/// Build the NetworkManager keyfile HAOS reads from `CONFIG/network/<id>` at
/// first boot. Pure; validates the inputs, then renders the documented sections.
pub fn wifi_keyfile(seed: &WifiSeed) -> Result<String, WifiSeedError> {
    if seed.ssid.is_empty() {
        return Err(WifiSeedError::EmptySsid);
    }
    if seed.ssid.len() > 32 {
        return Err(WifiSeedError::SsidTooLong {
            bytes: seed.ssid.len(),
        });
    }
    if seed.connection_id.is_empty() {
        return Err(WifiSeedError::EmptyConnectionId);
    }
    validate_passphrase(seed.passphrase)?;

    let mut k = String::new();
    k.push_str("[connection]\n");
    k.push_str(&format!("id={}\n", escape_value(seed.connection_id)));
    if let Some(uuid) = seed.uuid {
        k.push_str(&format!("uuid={uuid}\n"));
    }
    k.push_str("type=802-11-wireless\n");
    // Announce-and-respond on LLMNR and mDNS for this connection — the values
    // HAOS's own default wired profile uses. Without them a Wi-Fi-only hub can
    // sit healthy on the network yet never answer `homeassistant.local`, which
    // to a headless user is indistinguishable from a dead flash.
    k.push_str("llmnr=2\n");
    k.push_str("mdns=2\n\n");

    k.push_str("[802-11-wireless]\n");
    k.push_str("mode=infrastructure\n");
    k.push_str(&format!("ssid={}\n", ssid_bytes(seed.ssid)));
    if seed.hidden {
        k.push_str("hidden=true\n");
    }
    k.push('\n');

    k.push_str("[802-11-wireless-security]\n");
    k.push_str("auth-alg=open\n");
    k.push_str("key-mgmt=wpa-psk\n");
    k.push_str(&format!("psk={}\n\n", escape_value(seed.passphrase)));

    k.push_str("[ipv4]\n");
    k.push_str("method=auto\n\n");

    k.push_str("[ipv6]\n");
    k.push_str("addr-gen-mode=stable-privacy\n");
    k.push_str("method=auto\n");

    Ok(k)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn seed<'a>(ssid: &'a str, psk: &'a str) -> WifiSeed<'a> {
        WifiSeed {
            ssid,
            passphrase: psk,
            connection_id: "securacv-hub",
            uuid: None,
            hidden: false,
        }
    }

    #[test]
    fn a_basic_network_renders_all_the_sections() {
        let k = wifi_keyfile(&seed("home", "supersecret")).unwrap();
        for section in [
            "[connection]",
            "type=802-11-wireless",
            "llmnr=2",
            "mdns=2",
            "[802-11-wireless]",
            "mode=infrastructure",
            "[802-11-wireless-security]",
            "key-mgmt=wpa-psk",
            "[ipv4]",
            "[ipv6]",
        ] {
            assert!(k.contains(section), "missing {section} in:\n{k}");
        }
        assert!(k.contains("id=securacv-hub"));
        assert!(k.contains("psk=supersecret"));
        // No UUID line when none is supplied.
        assert!(!k.contains("uuid="));
        // Not hidden by default.
        assert!(!k.contains("hidden="));
    }

    #[test]
    fn the_connection_section_advertises_mdns_and_llmnr() {
        // HAOS's own default profile sets llmnr=2 / mdns=2 on [connection];
        // ours must too, or a Wi-Fi-only hub can be up yet never answer
        // homeassistant.local — the headless dead end.
        let k = wifi_keyfile(&seed("home", "supersecret")).unwrap();
        let connection = k.split("\n\n").next().unwrap();
        assert!(connection.starts_with("[connection]"));
        assert!(connection.contains("\nllmnr=2"));
        assert!(connection.contains("\nmdns=2"));
    }

    #[test]
    fn the_keyfile_is_lf_only() {
        // HA's docs: "Please make sure to save this file with UNIX line
        // endings (LF…)" — a CRLF keyfile is silently ignored.
        let k = wifi_keyfile(&seed("home", "supersecret")).unwrap();
        assert!(!k.contains('\r'));
    }

    #[test]
    fn the_ssid_is_a_decimal_byte_array() {
        // "home" = 104 111 109 101
        let k = wifi_keyfile(&seed("home", "supersecret")).unwrap();
        assert!(
            k.contains("ssid=104;111;109;101;\n"),
            "ssid line wrong in:\n{k}"
        );
    }

    #[test]
    fn an_ssid_with_spaces_and_unicode_survives_exactly() {
        // "My Wi‑Fi" with a space — byte array carries every byte, no escaping.
        let k = wifi_keyfile(&seed("My Net", "supersecret")).unwrap();
        // 'M'=77 'y'=121 ' '=32 'N'=78 'e'=101 't'=116
        assert!(k.contains("ssid=77;121;32;78;101;116;\n"));
        // An emoji SSID is multi-byte UTF-8 and still renders as bytes.
        let k2 = wifi_keyfile(&seed("café", "supersecret")).unwrap();
        // 'c'=99 'a'=97 'f'=102 'é'=0xC3 0xA9 = 195 169
        assert!(k2.contains("ssid=99;97;102;195;169;\n"));
    }

    #[test]
    fn a_uuid_and_hidden_flag_are_emitted_when_set() {
        let s = WifiSeed {
            uuid: Some("123e4567-e89b-12d3-a456-426614174000"),
            hidden: true,
            ..seed("home", "supersecret")
        };
        let k = wifi_keyfile(&s).unwrap();
        assert!(k.contains("uuid=123e4567-e89b-12d3-a456-426614174000\n"));
        assert!(k.contains("hidden=true\n"));
    }

    #[test]
    fn a_backslash_in_the_password_is_escaped() {
        let k = wifi_keyfile(&seed("home", "pa\\ssword")).unwrap();
        assert!(
            k.contains("psk=pa\\\\ssword\n"),
            "backslash not doubled in:\n{k}"
        );
    }

    #[test]
    fn a_leading_space_in_the_password_is_escaped() {
        let k = wifi_keyfile(&seed("home", " leadingspace")).unwrap();
        assert!(k.contains("psk=\\sleadingspace\n"));
    }

    #[test]
    fn a_64_hex_raw_pmk_is_accepted() {
        let pmk = "a".repeat(64);
        assert!(wifi_keyfile(&seed("home", &pmk)).is_ok());
    }

    #[test]
    fn an_empty_ssid_is_rejected() {
        assert_eq!(
            wifi_keyfile(&seed("", "supersecret")).unwrap_err(),
            WifiSeedError::EmptySsid
        );
    }

    #[test]
    fn an_oversized_ssid_is_rejected() {
        let long = "x".repeat(33);
        assert_eq!(
            wifi_keyfile(&seed(&long, "supersecret")).unwrap_err(),
            WifiSeedError::SsidTooLong { bytes: 33 }
        );
    }

    #[test]
    fn a_short_password_is_rejected() {
        assert_eq!(
            wifi_keyfile(&seed("home", "short")).unwrap_err(),
            WifiSeedError::PassphraseTooShort { len: 5 }
        );
    }

    #[test]
    fn a_too_long_non_hex_password_is_rejected() {
        let long = "x".repeat(64); // 64 chars but not hex → too long, not a PMK
        assert_eq!(
            wifi_keyfile(&seed("home", &long)).unwrap_err(),
            WifiSeedError::PassphraseTooLong { len: 64 }
        );
    }

    #[test]
    fn a_non_ascii_password_is_rejected() {
        assert_eq!(
            wifi_keyfile(&seed("home", "pässwörd123")).unwrap_err(),
            WifiSeedError::PassphraseNotAscii
        );
    }

    #[test]
    fn an_empty_connection_id_is_rejected() {
        let s = WifiSeed {
            connection_id: "",
            ..seed("home", "supersecret")
        };
        assert_eq!(
            wifi_keyfile(&s).unwrap_err(),
            WifiSeedError::EmptyConnectionId
        );
    }

    #[test]
    fn errors_render_human_text() {
        for e in [
            WifiSeedError::EmptySsid,
            WifiSeedError::SsidTooLong { bytes: 40 },
            WifiSeedError::PassphraseTooShort { len: 3 },
            WifiSeedError::PassphraseNotAscii,
        ] {
            assert!(!e.message().is_empty());
        }
    }
}
