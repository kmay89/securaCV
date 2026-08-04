//! The setup code as a QR you can scan off your own terminal.
//!
//! Typing `137-22-258` into the Home app works. Pointing your phone at the
//! terminal that just printed it is better, and it is the difference between
//! a setup that feels like configuration and one that feels like an Apple
//! product. The payload is the same `X-HM://` URI that would be printed on a
//! sticker, so the Home app treats this exactly like a real accessory's code.
//!
//! # Polarity is the whole problem
//!
//! A QR scanner needs **dark modules on a light background**. A terminal has
//! no idea which it is: draw `█` on a dark theme and you have produced a
//! photographic negative that most scanners refuse. So the default renderer
//! does not draw glyphs at all — it paints ANSI *background* colors, black
//! for dark modules and white for light. That is correct on a light theme, a
//! dark theme, and a terminal with a photograph behind it.
//!
//! [`Style::Blocks`] is the fallback for somewhere ANSI is not welcome (a log
//! file, a CI transcript, a pipe). It draws glyphs, so it is only correct on
//! a light background unless inverted — which is why the wizard says so
//! rather than leaving the user to wonder why their phone won't bite.
//!
//! # Aspect ratio
//!
//! Terminal cells are about twice as tall as they are wide, so one module is
//! drawn **two cells wide and one tall**. A QR rendered one cell per module
//! comes out squashed to half width, and squashed QRs do not scan.

use qrcodegen::{QrCode, QrCodeEcc};

/// How to draw the code.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Style {
    /// ANSI background colors. Correct on any terminal theme. The default.
    Ansi,
    /// Block glyphs, for output that cannot carry escape codes. Correct on a
    /// **light** background; use [`Style::BlocksInverted`] on a dark one.
    Blocks,
    /// Block glyphs with the polarity flipped, for a dark terminal.
    BlocksInverted,
}

/// The quiet zone every QR needs: four light modules on each side. Without
/// it a scanner cannot find the code's edges, and the most common "why won't
/// it scan" is a QR printed flush against other text.
const QUIET: usize = 4;

/// Why the code could not be drawn.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct QrTooLong;

impl std::fmt::Display for QrTooLong {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "payload does not fit in a QR code")
    }
}

impl std::error::Error for QrTooLong {}

/// Render `payload` as a scannable QR code, ready to print.
///
/// Error correction is deliberately **medium**, not low: a terminal QR gets
/// photographed at an angle, through a glass screen, often with glare. The
/// extra redundancy costs a couple of modules and buys a code that actually
/// scans in the room the user is standing in.
pub fn render(payload: &str, style: Style) -> Result<String, QrTooLong> {
    let code = QrCode::encode_text(payload, QrCodeEcc::Medium).map_err(|_| QrTooLong)?;
    let size = code.size();
    // `size` is 21..=177 for any valid QR, so this cast is always in range.
    let n = size as usize;

    let dark = |x: usize, y: usize| -> bool {
        // Anything in the quiet zone is light by definition.
        if x < QUIET || y < QUIET || x >= QUIET + n || y >= QUIET + n {
            return false;
        }
        code.get_module((x - QUIET) as i32, (y - QUIET) as i32)
    };

    let total = n + QUIET * 2;
    let mut out = String::new();
    for y in 0..total {
        for x in 0..total {
            out.push_str(cell(dark(x, y), style));
        }
        if style == Style::Ansi {
            out.push_str(RESET);
        }
        out.push('\n');
    }
    Ok(out)
}

const RESET: &str = "\x1b[0m";
/// Two spaces on a black background — one dark module, two cells wide.
const ANSI_DARK: &str = "\x1b[40m  ";
/// Two spaces on a white background — one light module.
const ANSI_LIGHT: &str = "\x1b[47m  ";

fn cell(is_dark: bool, style: Style) -> &'static str {
    match (style, is_dark) {
        (Style::Ansi, true) => ANSI_DARK,
        (Style::Ansi, false) => ANSI_LIGHT,
        // Glyph styles: a "dark module" is drawn as ink on a light page.
        (Style::Blocks, true) => "██",
        (Style::Blocks, false) => "  ",
        (Style::BlocksInverted, true) => "  ",
        (Style::BlocksInverted, false) => "██",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const URI: &str = "X-HM://0023P3O9E13I9";

    fn lines(s: &str) -> Vec<&str> {
        s.lines().collect()
    }

    #[test]
    fn renders_a_square_code_with_a_quiet_zone() {
        let out = render(URI, Style::Blocks).expect("renders");
        let rows = lines(&out);
        // Square: as many rows as modules-wide, since each module is one row
        // tall and two cells wide.
        let width_in_cells = rows[0].chars().count();
        assert_eq!(width_in_cells, rows.len() * 2, "code must be square");

        // The outermost QUIET rows and columns are entirely light.
        for row in rows.iter().take(QUIET) {
            assert!(row.trim().is_empty(), "top quiet zone must be blank");
        }
        for row in rows.iter().rev().take(QUIET) {
            assert!(row.trim().is_empty(), "bottom quiet zone must be blank");
        }
        for row in &rows {
            let chars: Vec<char> = row.chars().collect();
            assert!(
                chars[..QUIET * 2].iter().all(|c| *c == ' '),
                "left quiet zone must be blank"
            );
        }
    }

    /// A QR one cell per module renders at half width and does not scan. This
    /// pins the two-cells-wide rule that fixes it.
    #[test]
    fn each_module_is_two_cells_wide() {
        let out = render(URI, Style::Blocks).expect("renders");
        let rows = lines(&out);
        // Every glyph run is even-length, because modules come in pairs.
        for row in &rows {
            let chars: Vec<char> = row.chars().collect();
            assert_eq!(chars.len() % 2, 0);
            for pair in chars.chunks(2) {
                assert_eq!(pair[0], pair[1], "a module must be two identical cells");
            }
        }
    }

    /// The reason ANSI is the default: it carries its own background, so the
    /// user's terminal theme cannot invert the code.
    #[test]
    fn ansi_paints_its_own_background() {
        let out = render(URI, Style::Ansi).expect("renders");
        assert!(
            out.contains(ANSI_DARK),
            "dark modules need a black background"
        );
        assert!(
            out.contains(ANSI_LIGHT),
            "light modules need a white background"
        );
        for line in out.lines() {
            assert!(line.ends_with(RESET), "every row must reset the color");
        }
    }

    /// Inverted is a true negative of Blocks — same code, opposite ink.
    #[test]
    fn inverted_is_the_exact_negative() {
        let normal = render(URI, Style::Blocks).expect("renders");
        let inverted = render(URI, Style::BlocksInverted).expect("renders");
        assert_eq!(normal.lines().count(), inverted.lines().count());
        for (a, b) in normal.lines().zip(inverted.lines()) {
            let flipped: String = a
                .chars()
                .map(|c| if c == '█' { ' ' } else { '█' })
                .collect();
            assert_eq!(flipped, b);
        }
    }

    /// The finder pattern is the 7×7 square a scanner locks onto. If it is
    /// missing or misplaced the code is decoration, not a QR.
    #[test]
    fn the_top_left_finder_pattern_is_where_a_scanner_looks_for_it() {
        let out = render(URI, Style::Blocks).expect("renders");
        let rows = lines(&out);
        // Row QUIET, starting at column QUIET, is the finder's top edge:
        // seven dark modules in a row.
        let row: Vec<char> = rows[QUIET].chars().collect();
        let start = QUIET * 2;
        for i in 0..7 * 2 {
            assert_eq!(row[start + i], '█', "finder top edge must be solid");
        }
        // And the module immediately after it is light (the separator).
        assert_eq!(row[start + 7 * 2], ' ');
    }

    #[test]
    fn rendering_is_deterministic() {
        assert_eq!(
            render(URI, Style::Ansi).expect("a"),
            render(URI, Style::Ansi).expect("b")
        );
    }

    #[test]
    fn different_setup_codes_render_differently() {
        let a = render("X-HM://0023P3O9E13I9", Style::Blocks).expect("a");
        let b = render("X-HM://0023P3O9E13J0", Style::Blocks).expect("b");
        assert_ne!(a, b);
    }

    /// A HomeKit setup URI is short, so it must fit in a small QR — if this
    /// ever needed a huge version, something is wrong with the payload.
    #[test]
    fn a_setup_uri_fits_in_a_small_code() {
        let out = render(URI, Style::Blocks).expect("renders");
        let rows = out.lines().count();
        assert!(
            rows <= 33 + QUIET * 2,
            "a setup URI should stay a low-version QR, got {rows} rows"
        );
    }

    #[test]
    fn an_absurd_payload_is_refused_not_panicked_on() {
        let huge = "X".repeat(8000);
        assert_eq!(render(&huge, Style::Blocks), Err(QrTooLong));
    }
}
