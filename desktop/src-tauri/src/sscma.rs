//! Pure SSCMA wire discipline for the Grove Vision AI V2 — the framing and
//! matching rules the module's AT protocol uses, extracted so the live bench's
//! long-running reader (`we2_bench.rs`) shares one host-tested implementation
//! instead of a third hand-rolled copy of the scan loop in `we2.rs`.
//!
//! The rules mirror `we2.rs::read_reply` (and the browser's AT client in
//! `canary-local/assets/we2-flash.js`) byte-for-byte:
//!   * a frame starts after a `\r`, and its first byte must be `{` — anything
//!     else re-arms the scanner at the next `\r` (the wire carries boot noise)
//!   * a frame completes at a `\n` whose previous byte is `}`; the JSON is the
//!     frame minus that trailing `\n`
//!   * an oversized frame (> 2 MiB) is dropped, never grown forever
//!
//! Kept dependency-free (std only) so it unit-tests WITHOUT the desktop stack
//! (`rustc --test src/sscma.rs`), exactly like `rescue`/`health`/`port_hint`.

/// Byte-at-a-time scanner: feed wire bytes, get complete SSCMA JSON frames.
pub struct FrameScanner {
    frame: Vec<u8>,
    collecting: bool,
}

impl Default for FrameScanner {
    fn default() -> Self {
        Self::new()
    }
}

impl FrameScanner {
    pub fn new() -> Self {
        Self {
            frame: Vec::new(),
            collecting: false,
        }
    }

    /// Push one byte; returns a complete JSON frame (without the trailing
    /// newline) when this byte finishes one.
    pub fn push(&mut self, byte: u8) -> Option<Vec<u8>> {
        if !self.collecting {
            if byte == b'\r' {
                self.frame.clear();
                self.collecting = true;
            }
            return None;
        }
        if self.frame.is_empty() && byte != b'{' {
            // Not JSON after the \r — boot noise. Re-arm only on another \r.
            self.collecting = byte == b'\r';
            return None;
        }
        self.frame.push(byte);
        if byte == b'\n' && self.frame.get(self.frame.len().saturating_sub(2)) == Some(&b'}') {
            let json = self.frame[..self.frame.len() - 1].to_vec();
            self.frame.clear();
            self.collecting = false;
            return Some(json);
        }
        if self.frame.len() > 2 * 1024 * 1024 {
            self.frame.clear();
            self.collecting = false;
        }
        None
    }
}

/// The full command line for an AT body: `AT+<body>\r`.
pub fn at_line(body: &str) -> String {
    format!("AT+{body}\r")
}

/// Does a reply's `name` answer this command `body`? Same triple rule as
/// `we2.rs::at_command`: the name equals the body's word before `=`, or the
/// whole body, or is a prefix of the body (a `TSCORE?` query is answered by
/// name `TSCORE`).
pub fn reply_matches(name: &str, body: &str) -> bool {
    let expected = body.split('=').next().unwrap_or(body);
    name == expected || name == body || body.starts_with(name)
}

/// Is this a body the bench may send? The bench speaks a tiny, fixed dialect
/// (INVOKE / BREAK / TSCORE / TIOU / VER? / INFO?), so anything outside plain
/// AT characters — especially CR/LF, which would frame-inject — is refused.
pub fn valid_cmd_body(body: &str) -> bool {
    !body.is_empty()
        && body.len() <= 64
        && body
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '=' | ',' | '?' | '.' | '_' | '-'))
}

/// Clamp a slider value to the module's 0-100 threshold range.
pub fn clamp_threshold(value: i64) -> u8 {
    value.clamp(0, 100) as u8
}

#[cfg(test)]
mod tests {
    use super::*;

    fn feed(scanner: &mut FrameScanner, bytes: &[u8]) -> Vec<Vec<u8>> {
        let mut frames = Vec::new();
        for &b in bytes {
            if let Some(f) = scanner.push(b) {
                frames.push(f);
            }
        }
        frames
    }

    #[test]
    fn scanner_frames_json_between_cr_and_brace_newline() {
        let mut s = FrameScanner::new();
        let frames = feed(&mut s, b"boot noise\r{\"type\":0,\"name\":\"VER\"}\n");
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0], b"{\"type\":0,\"name\":\"VER\"}");
    }

    #[test]
    fn scanner_skips_non_json_lines_and_recovers() {
        let mut s = FrameScanner::new();
        // A non-{ line after \r is dropped; the next \r re-arms.
        let frames = feed(&mut s, b"\rERROR: nope\r{\"a\":1}\n\r{\"b\":2}\n");
        assert_eq!(frames.len(), 2);
        assert_eq!(frames[0], b"{\"a\":1}");
        assert_eq!(frames[1], b"{\"b\":2}");
    }

    #[test]
    fn scanner_needs_brace_before_newline() {
        let mut s = FrameScanner::new();
        // A newline NOT preceded by } stays inside the frame (JSON strings may
        // hold \n only escaped, but the scanner is byte-level and tolerant).
        let frames = feed(&mut s, b"\r{\"a\":\n1}\n");
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0], b"{\"a\":\n1}");
    }

    #[test]
    fn reply_matching_mirrors_at_command() {
        assert!(reply_matches("TSCORE", "TSCORE=50")); // set → name is the word
        assert!(reply_matches("TSCORE", "TSCORE?")); // query → name is a prefix
        assert!(reply_matches("INVOKE", "INVOKE=-1,0,0"));
        assert!(reply_matches("BREAK", "BREAK"));
        assert!(!reply_matches("TIOU", "TSCORE=50"));
        assert!(!reply_matches("VER", "TSCORE?"));
    }

    #[test]
    fn cmd_body_dialect_is_tight() {
        assert!(valid_cmd_body("INVOKE=-1,0,0"));
        assert!(valid_cmd_body("TSCORE=50"));
        assert!(valid_cmd_body("TIOU?"));
        assert!(valid_cmd_body("BREAK"));
        assert!(!valid_cmd_body("")); // empty
        assert!(!valid_cmd_body("VER?\rAT+RESET")); // frame injection
        assert!(!valid_cmd_body(&"X".repeat(65))); // oversized
        assert!(!valid_cmd_body("INFO=\"quote\"")); // quotes are outside the bench dialect
    }

    #[test]
    fn thresholds_clamp_to_module_range() {
        assert_eq!(clamp_threshold(-5), 0);
        assert_eq!(clamp_threshold(50), 50);
        assert_eq!(clamp_threshold(1000), 100);
    }
}
