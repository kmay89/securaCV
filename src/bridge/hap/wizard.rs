//! The parts of the setup wizard that can be tested without a terminal.
//!
//! The wizard's job is to ask as few questions as possible and to make the
//! ones it does ask answerable by someone who does not know what SRP is. That
//! means guessing well: a device publishing as `porch-canary` should be
//! offered the name "Porch Canary", not an empty box. Every guess here is a
//! *default the user can overtype*, never a decision made on their behalf.
//!
//! The prompt parsers are deliberately forgiving in one direction only. "y",
//! "yes", "" (accept the default) and a stray capital all mean yes; anything
//! unrecognized means **no**, because the questions that matter — should
//! Apple Home be told what *kind* of thing moved — are ones where a
//! misunderstood keystroke must not widen what leaves the house.

/// Turn an MQTT device id into a name a human would have typed.
///
/// `porch-canary` → `Porch Canary`, `garage_cam_2` → `Garage Cam 2`. Word
/// separators are `-`, `_` and whitespace; everything else is left alone, so
/// a device id that is already a name survives unharmed.
pub fn suggest_name(device_id: &str) -> String {
    let words: Vec<String> = device_id
        .split(|c: char| c == '-' || c == '_' || c.is_whitespace())
        .filter(|w| !w.is_empty())
        .map(title_case_word)
        .collect();
    if words.is_empty() {
        device_id.to_string()
    } else {
        words.join(" ")
    }
}

fn title_case_word(word: &str) -> String {
    // An all-caps word is probably an initialism (WAP, RTSP) and should stay
    // as it is; lowercasing it would read as a typo.
    if word.len() > 1
        && word
            .chars()
            .all(|c| c.is_ascii_uppercase() || c.is_ascii_digit())
    {
        return word.to_string();
    }
    let mut chars = word.chars();
    match chars.next() {
        Some(first) => first.to_uppercase().collect::<String>() + &chars.as_str().to_lowercase(),
        None => String::new(),
    }
}

/// Read a yes/no answer, where empty means `default`.
///
/// Anything not recognized as yes is **no**. A wizard that treated "sure" or
/// a fat-fingered "u" as consent would be widening what a home publishes on a
/// typo, which is exactly the failure this vocabulary exists to prevent.
pub fn parse_yes_no(input: &str, default: bool) -> bool {
    match input.trim().to_ascii_lowercase().as_str() {
        "" => default,
        "y" | "yes" => true,
        _ => false,
    }
}

/// Parse a selection over a list of `len` items.
///
/// Accepts `all`, empty (meaning all — the wizard offers what it found, and
/// the common case is "yes, those"), or a comma/space separated list of
/// 1-based indices. Out-of-range and unparseable entries are dropped rather
/// than aborting the wizard, and the result is de-duplicated and ordered so
/// the caller gets a clean, stable list.
pub fn parse_selection(input: &str, len: usize) -> Vec<usize> {
    let t = input.trim().to_ascii_lowercase();
    if t.is_empty() || t == "all" || t == "a" {
        return (0..len).collect();
    }
    if t == "none" || t == "-" {
        return Vec::new();
    }
    let mut picked: Vec<usize> = t
        .split(|c: char| c == ',' || c.is_whitespace())
        .filter(|s| !s.is_empty())
        .filter_map(|s| s.parse::<usize>().ok())
        .filter(|n| *n >= 1 && *n <= len)
        .map(|n| n - 1)
        .collect();
    picked.sort_unstable();
    picked.dedup();
    picked
}

/// The pacing presets the wizard offers, as `(label, tick_ms)`.
///
/// Phrased in what the user gets rather than what the number is. Every option
/// is honest about the trade in the same sentence, because the whole point of
/// exposing this dial is that it is a privacy choice and not a speed setting.
pub const PACING_CHOICES: [(&str, u32); 3] = [
    ("Instant — reacts within a second", 1_000),
    ("Relaxed — blurs event timing to ~5 seconds", 5_000),
    ("Private — blurs event timing to ~30 seconds", 30_000),
];

/// Resolve a pacing answer: a menu number, a raw millisecond value, or empty
/// for the default.
pub fn parse_pacing(input: &str, default_ms: u32) -> Option<u32> {
    let t = input.trim();
    if t.is_empty() {
        return Some(default_ms);
    }
    if let Ok(n) = t.parse::<u32>() {
        // A small number is a menu pick; anything larger is a literal
        // millisecond value. The menu has three entries and the minimum
        // tick is 200 ms, so the two ranges cannot overlap.
        if (1..=PACING_CHOICES.len() as u32).contains(&n) {
            return Some(PACING_CHOICES[(n - 1) as usize].1);
        }
        return Some(n);
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn device_ids_become_names_a_human_would_type() {
        assert_eq!(suggest_name("porch-canary"), "Porch Canary");
        assert_eq!(suggest_name("garage_cam_2"), "Garage Cam 2");
        assert_eq!(suggest_name("canary-1"), "Canary 1");
        assert_eq!(suggest_name("front door"), "Front Door");
        assert_eq!(suggest_name("kitchen"), "Kitchen");
    }

    #[test]
    fn initialisms_are_not_mangled() {
        assert_eq!(suggest_name("wap-canary"), "Wap Canary");
        assert_eq!(suggest_name("WAP-canary"), "WAP Canary");
        assert_eq!(suggest_name("RTSP_2"), "RTSP 2");
    }

    #[test]
    fn an_odd_device_id_survives() {
        assert_eq!(suggest_name(""), "");
        assert_eq!(suggest_name("---"), "---");
        assert_eq!(suggest_name("Already A Name"), "Already A Name");
    }

    /// The consent question must not be answerable by accident.
    #[test]
    fn only_an_explicit_yes_is_yes() {
        assert!(parse_yes_no("y", false));
        assert!(parse_yes_no("Y", false));
        assert!(parse_yes_no("yes", false));
        assert!(parse_yes_no("  YES  ", false));
        for maybe in ["sure", "u", "ok", "yep", "1", "true", "n", "no", "xyz"] {
            assert!(!parse_yes_no(maybe, false), "{maybe:?} must not mean yes");
        }
    }

    #[test]
    fn empty_takes_the_default_either_way() {
        assert!(parse_yes_no("", true));
        assert!(!parse_yes_no("", false));
        assert!(!parse_yes_no("   ", false));
    }

    #[test]
    fn selection_defaults_to_everything_found() {
        assert_eq!(parse_selection("", 3), vec![0, 1, 2]);
        assert_eq!(parse_selection("all", 3), vec![0, 1, 2]);
        assert_eq!(parse_selection(" A ", 2), vec![0, 1]);
    }

    #[test]
    fn selection_accepts_indices_in_any_shape() {
        assert_eq!(parse_selection("1,3", 3), vec![0, 2]);
        assert_eq!(parse_selection("3 1", 3), vec![0, 2]);
        assert_eq!(parse_selection("2", 3), vec![1]);
    }

    #[test]
    fn selection_drops_nonsense_rather_than_aborting() {
        assert_eq!(parse_selection("1, banana, 9, 2", 3), vec![0, 1]);
        assert_eq!(parse_selection("0", 3), Vec::<usize>::new());
        assert_eq!(parse_selection("banana", 3), Vec::<usize>::new());
    }

    #[test]
    fn selection_dedupes_and_orders() {
        assert_eq!(parse_selection("3,1,3,1", 3), vec![0, 2]);
    }

    #[test]
    fn none_is_expressible() {
        assert_eq!(parse_selection("none", 3), Vec::<usize>::new());
    }

    #[test]
    fn pacing_accepts_a_menu_pick_or_a_raw_value() {
        assert_eq!(parse_pacing("", 1_000), Some(1_000));
        assert_eq!(parse_pacing("1", 1_000), Some(1_000));
        assert_eq!(parse_pacing("2", 1_000), Some(5_000));
        assert_eq!(parse_pacing("3", 1_000), Some(30_000));
        // Past the menu length it is a literal millisecond value.
        assert_eq!(parse_pacing("2500", 1_000), Some(2_500));
        assert_eq!(parse_pacing("nonsense", 1_000), None);
    }

    /// The menu and the pacer's own bounds must not disagree, or the wizard
    /// would offer a choice the bridge then refuses.
    #[test]
    fn every_offered_pacing_is_one_the_pacer_accepts() {
        use crate::bridge::homekit::{PacingConfig, Projection};
        for (label, ms) in PACING_CHOICES {
            let cfg = PacingConfig {
                tick_ms: ms,
                ..PacingConfig::default()
            };
            // The pacer refuses rather than clamps, so constructing one is
            // the real check that the menu offers nothing it would reject.
            assert!(
                Projection::new(cfg).is_ok(),
                "{label} offers {ms} ms, which the pacer refuses"
            );
        }
    }

    /// The first preset must be the documented default, or the wizard and the
    /// docs would disagree about what "instant" means.
    #[test]
    fn the_first_preset_is_the_default_tick() {
        use crate::bridge::homekit::PacingConfig;
        assert_eq!(PACING_CHOICES[0].1, PacingConfig::default().tick_ms);
    }

    /// Each preset explains the trade in its own label — this is a privacy
    /// dial, and a bare number would invite reading it as a speed setting.
    #[test]
    fn the_pacing_labels_say_what_the_trade_is() {
        for (label, _) in PACING_CHOICES {
            let l = label.to_lowercase();
            assert!(
                l.contains("second") || l.contains("blur"),
                "{label:?} must describe the effect, not just the speed"
            );
        }
    }
}
