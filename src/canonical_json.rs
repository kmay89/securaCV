//! Canonical JSON serialization ("securacv-cjson-v1").
//!
//! A deliberately small subset of RFC 8785 (JSON Canonicalization Scheme) used to
//! produce the byte string that evidence-envelope digests are computed over
//! (see `spec/evidence_envelope.md` §9). The subset is chosen so that the digest
//! can be reproduced byte-for-byte in JavaScript (`viewer/verify_core.js`) without
//! a full JCS implementation:
//!
//! - Object keys are sorted by UTF-16 code-unit order.
//! - No insignificant whitespace.
//! - Strings are escaped per RFC 8785.
//! - Only objects, arrays, strings, booleans, null, and **integers** are permitted.
//!   Floating-point numbers are rejected, so cross-language number formatting can
//!   never cause a digest to diverge. (Floats — e.g. `confidence: f32` — are bound
//!   into the envelope by reference via the artifact hash, not serialized here.)

use anyhow::{anyhow, Result};
use serde_json::Value;

/// The canonicalization scheme identifier recorded in the envelope manifest.
pub const CANONICALIZATION_ID: &str = "securacv-cjson-v1";

/// `Number.MAX_SAFE_INTEGER` (2^53 - 1). Integers must stay within ±this so the JS verifier,
/// which parses numbers as IEEE-754 doubles, reproduces the digest exactly.
const MAX_SAFE_INTEGER: i64 = 9_007_199_254_740_991;

/// Serialize a [`serde_json::Value`] to canonical bytes.
///
/// # Errors
/// Returns an error if the value (or any nested value) contains a floating-point number.
pub fn to_canonical_bytes(value: &Value) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    write_value(value, &mut out)?;
    Ok(out)
}

fn write_value(value: &Value, out: &mut Vec<u8>) -> Result<()> {
    match value {
        Value::Null => out.extend_from_slice(b"null"),
        Value::Bool(true) => out.extend_from_slice(b"true"),
        Value::Bool(false) => out.extend_from_slice(b"false"),
        Value::Number(n) => {
            // Reject any non-integer number. serde_json represents integers losslessly
            // via i64/u64; `is_f64()` is true only for values that required a float.
            if n.is_f64() {
                return Err(anyhow!(
                    "canonical JSON does not permit floating-point numbers (got {n})"
                ));
            }
            // Reject integers outside the JS safe-integer range. The offline verifier parses
            // numbers as IEEE-754 doubles, which lose precision beyond 2^53-1, so an out-of-range
            // integer would silently produce a different digest cross-language.
            let in_range = match (n.as_i64(), n.as_u64()) {
                (Some(v), _) => (-MAX_SAFE_INTEGER..=MAX_SAFE_INTEGER).contains(&v),
                (None, Some(v)) => v <= MAX_SAFE_INTEGER as u64,
                _ => false,
            };
            if !in_range {
                return Err(anyhow!(
                    "integer {n} is outside the JavaScript safe-integer range (±2^53-1)"
                ));
            }
            out.extend_from_slice(n.to_string().as_bytes());
        }
        Value::String(s) => write_string(s, out),
        Value::Array(items) => {
            out.push(b'[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push(b',');
                }
                write_value(item, out)?;
            }
            out.push(b']');
        }
        Value::Object(map) => {
            let mut keys: Vec<&String> = map.keys().collect();
            keys.sort_by(|a, b| cmp_utf16(a, b));
            out.push(b'{');
            for (i, key) in keys.iter().enumerate() {
                if i > 0 {
                    out.push(b',');
                }
                write_string(key, out);
                out.push(b':');
                write_value(&map[*key], out)?;
            }
            out.push(b'}');
        }
    }
    Ok(())
}

/// Compare two strings by UTF-16 code-unit order (RFC 8785 object key ordering).
fn cmp_utf16(a: &str, b: &str) -> std::cmp::Ordering {
    a.encode_utf16().cmp(b.encode_utf16())
}

/// Write a JSON string with RFC 8785 escaping.
fn write_string(s: &str, out: &mut Vec<u8>) {
    out.push(b'"');
    for ch in s.chars() {
        match ch {
            '"' => out.extend_from_slice(b"\\\""),
            '\\' => out.extend_from_slice(b"\\\\"),
            '\u{0008}' => out.extend_from_slice(b"\\b"),
            '\u{000C}' => out.extend_from_slice(b"\\f"),
            '\n' => out.extend_from_slice(b"\\n"),
            '\r' => out.extend_from_slice(b"\\r"),
            '\t' => out.extend_from_slice(b"\\t"),
            c if (c as u32) < 0x20 => {
                out.extend_from_slice(format!("\\u{:04x}", c as u32).as_bytes());
            }
            c => {
                let mut buf = [0u8; 4];
                out.extend_from_slice(c.encode_utf8(&mut buf).as_bytes());
            }
        }
    }
    out.push(b'"');
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn sorts_object_keys() {
        let v = json!({ "b": 1, "a": 2, "c": 3 });
        assert_eq!(to_canonical_bytes(&v).unwrap(), br#"{"a":2,"b":1,"c":3}"#);
    }

    #[test]
    fn nested_objects_sorted_recursively() {
        let v = json!({ "z": { "y": 1, "x": 2 }, "a": [3, 2, 1] });
        assert_eq!(
            to_canonical_bytes(&v).unwrap(),
            br#"{"a":[3,2,1],"z":{"x":2,"y":1}}"#
        );
    }

    #[test]
    fn no_insignificant_whitespace() {
        let v = json!({ "a": "x", "b": [1, 2] });
        let bytes = to_canonical_bytes(&v).unwrap();
        assert!(!bytes.contains(&b' '));
        assert!(!bytes.contains(&b'\n'));
    }

    #[test]
    fn escapes_control_and_quotes() {
        let v = json!({ "k": "a\"b\\c\nd\te" });
        assert_eq!(to_canonical_bytes(&v).unwrap(), br#"{"k":"a\"b\\c\nd\te"}"#);
    }

    #[test]
    fn rejects_floats() {
        let v = json!({ "confidence": 0.85 });
        let err = to_canonical_bytes(&v).unwrap_err();
        assert!(format!("{err}").contains("floating-point"));
    }

    #[test]
    fn integers_are_exact() {
        // Largest value still inside the JS safe-integer range (2^53 - 1).
        let v = json!({ "n": 9007199254740991u64 });
        assert_eq!(
            to_canonical_bytes(&v).unwrap(),
            br#"{"n":9007199254740991}"#
        );
    }

    #[test]
    fn rejects_integers_outside_js_safe_range() {
        let too_big = json!({ "n": 9007199254740992u64 });
        assert!(format!("{}", to_canonical_bytes(&too_big).unwrap_err()).contains("safe-integer"));

        let too_small = json!({ "n": -9007199254740992i64 });
        assert!(format!("{}", to_canonical_bytes(&too_small).unwrap_err()).contains("safe-integer"));
    }

    #[test]
    fn deterministic_regardless_of_input_order() {
        let a = json!({ "one": 1, "two": 2, "three": 3 });
        let b = json!({ "three": 3, "one": 1, "two": 2 });
        assert_eq!(
            to_canonical_bytes(&a).unwrap(),
            to_canonical_bytes(&b).unwrap()
        );
    }
}
