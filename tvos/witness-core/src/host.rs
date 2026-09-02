//! The one rule for where a Bonjour advert may send the Wall.
//!
//! A Canary announces itself as `_securacv._tcp` with a TXT `host` key — the
//! salted mDNS hostname the firmware's `make_hostname()` writes, a BARE label
//! like `canary-nightstand7-001-a1b2c3`. The Wall turns that into the address
//! it polls for `/api/fleet`. Anyone on the LAN can publish such an advert, so
//! the value must never be trusted as-is: an advert saying
//! `host=evil.example.com` (or a public IP) would steer a household's
//! television at a stranger's server, and the old code did exactly that.
//!
//! This is the same gate the iPhone applies to every base URL it dials —
//! `DeviceAPI.isPrivate` plus `DeviceAPI.url(forDiscoveredHost:)` in
//! `ios/Sources/SecuraCV/Transport/DeviceAPI.swift` — restated here as a pure
//! function so the TV and the phone accept and refuse the same hosts. If one
//! side changes, change the other in the same commit.
//!
//! Accepted, and nothing else:
//!
//! * a bare DNS label (`canary-…`), which is qualified to `<label>.local`;
//! * a `.local` hostname whose every label is a DNS label —
//!   `[A-Za-z0-9-]`, 1–63 characters, no leading or trailing hyphen;
//! * a dotted-decimal IPv4 address of exactly four octets that is private:
//!   10/8, 172.16/12, 192.168/16, 169.254/16 (link-local — a Canary on a
//!   hotspot with no DHCP) or 127/8.
//!
//! A dotted hostname that is not `.local` is refused even when every label is
//! well-formed: an mDNS advert has no business naming a unicast-DNS host, and
//! the phone refuses those too.

/// One DNS label: `[A-Za-z0-9-]`, 1..=63 bytes, neither starting nor ending
/// with a hyphen. ASCII only — an IDN label is not something the firmware
/// ever writes, so it is refused rather than guessed at.
pub fn is_dns_label(label: &str) -> bool {
    let bytes = label.as_bytes();
    (1..=63).contains(&bytes.len())
        && bytes
            .iter()
            .all(|b| b.is_ascii_alphanumeric() || *b == b'-')
        && !label.starts_with('-')
        && !label.ends_with('-')
}

/// Exactly four dotted-decimal octets, each a plain 0–255 with no sign, no
/// whitespace and at most three digits. `Some` for `192.168.1.20`; `None` for
/// `192.168.1`, `192.168.1.20.local`, `1.2.3.256`, or `10.0.0.1.attacker.com`.
///
/// Deliberately not `std::net::Ipv4Addr::from_str`: that accepts nothing
/// extra today, but this is the check that keeps a crafted host from
/// collapsing into a private address, and it should read as exactly the rule
/// the phone applies ("every label must be a decimal octet and there must be
/// exactly four").
pub fn parse_ipv4(host: &str) -> Option<[u8; 4]> {
    let mut octets = [0u8; 4];
    let mut count = 0usize;
    for part in host.split('.') {
        if count == 4 || part.is_empty() || part.len() > 3 {
            return None;
        }
        if !part.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        octets[count] = part.parse::<u8>().ok()?;
        count += 1;
    }
    (count == 4).then_some(octets)
}

/// RFC 1918 private ranges, loopback, and IPv4 link-local — the addresses a
/// Canary on the same LAN can actually have. Same table as the phone's
/// `isPrivate`.
pub fn is_private_ipv4(octets: [u8; 4]) -> bool {
    matches!(
        octets,
        [10, ..] | [172, 16..=31, ..] | [192, 168, ..] | [169, 254, ..] | [127, ..]
    )
}

/// Turn a TXT `host` value into the hostname the Wall may poll, or `None`
/// when the advert must be skipped.
///
/// Returns the host lower-cased and, for a bare label, qualified with
/// `.local` (which is what makes a bare mDNS label resolvable — the phone's
/// `url(forDiscoveredHost:)` does the same). A private IPv4 address comes back
/// as itself, never with `.local` appended.
pub fn normalize_source_host(raw: &str) -> Option<String> {
    let host = raw.trim().to_ascii_lowercase();
    if host.is_empty() {
        return None;
    }
    if let Some(octets) = parse_ipv4(&host) {
        return is_private_ipv4(octets).then_some(host);
    }
    if !host.contains('.') {
        // The firmware's own shape: one salted label, no domain.
        return is_dns_label(&host).then(|| format!("{host}.local"));
    }
    let labels: Vec<&str> = host.split('.').collect();
    // "<something>.local" only — never ".local" alone, never "local".
    if labels.len() < 2 || labels[labels.len() - 1] != "local" {
        return None;
    }
    labels
        .iter()
        .all(|label| is_dns_label(label))
        .then_some(host)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_firmwares_bare_label_is_qualified_to_local() {
        // Verbatim shape of make_hostname(): product, unit, salted pseudonym.
        assert_eq!(
            normalize_source_host("canary-nightstand7-001-a1b2c3").as_deref(),
            Some("canary-nightstand7-001-a1b2c3.local")
        );
    }

    #[test]
    fn an_already_qualified_local_name_is_kept() {
        assert_eq!(
            normalize_source_host("canary-dash-001-d4e5f6.local").as_deref(),
            Some("canary-dash-001-d4e5f6.local")
        );
        // Multi-label .local names are fine as long as every label is.
        assert_eq!(
            normalize_source_host("hub.kitchen.local").as_deref(),
            Some("hub.kitchen.local")
        );
    }

    #[test]
    fn case_and_whitespace_are_folded() {
        assert_eq!(
            normalize_source_host("  Canary-WAP-01.LOCAL ").as_deref(),
            Some("canary-wap-01.local")
        );
    }

    #[test]
    fn a_private_ipv4_is_accepted_as_itself() {
        for ip in [
            "10.0.0.1",
            "10.255.255.255",
            "172.16.0.1",
            "172.31.255.254",
            "192.168.1.20",
            "169.254.7.7",
            "127.0.0.1",
        ] {
            assert_eq!(normalize_source_host(ip).as_deref(), Some(ip), "{ip}");
        }
    }

    #[test]
    fn a_public_ipv4_is_refused() {
        for ip in [
            "8.8.8.8",
            "1.1.1.1",
            "172.15.0.1",  // just below the /12
            "172.32.0.1",  // just above it
            "192.169.0.1", // not 192.168
            "169.253.0.1",
            "11.0.0.1",
            "0.0.0.0",
            "255.255.255.255",
        ] {
            assert_eq!(normalize_source_host(ip), None, "{ip}");
        }
    }

    #[test]
    fn a_crafted_address_cannot_collapse_into_a_private_one() {
        // The phone's first version dropped non-numeric labels, so this read
        // as 10.0.0.1. It must not read as anything.
        assert_eq!(normalize_source_host("10.0.0.1.attacker.com"), None);
        assert_eq!(normalize_source_host("10.0.0.1.attacker.local.com"), None);
        // Five octets, three octets, an out-of-range octet, a signed octet.
        assert_eq!(normalize_source_host("10.0.0.1.2"), None);
        assert_eq!(normalize_source_host("10.0.0"), None);
        assert_eq!(normalize_source_host("10.0.0.256"), None);
        assert_eq!(normalize_source_host("10.0.0.+1"), None);
        assert_eq!(normalize_source_host("10.0.0.0001"), None);
    }

    #[test]
    fn a_public_hostname_is_refused_even_when_well_formed() {
        // The whole reason this module exists: an advert may not point the
        // television at a stranger's server.
        assert_eq!(normalize_source_host("evil.example.com"), None);
        assert_eq!(normalize_source_host("canary.local.example.com"), None);
        assert_eq!(normalize_source_host("securacv.com"), None);
    }

    #[test]
    fn a_local_suffix_alone_is_not_a_host() {
        assert_eq!(normalize_source_host(".local"), None);
        assert_eq!(normalize_source_host("canary..local"), None);
        assert_eq!(normalize_source_host("canary.local."), None);
    }

    #[test]
    fn malformed_labels_are_refused() {
        for bad in [
            "",
            "   ",
            "-canary",
            "canary-",
            "-canary.local",
            "canary-.local",
            "can ary",
            "canary_dash_001", // underscores are not DNS-label characters
            "canary/../etc",
            "user@canary.local",
            "canary.local:8099", // a port is not part of a hostname
            "http://canary.local",
            "canary.lócal",
            "cañary",
        ] {
            assert_eq!(normalize_source_host(bad), None, "{bad:?}");
        }
        let too_long = "a".repeat(64);
        assert_eq!(normalize_source_host(&too_long), None);
        let longest_allowed = "a".repeat(63);
        assert_eq!(
            normalize_source_host(&longest_allowed).as_deref(),
            Some(format!("{longest_allowed}.local").as_str())
        );
    }

    #[test]
    fn the_label_rule_is_the_documented_one() {
        assert!(is_dns_label("a"));
        assert!(is_dns_label("canary-wap-01"));
        assert!(is_dns_label("A1B2C3"));
        assert!(!is_dns_label(""));
        assert!(!is_dns_label("-a"));
        assert!(!is_dns_label("a-"));
        assert!(!is_dns_label("a.b"));
        assert!(!is_dns_label("a_b"));
    }

    #[test]
    fn the_private_table_matches_the_phones() {
        assert!(is_private_ipv4([10, 1, 2, 3]));
        assert!(is_private_ipv4([172, 16, 0, 0]));
        assert!(is_private_ipv4([172, 31, 255, 255]));
        assert!(!is_private_ipv4([172, 15, 255, 255]));
        assert!(!is_private_ipv4([172, 32, 0, 0]));
        assert!(is_private_ipv4([192, 168, 0, 1]));
        assert!(!is_private_ipv4([192, 167, 0, 1]));
        assert!(is_private_ipv4([169, 254, 1, 1]));
        assert!(is_private_ipv4([127, 0, 0, 1]));
        assert!(!is_private_ipv4([8, 8, 8, 8]));
    }
}
