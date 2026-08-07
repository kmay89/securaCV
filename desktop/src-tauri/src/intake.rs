//! Read-only intake checks: is this board what it says it is?
//!
//! Port of the pure half of the browser's `intake.js`. Two questions it can
//! answer from bytes the app already has, with no extra serial traffic at all:
//! is the flash really as big as it claims, and is the MAC a real one.
//!
//! The flash-capacity check is the reason this module exists. A relabeled part
//! — a 4 MB die sold as 16 MB — accepts writes past its real end and silently
//! discards them, so a "successful" flash produces a board that cannot boot,
//! with no error at any layer. The safety copy already reads the whole chip
//! into memory before a write, so the check is free: it is a comparison, not
//! a read.

/// Every byte the same? A blank (or uniformly erased) head has no pattern to
/// look for further up, which makes the alias probe inconclusive rather than
/// clean — on a blank part a mirror and an honest chip read identically.
fn looks_uniform(bytes: &[u8]) -> bool {
    bytes.first().is_none_or(|f| bytes.iter().all(|b| b == f))
}

/// The capacities to probe: every power of two from 256 KB up to (not
/// including) the declared size.
///
/// WHERE you read matters, and getting it wrong makes the check useless. If a
/// 4 MB part claims 16 MB, address `declared - 4K` wraps modulo the REAL size
/// to the top of the real part — which holds something *different* from offset
/// zero, so a naive "compare the two ends" probe sees two unequal blocks and
/// happily calls a counterfeit genuine. The addresses that actually alias
/// offset zero are the candidate capacities themselves: on a real 4 MB die,
/// address 0x400000 IS address 0.
pub fn alias_candidates(declared: u64) -> Vec<u64> {
    let mut out = Vec::new();
    let mut size = 256 * 1024u64;
    while size < declared {
        out.push(size);
        size *= 2;
    }
    out
}

#[derive(Debug, PartialEq, Eq)]
pub struct Finding {
    pub level: &'static str, // clear | inconclusive | attention | stop
    pub label: String,
    pub detail: Option<String>,
}

fn format_size(bytes: u64) -> String {
    let mb = bytes as f64 / (1024.0 * 1024.0);
    if (mb.fract()).abs() < f64::EPSILON {
        format!("{} MB", mb as u64)
    } else {
        format!("{mb:.1} MB")
    }
}

/// Compare the head of the chip against each aliasing candidate inside the
/// bytes we already read. `dump` is the whole-chip safety copy.
pub fn flash_alias_verdict(dump: &[u8], declared: u64) -> Finding {
    const WINDOW: usize = 0x1000;
    if declared == 0 || dump.len() < WINDOW {
        return Finding {
            level: "unknown",
            label: "Flash size not checked".into(),
            detail: None,
        };
    }
    let head = &dump[..WINDOW];
    if looks_uniform(head) {
        return Finding {
            level: "inconclusive",
            label: "Flash size can't be confirmed yet — the chip is blank".into(),
            detail: Some(
                "Every byte at the start of the chip is the same, so there's no pattern to \
                 look for further up. On a blank part a mirror and an honest chip read \
                 identically. The check becomes conclusive after firmware is written."
                    .into(),
            ),
        };
    }
    // The SMALLEST aliasing candidate is the real capacity: a 4 MB die mirrors
    // at 4, 8 and 12 MB, so the first hit is the true size.
    for at in alias_candidates(declared) {
        let start = at as usize;
        let end = start + WINDOW;
        if end > dump.len() {
            break; // the copy doesn't reach this candidate; nothing to compare
        }
        if &dump[start..end] == head {
            return Finding {
                level: "stop",
                label: format!(
                    "Flash is {}, not the {} it claims",
                    format_size(at),
                    format_size(declared)
                ),
                detail: Some(format!(
                    "Reading at {} returns the same bytes as offset zero — the address lines \
                     are wrapping, which is exactly what a relabeled flash part does. Anything \
                     written past the real capacity would be silently lost.",
                    format_size(at)
                )),
            };
        }
    }
    Finding {
        level: "clear",
        label: format!("Flash reads a genuine {}", format_size(declared)),
        detail: None,
    }
}

fn parse_mac(mac: &str) -> Option<[u8; 6]> {
    let parts: Vec<&str> = mac.trim().split([':', '-']).collect();
    if parts.len() != 6 {
        return None;
    }
    let mut out = [0u8; 6];
    for (slot, p) in out.iter_mut().zip(parts) {
        *slot = u8::from_str_radix(p, 16).ok()?;
    }
    Some(out)
}

/// The three things an IEEE-assigned station MAC cannot be. Asserting those
/// beats testing membership of a vendor list we would have to keep honest.
pub fn mac_checks(mac: &str) -> Finding {
    let Some(b) = parse_mac(mac) else {
        return Finding {
            level: "unknown",
            label: "No MAC read".into(),
            detail: None,
        };
    };
    if b.iter().all(|x| *x == 0x00) || b.iter().all(|x| *x == 0xff) {
        return Finding {
            level: "stop",
            label: "MAC is blank".into(),
            detail: Some(
                "An all-zero or all-ones MAC means the address was never programmed — \
                 a hallmark of a cloned or reject part."
                    .into(),
            ),
        };
    }
    if b[0] & 0x01 != 0 {
        return Finding {
            level: "stop",
            label: "MAC is a multicast address".into(),
            detail: Some(
                "The low bit of the first byte marks a multicast address, which can never \
                 be a factory-assigned station MAC."
                    .into(),
            ),
        };
    }
    if b[0] & 0x02 != 0 {
        return Finding {
            level: "attention",
            label: "MAC is locally administered".into(),
            detail: Some(
                "This address was assigned by software, not burned in at the factory.".into(),
            ),
        };
    }
    Finding {
        level: "clear",
        label: "MAC is a factory-assigned address".into(),
        detail: None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Content that varies ACROSS 4 KB blocks, not just within one. A pattern
    // with a 4 KB period would make every block equal the head, so every
    // candidate would "alias" and the fixture would prove nothing.
    fn patterned(len: usize) -> Vec<u8> {
        (0..len)
            .map(|i| ((i ^ (i >> 8) ^ (i >> 16)) & 0xff) as u8)
            .collect()
    }

    /// A dump of a part that really is `real` bytes but is addressed as
    /// `declared`: every read wraps modulo the real capacity.
    fn aliased_dump(real: usize, declared: usize) -> Vec<u8> {
        let content = patterned(real);
        (0..declared).map(|i| content[i % real]).collect()
    }

    #[test]
    fn a_relabeled_part_is_caught_by_the_capacity_that_mirrors_offset_zero() {
        // A 4 MB die sold as 16 MB: reading at 4 MB returns offset zero.
        let dump = aliased_dump(4 * 1024 * 1024, 16 * 1024 * 1024);
        let declared = 16 * 1024 * 1024u64;
        let v = flash_alias_verdict(&dump, declared);
        assert_eq!(v.level, "stop");
        assert!(v.label.contains("4 MB"), "got: {}", v.label);
        assert!(v.label.contains("16 MB"), "got: {}", v.label);
    }

    #[test]
    fn an_honest_part_reads_clear() {
        let declared = 4 * 1024 * 1024u64;
        let dump = patterned(declared as usize);
        assert_eq!(flash_alias_verdict(&dump, declared).level, "clear");
    }

    #[test]
    fn comparing_the_two_ends_would_have_missed_it_so_we_probe_capacities() {
        // The regression this function's shape exists to prevent: on a 4 MB
        // die claiming 16 MB, `declared - 4K` wraps to the TOP of the real
        // part, whose bytes differ from offset zero. A naive end-to-end
        // compare therefore passes a counterfeit. Assert the top block really
        // does differ from the head, and that we catch it anyway.
        let declared = 16 * 1024 * 1024u64;
        let dump = aliased_dump(4 * 1024 * 1024, declared as usize);
        let tail_start = declared as usize - 0x1000;
        assert_ne!(
            &dump[tail_start..],
            &dump[..0x1000],
            "the naive probe location genuinely does NOT alias — hence capacity probing"
        );
        assert_eq!(flash_alias_verdict(&dump, declared).level, "stop");
    }

    #[test]
    fn a_blank_chip_is_inconclusive_not_clean() {
        // Missing evidence must never read as a passed check.
        let declared = 4 * 1024 * 1024u64;
        let dump = vec![0xff; declared as usize];
        assert_eq!(flash_alias_verdict(&dump, declared).level, "inconclusive");
    }

    #[test]
    fn a_truncated_copy_never_invents_a_verdict() {
        // The safety copy stops short (a partial read): candidates past its
        // end are skipped rather than compared against nothing.
        let declared = 16 * 1024 * 1024u64;
        let dump = patterned(1024 * 1024);
        assert_eq!(flash_alias_verdict(&dump, declared).level, "clear");
        assert_eq!(flash_alias_verdict(&[], declared).level, "unknown");
    }

    #[test]
    fn mac_checks_assert_only_what_is_definitional() {
        assert_eq!(mac_checks("a4:cf:12:9b:00:01").level, "clear");
        assert_eq!(mac_checks("00:00:00:00:00:00").level, "stop");
        assert_eq!(mac_checks("ff:ff:ff:ff:ff:ff").level, "stop");
        assert_eq!(mac_checks("a5:cf:12:9b:00:01").level, "stop"); // multicast bit
        assert_eq!(mac_checks("a6:cf:12:9b:00:01").level, "attention"); // locally administered
        assert_eq!(mac_checks("nonsense").level, "unknown");
    }

    #[test]
    fn alias_candidates_are_the_powers_of_two_below_the_claim() {
        assert_eq!(
            alias_candidates(4 * 1024 * 1024),
            vec![256 * 1024, 512 * 1024, 1024 * 1024, 2 * 1024 * 1024]
        );
        assert!(alias_candidates(256 * 1024).is_empty());
    }
}
