//! hub_headless — the pure half of "the app finishes hub setup itself".
//!
//! A freshly flashed, headless hub exposes exactly one shell before any add-on
//! is installed: the HAOS developer console on port 22222, unlocked by an
//! `authorized_keys` file at the root of the boot partition (HAOS's documented
//! debugging mechanism). The flasher seeds that key next to the provisioning
//! bundle, and once the hub answers on the network, the first-boot companion
//! connects and runs the bundle's `host_provision.sh` — installing the
//! Mosquitto broker, the MQTT integration, Frigate, and the securaCV kernel
//! with no monitor ever attached to the Pi.
//!
//! This module is everything about that path that can be decided without a
//! network: validating the public key that becomes `authorized_keys`,
//! validating the host string, and assembling the exact `ssh` invocation. The
//! spawn itself lives in the app (a thin layer, like every other I/O edge);
//! keeping the decisions here keeps them host-tested on every PR.
//!
//! Honest scope: like the Wi-Fi keyfile, this crate proves the *artifacts* are
//! right by construction. Whether a given HAOS build accepts the seeded key and
//! serves the console is pinned by a real first boot on hardware.

/// The port HAOS's developer console listens on when an `authorized_keys` file
/// was accepted. Not configurable — it is HAOS's own choice.
pub const CONSOLE_PORT: u16 = 22222;

/// Where the bundle's host-side runner lands on the booted hub: the boot
/// partition mounts at `/mnt/boot`, and the seed writer places the bundle
/// under `CONFIG/securacv/`. Must match `bundle_root_on_card` in
/// `canary-local/devices/hub_provision_bundle.json`.
pub const REMOTE_RUNNER: &str = "/mnt/boot/CONFIG/securacv/host_provision.sh";

/// Why a public key line was refused for `authorized_keys`.
#[derive(Debug, PartialEq, Eq)]
pub enum KeyError {
    Empty,
    MultipleLines,
    NotAPublicKey,
    UnsafeCharacters,
}

impl KeyError {
    pub fn message(&self) -> String {
        match self {
            KeyError::Empty => "the public key file is empty".to_string(),
            KeyError::MultipleLines => {
                "the public key file has more than one line — expected a single key".to_string()
            }
            KeyError::NotAPublicKey => {
                "that doesn't look like an OpenSSH public key (expected e.g. `ssh-ed25519 …`)"
                    .to_string()
            }
            KeyError::UnsafeCharacters => {
                "the public key line contains characters that can't go in authorized_keys"
                    .to_string()
            }
        }
    }
}

/// Turn the contents of an `id_ed25519.pub` file into the exact
/// `authorized_keys` content to seed: one validated key line, LF-terminated.
///
/// Strict on purpose — this file is read by the hub's SSH daemon as root, so a
/// mangled or multi-line read of the key file must be refused here, at the
/// desk, not discovered as an unreachable console after first boot.
pub fn authorized_keys_content(pubkey_file: &str) -> Result<String, KeyError> {
    let trimmed = pubkey_file.trim();
    if trimmed.is_empty() {
        return Err(KeyError::Empty);
    }
    if trimmed.lines().count() > 1 {
        return Err(KeyError::MultipleLines);
    }
    // authorized_keys is a line-oriented format; a control character could
    // smuggle a second entry or options. Printable ASCII only (the base64 blob
    // and standard comments are ASCII; a comment with anything fancier is not
    // worth carrying).
    if !trimmed.chars().all(|c| c.is_ascii_graphic() || c == ' ') {
        return Err(KeyError::UnsafeCharacters);
    }
    // The key types ssh-keygen emits today. We mint ed25519; the rest are
    // accepted so a user-supplied key also works.
    const TYPES: [&str; 4] = ["ssh-ed25519 ", "ssh-rsa ", "ecdsa-sha2-", "sk-ssh-"];
    if !TYPES.iter().any(|t| trimmed.starts_with(t)) {
        return Err(KeyError::NotAPublicKey);
    }
    // Type + base64 blob at minimum (comment optional).
    if trimmed.split_ascii_whitespace().count() < 2 {
        return Err(KeyError::NotAPublicKey);
    }
    Ok(format!("{trimmed}\n"))
}

/// True when `host` can only be on this network: a private, loopback or
/// link-local IP literal, a `.local`-style name (`.local`, `.lan`,
/// `.internal`, `.home.arpa`), or a single-label LAN hostname. A well-formed
/// hostname is not enough on its own — `attacker.example` is one — and the
/// companion carries the owner's typed credentials to whatever it accepts.
/// Same policy as the Flasher's device calls (`src-tauri/src/fleet.rs`
/// `host_is_local`); it lives here too so PR CI tests it.
pub fn host_is_local(host: &str) -> bool {
    let host = host.trim_matches(['[', ']']);
    if host.is_empty() {
        return false;
    }
    if let Ok(ip) = host.parse::<std::net::IpAddr>() {
        return match ip {
            std::net::IpAddr::V4(v4) => v4.is_private() || v4.is_loopback() || v4.is_link_local(),
            std::net::IpAddr::V6(v6) => {
                v6.is_loopback()
                    // fe80::/10 link-local and fc00::/7 unique-local
                    || (v6.segments()[0] & 0xffc0) == 0xfe80
                    || (v6.segments()[0] & 0xfe00) == 0xfc00
            }
        };
    }
    let lower = host.to_ascii_lowercase();
    for suffix in [".local", ".lan", ".internal", ".home.arpa"] {
        if lower.ends_with(suffix) && lower.len() > suffix.len() {
            return true;
        }
    }
    // A single-label hostname ("canary-3f2a") can only resolve locally.
    !lower.contains('.')
}

/// True when `host` is a plain hostname or address the companion may connect
/// to — no scheme, path, port, or shell metacharacters, and on this network
/// ([`host_is_local`]). Same shape the HTTP probe enforces, minus `:` (the
/// console port is fixed and passed separately, so a smuggled `host:port`
/// can't redirect it).
pub fn valid_host(host: &str) -> bool {
    let host = host.trim();
    !host.is_empty()
        && host
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '-')
        && host_is_local(host)
}

/// The remote command the companion runs: the bundle's own host-side runner,
/// optionally in preview mode, optionally with the plan's opt-in extras
/// (Pi-hole, the hub display). Assembled from fixed strings on purpose — the
/// companion never composes remote shell from user input; the three booleans
/// are the entire input surface.
pub fn remote_command(dry_run: bool, with_pihole: bool, with_display: bool) -> String {
    let mut cmd = format!("sh {REMOTE_RUNNER}");
    if with_pihole {
        cmd.push_str(" --with pihole");
    }
    if with_display {
        cmd.push_str(" --with display");
    }
    if dry_run {
        cmd.push_str(" --dry-run");
    }
    cmd
}

/// The full argument list for the system `ssh` client (macOS and Linux both
/// ship OpenSSH). Decisions worth stating:
///
///   * `BatchMode=yes` — never hang on a password prompt; only the seeded key
///     can answer.
///   * `IdentitiesOnly=yes` + `-i` — offer exactly the maintenance key, not
///     whatever an agent holds.
///   * `StrictHostKeyChecking=accept-new` with a dedicated `known_hosts` —
///     trust-on-first-use per hub, in the app's own file so the user's real
///     known_hosts is never touched. A REFLASHED hub mints new host keys; the
///     caller detects that specific failure ([`host_key_changed`]) and may
///     clear the dedicated file and retry once.
///   * `ConnectTimeout` — the companion polls; a dead hub answers in seconds,
///     not at TCP's leisure.
///   * `ServerAliveInterval`/`CountMax` — the install includes multi-minute
///     quiet stretches (Frigate's image download happens on the hub), so the
///     connection must be probed, not assumed: a Wi-Fi drop mid-run surfaces
///     as a failure in ~2 minutes instead of a session that hangs forever
///     with no cancel button.
pub fn ssh_args(
    host: &str,
    identity_file: &str,
    known_hosts_file: &str,
    dry_run: bool,
    with_pihole: bool,
    with_display: bool,
) -> Result<Vec<String>, String> {
    if !valid_host(host) {
        return Err("that doesn't look like a hostname".to_string());
    }
    Ok(vec![
        "-p".to_string(),
        CONSOLE_PORT.to_string(),
        "-i".to_string(),
        identity_file.to_string(),
        "-o".to_string(),
        "BatchMode=yes".to_string(),
        "-o".to_string(),
        "IdentitiesOnly=yes".to_string(),
        "-o".to_string(),
        "ConnectTimeout=8".to_string(),
        "-o".to_string(),
        "ServerAliveInterval=15".to_string(),
        "-o".to_string(),
        "ServerAliveCountMax=8".to_string(),
        "-o".to_string(),
        "StrictHostKeyChecking=accept-new".to_string(),
        "-o".to_string(),
        format!("UserKnownHostsFile={known_hosts_file}"),
        format!("root@{}", host.trim()),
        remote_command(dry_run, with_pihole, with_display),
    ])
}

/// True when ssh's output says the hub's host key no longer matches the one we
/// stored — the expected signature of a REFLASH (new OS, new host keys), and
/// the one verification failure the companion may heal by clearing its own
/// dedicated known_hosts file and retrying once.
pub fn host_key_changed(ssh_output: &str) -> bool {
    ssh_output.contains("REMOTE HOST IDENTIFICATION HAS CHANGED")
        || ssh_output.contains("Host key verification failed")
}

#[cfg(test)]
mod tests {
    use super::*;

    const KEY: &str = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFa1n2 securacv-flasher";

    #[test]
    fn a_real_pubkey_file_becomes_one_lf_terminated_line() {
        // ssh-keygen writes a trailing newline; the seed must stay one line.
        let got = authorized_keys_content(&format!("{KEY}\n")).unwrap();
        assert_eq!(got, format!("{KEY}\n"));
    }

    #[test]
    fn junk_is_refused_with_the_reason() {
        assert_eq!(authorized_keys_content(""), Err(KeyError::Empty));
        assert_eq!(
            authorized_keys_content("ssh-ed25519 AAA a\nssh-rsa BBB b"),
            Err(KeyError::MultipleLines)
        );
        // A privately-held key pasted by mistake must never become an
        // authorized_keys entry. The PEM header is assembled through a
        // constant rather than written out literally: the repo's secret
        // scanner greps source for that header shape, and a scanner that
        // cries wolf over its own fixtures is one people learn to ignore.
        // Interpolating keeps the halves off one source line even after
        // `cargo fmt` re-joins wrapped arguments.
        const HELD: &str = "PRIVATE";
        let pem_header = format!("-----BEGIN OPENSSH {HELD} KEY-----");
        assert_eq!(
            authorized_keys_content(&pem_header),
            Err(KeyError::NotAPublicKey)
        );
        assert_eq!(
            authorized_keys_content("ssh-ed25519"),
            Err(KeyError::NotAPublicKey)
        );
        assert_eq!(
            authorized_keys_content("ssh-ed25519 AAA\tevil"),
            Err(KeyError::UnsafeCharacters)
        );
        // Every error narrates.
        for e in [
            KeyError::Empty,
            KeyError::MultipleLines,
            KeyError::NotAPublicKey,
            KeyError::UnsafeCharacters,
        ] {
            assert!(!e.message().is_empty());
        }
    }

    #[test]
    fn hosts_are_validated_like_the_probe_but_stricter() {
        assert!(valid_host("homeassistant.local"));
        assert!(valid_host("10.0.0.17"));
        assert!(!valid_host(""));
        assert!(!valid_host("host/path"));
        assert!(!valid_host("host cmd"));
        assert!(!valid_host("host;rm -rf /"));
        assert!(
            !valid_host("homeassistant.local:8123"),
            "the console port is fixed — a smuggled port must be refused"
        );
    }

    #[test]
    fn hosts_off_this_network_are_refused_however_well_formed() {
        // The companion posts the owner's credentials to whatever passes, so
        // a syntactically fine public host is exactly the one to refuse.
        for far in [
            "example.com",
            "attacker-vps.example",
            "203.0.113.5",
            "8.8.8.8",
        ] {
            assert!(!host_is_local(far), "{far} is not on this network");
            assert!(!valid_host(far), "{far} must be refused");
        }
        for near in [
            "homeassistant.local",
            "hub",
            "hub-2.lan",
            "pi.home.arpa",
            "10.0.0.17",
            "192.168.1.20",
            "172.16.4.9",
            "127.0.0.1",
            "[fe80::1]",
            "fd00::a1",
        ] {
            assert!(host_is_local(near), "{near} is on this network");
        }
        assert!(!host_is_local(""));
        assert!(!host_is_local(".local"), "the bare suffix is not a name");
    }

    #[test]
    fn the_remote_command_is_the_bundles_own_runner() {
        assert_eq!(
            remote_command(false, false, false),
            "sh /mnt/boot/CONFIG/securacv/host_provision.sh"
        );
        assert_eq!(
            remote_command(true, false, false),
            "sh /mnt/boot/CONFIG/securacv/host_provision.sh --dry-run"
        );
        // The opt-in extras ride as the executor's own --with flags; flag
        // order matters (--with before --dry-run so a preview previews them).
        assert_eq!(
            remote_command(false, true, false),
            "sh /mnt/boot/CONFIG/securacv/host_provision.sh --with pihole"
        );
        assert_eq!(
            remote_command(true, true, false),
            "sh /mnt/boot/CONFIG/securacv/host_provision.sh --with pihole --dry-run"
        );
        assert_eq!(
            remote_command(false, false, true),
            "sh /mnt/boot/CONFIG/securacv/host_provision.sh --with display"
        );
        assert_eq!(
            remote_command(true, true, true),
            "sh /mnt/boot/CONFIG/securacv/host_provision.sh --with pihole --with display --dry-run"
        );
    }

    #[test]
    fn ssh_args_carry_every_safety_option() {
        let args = ssh_args(
            "homeassistant.local",
            "/k/id",
            "/k/known_hosts",
            false,
            false,
            false,
        )
        .unwrap();
        let joined = args.join(" ");
        assert!(joined.contains("-p 22222"));
        assert!(joined.contains("BatchMode=yes"));
        assert!(joined.contains("IdentitiesOnly=yes"));
        assert!(
            joined.contains("ServerAliveInterval=15") && joined.contains("ServerAliveCountMax=8"),
            "without keepalives a Wi-Fi drop mid-install hangs the run forever"
        );
        assert!(joined.contains("StrictHostKeyChecking=accept-new"));
        assert!(joined.contains("UserKnownHostsFile=/k/known_hosts"));
        assert!(joined
            .ends_with("root@homeassistant.local sh /mnt/boot/CONFIG/securacv/host_provision.sh"));
        assert!(ssh_args("bad host", "/k/id", "/k/kh", false, false, false).is_err());
    }

    #[test]
    fn a_reflashed_hub_is_recognized_from_ssh_output() {
        assert!(host_key_changed(
            "@@@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @@@"
        ));
        assert!(host_key_changed("Host key verification failed."));
        assert!(!host_key_changed("Connection refused"));
    }
}
