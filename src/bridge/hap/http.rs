//! The small, strict HTTP/1.1 subset HAP speaks.
//!
//! HAP is HTTP-shaped but not HTTP-served: the requests arrive inside an
//! encrypted session (or, before pairing, on a bare socket), there are seven
//! routes, and the accessory also *originates* messages — the `EVENT/1.0`
//! notification that tells a controller a characteristic changed. A general
//! HTTP server would be a large dependency and a large attack surface for a
//! protocol this narrow, so this is a parser for exactly what HAP uses and
//! nothing else.
//!
//! Everything here reads bytes a peer controls, so every limit is explicit
//! and every failure is a `Result` (FR-2, FR-4). A request that exceeds a
//! bound is refused, never truncated into something that parses differently
//! than it was sent.

/// The largest request line plus headers we will buffer.
pub const MAX_HEADER_BYTES: usize = 8 * 1024;

/// The largest request body we will accept. HAP bodies are a few hundred
/// bytes; the SRP messages are the biggest at well under 1 KiB.
pub const MAX_BODY_BYTES: usize = 16 * 1024;

/// The HAP content types.
pub mod content_type {
    pub const TLV8: &str = "application/pairing+tlv8";
    pub const JSON: &str = "application/hap+json";
}

/// Why a request could not be parsed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum HttpError {
    /// The request line or headers exceeded [`MAX_HEADER_BYTES`].
    HeadersTooLarge,
    /// The body exceeded [`MAX_BODY_BYTES`], or `Content-Length` was absurd.
    BodyTooLarge,
    /// The request line was not `METHOD PATH VERSION`.
    Malformed,
}

impl std::fmt::Display for HttpError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let s = match self {
            HttpError::HeadersTooLarge => "HTTP headers exceeded the buffer cap",
            HttpError::BodyTooLarge => "HTTP body exceeded the buffer cap",
            HttpError::Malformed => "malformed HTTP request line",
        };
        write!(f, "{s}")
    }
}

impl std::error::Error for HttpError {}

/// A parsed HAP request.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Request {
    /// `GET`, `POST` or `PUT`.
    pub method: String,
    /// The request target, including any query string.
    pub target: String,
    /// The body, already read to `Content-Length`.
    pub body: Vec<u8>,
}

impl Request {
    /// Try to parse one request from the front of `buf`.
    ///
    /// Returns `Ok(None)` when the buffer does not hold a complete request
    /// yet, along with nothing consumed — the caller reads more and retries.
    /// On success, returns the request and how many bytes it used.
    pub fn parse(buf: &[u8]) -> Result<Option<(Request, usize)>, HttpError> {
        let head_end = match find_double_crlf(buf) {
            Some(i) => i,
            None => {
                if buf.len() > MAX_HEADER_BYTES {
                    return Err(HttpError::HeadersTooLarge);
                }
                return Ok(None);
            }
        };
        if head_end > MAX_HEADER_BYTES {
            return Err(HttpError::HeadersTooLarge);
        }

        // Headers are ASCII per the HTTP grammar; anything else is malformed
        // rather than something to guess at.
        let head = std::str::from_utf8(&buf[..head_end]).map_err(|_| HttpError::Malformed)?;
        let mut lines = head.split("\r\n");
        let request_line = lines.next().ok_or(HttpError::Malformed)?;
        let mut parts = request_line.split(' ');
        let method = parts.next().ok_or(HttpError::Malformed)?;
        let target = parts.next().ok_or(HttpError::Malformed)?;
        // The version is present in every real request; we do not branch on
        // it, but its absence means this is not a request line.
        parts.next().ok_or(HttpError::Malformed)?;
        if method.is_empty() || target.is_empty() {
            return Err(HttpError::Malformed);
        }

        let mut content_length = 0usize;
        for line in lines {
            if let Some((name, value)) = line.split_once(':') {
                if name.trim().eq_ignore_ascii_case("content-length") {
                    content_length = value
                        .trim()
                        .parse::<usize>()
                        .map_err(|_| HttpError::Malformed)?;
                    if content_length > MAX_BODY_BYTES {
                        return Err(HttpError::BodyTooLarge);
                    }
                }
            }
        }

        let body_start = head_end + 4;
        let total = body_start.saturating_add(content_length);
        if buf.len() < total {
            return Ok(None);
        }

        Ok(Some((
            Request {
                method: method.to_string(),
                target: target.to_string(),
                body: buf[body_start..total].to_vec(),
            },
            total,
        )))
    }

    /// The path with any query string removed.
    pub fn path(&self) -> &str {
        match self.target.split_once('?') {
            Some((p, _)) => p,
            None => &self.target,
        }
    }

    /// The query string, if any.
    pub fn query(&self) -> Option<&str> {
        self.target.split_once('?').map(|(_, q)| q)
    }
}

/// Find the `\r\n\r\n` that ends the headers.
fn find_double_crlf(buf: &[u8]) -> Option<usize> {
    buf.windows(4).position(|w| w == b"\r\n\r\n")
}

/// The reason phrase for the status codes HAP uses.
fn reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        204 => "No Content",
        207 => "Multi-Status",
        400 => "Bad Request",
        404 => "Not Found",
        405 => "Method Not Allowed",
        470 => "Connection Authorization Required",
        500 => "Internal Server Error",
        _ => "Unknown",
    }
}

/// Build a response with a body.
pub fn response(status: u16, content_type: &str, body: &[u8]) -> Vec<u8> {
    let mut out = format!(
        "HTTP/1.1 {status} {}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\n\r\n",
        reason(status),
        body.len()
    )
    .into_bytes();
    out.extend_from_slice(body);
    out
}

/// Build a response with no body (HAP's 204 for a successful write).
pub fn empty_response(status: u16) -> Vec<u8> {
    format!(
        "HTTP/1.1 {status} {}\r\nContent-Length: 0\r\n\r\n",
        reason(status)
    )
    .into_bytes()
}

/// Build an `EVENT/1.0` notification — the accessory speaking first, which
/// is how a controller learns a characteristic changed without polling.
pub fn event(body: &[u8]) -> Vec<u8> {
    let mut out = format!(
        "EVENT/1.0 200 OK\r\nContent-Type: {}\r\nContent-Length: {}\r\n\r\n",
        content_type::JSON,
        body.len()
    )
    .into_bytes();
    out.extend_from_slice(body);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn raw(s: &str) -> Vec<u8> {
        s.replace('\n', "\r\n").into_bytes()
    }

    #[test]
    fn parses_a_get_with_no_body() {
        let buf = raw("GET /accessories HTTP/1.1\nHost: canary\n\n");
        let (req, used) = Request::parse(&buf).expect("ok").expect("complete");
        assert_eq!(req.method, "GET");
        assert_eq!(req.path(), "/accessories");
        assert!(req.body.is_empty());
        assert_eq!(used, buf.len());
    }

    #[test]
    fn parses_a_post_with_a_body() {
        let mut buf = raw("POST /pair-setup HTTP/1.1\nContent-Length: 4\n\n");
        buf.extend_from_slice(&[1, 2, 3, 4]);
        let (req, used) = Request::parse(&buf).expect("ok").expect("complete");
        assert_eq!(req.method, "POST");
        assert_eq!(req.body, vec![1, 2, 3, 4]);
        assert_eq!(used, buf.len());
    }

    /// The stream case: a request split across reads must not parse until
    /// its body has fully arrived.
    #[test]
    fn an_incomplete_body_is_not_parsed_early() {
        let mut buf = raw("POST /pair-setup HTTP/1.1\nContent-Length: 8\n\n");
        buf.extend_from_slice(&[1, 2, 3]);
        assert_eq!(Request::parse(&buf).expect("ok"), None);
        buf.extend_from_slice(&[4, 5, 6, 7, 8]);
        let (req, _) = Request::parse(&buf).expect("ok").expect("now complete");
        assert_eq!(req.body.len(), 8);
    }

    #[test]
    fn incomplete_headers_are_not_parsed_early() {
        let buf = raw("GET /accessories HTTP/1.1\nHost: cana");
        assert_eq!(Request::parse(&buf).expect("ok"), None);
    }

    /// Two pipelined requests: the second must survive the first being
    /// consumed.
    #[test]
    fn only_the_first_request_is_consumed() {
        let mut buf = raw("GET /accessories HTTP/1.1\n\n");
        let first_len = buf.len();
        buf.extend_from_slice(&raw("GET /characteristics?id=1.10 HTTP/1.1\n\n"));

        let (req, used) = Request::parse(&buf).expect("ok").expect("first");
        assert_eq!(req.path(), "/accessories");
        assert_eq!(used, first_len);

        let (req2, _) = Request::parse(&buf[used..]).expect("ok").expect("second");
        assert_eq!(req2.path(), "/characteristics");
        assert_eq!(req2.query(), Some("id=1.10"));
    }

    #[test]
    fn query_strings_split_off_the_path() {
        let buf = raw("GET /characteristics?id=2.11,2.13 HTTP/1.1\n\n");
        let (req, _) = Request::parse(&buf).expect("ok").expect("complete");
        assert_eq!(req.path(), "/characteristics");
        assert_eq!(req.query(), Some("id=2.11,2.13"));
    }

    #[test]
    fn content_length_is_case_insensitive() {
        let mut buf = raw("POST /x HTTP/1.1\ncOnTeNt-LeNgTh: 2\n\n");
        buf.extend_from_slice(&[9, 9]);
        let (req, _) = Request::parse(&buf).expect("ok").expect("complete");
        assert_eq!(req.body, vec![9, 9]);
    }

    #[test]
    fn an_absurd_content_length_is_refused() {
        let buf = raw("POST /x HTTP/1.1\nContent-Length: 999999999\n\n");
        assert_eq!(Request::parse(&buf), Err(HttpError::BodyTooLarge));
    }

    #[test]
    fn a_non_numeric_content_length_is_malformed() {
        let buf = raw("POST /x HTTP/1.1\nContent-Length: banana\n\n");
        assert_eq!(Request::parse(&buf), Err(HttpError::Malformed));
    }

    /// A peer must not be able to pin memory by never sending the header
    /// terminator.
    #[test]
    fn endless_headers_are_refused() {
        let mut buf = raw("GET / HTTP/1.1\n");
        buf.extend(std::iter::repeat_n(b'x', MAX_HEADER_BYTES + 1));
        assert_eq!(Request::parse(&buf), Err(HttpError::HeadersTooLarge));
    }

    #[test]
    fn a_junk_request_line_is_refused_not_panicked_on() {
        for junk in ["GET\n\n", "\n\n", "GET /only-two-parts\n\n"] {
            let buf = raw(junk);
            assert!(
                matches!(Request::parse(&buf), Err(HttpError::Malformed) | Ok(None)),
                "junk {junk:?} must not parse"
            );
        }
    }

    #[test]
    fn non_utf8_headers_are_refused() {
        let mut buf = b"GET /".to_vec();
        buf.extend_from_slice(&[0xFF, 0xFE]);
        buf.extend_from_slice(b" HTTP/1.1\r\n\r\n");
        assert_eq!(Request::parse(&buf), Err(HttpError::Malformed));
    }

    #[test]
    fn responses_carry_a_correct_content_length() {
        let out = response(200, content_type::JSON, br#"{"ok":true}"#);
        let text = String::from_utf8(out).expect("utf8");
        assert!(text.starts_with("HTTP/1.1 200 OK\r\n"));
        assert!(text.contains("Content-Length: 11\r\n"));
        assert!(text.ends_with(r#"{"ok":true}"#));
    }

    #[test]
    fn empty_responses_declare_zero_length() {
        let text = String::from_utf8(empty_response(204)).expect("utf8");
        assert!(text.starts_with("HTTP/1.1 204 No Content\r\n"));
        assert!(text.contains("Content-Length: 0\r\n"));
    }

    /// The event is what makes automations feel instant; it must be an
    /// `EVENT/1.0` message, not an HTTP response, or controllers ignore it.
    #[test]
    fn events_use_the_event_protocol_line() {
        let text = String::from_utf8(event(br#"{"characteristics":[]}"#)).expect("utf8");
        assert!(text.starts_with("EVENT/1.0 200 OK\r\n"));
        assert!(text.contains("application/hap+json"));
    }

    /// 470 is HAP's own status for "you have not authenticated yet"; a plain
    /// 401 makes controllers give up rather than start Pair Verify.
    #[test]
    fn hap_specific_statuses_have_reasons() {
        let text = String::from_utf8(empty_response(470)).expect("utf8");
        assert!(text.contains("470 Connection Authorization Required"));
    }
}
