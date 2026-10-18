//! Canonical JSON, "securacv-cjson-v1", implemented locally.
//!
//! The same rules as the root crate's `src/canonical_json.rs`, re-implemented
//! here because this crate must not depend on the root crate (the checker is
//! a separate crate and process from witnessd). The test
//! `pinned_vector_from_root_rules` holds the two to each other byte for byte.
//!
//! - Object keys sorted by UTF-16 code-unit order.
//! - No insignificant whitespace.
//! - Strings escaped per RFC 8785.
//! - Only objects, arrays, strings, booleans, null and integers within the
//!   JavaScript safe-integer range (±(2^53 − 1)). Floats are refused.

use crate::{Error, Result};
use serde_json::Value;

/// The scheme identifier.
pub const CANONICALIZATION_ID: &str = "securacv-cjson-v1";

/// `Number.MAX_SAFE_INTEGER` (2^53 − 1).
pub const MAX_SAFE_INTEGER: u64 = 9_007_199_254_740_991;

/// Serialize a value to canonical bytes.
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
            if n.is_f64() {
                return Err(Error::Invalid("canonical JSON does not permit floats"));
            }
            let in_range = match (n.as_i64(), n.as_u64()) {
                (Some(v), _) => v.unsigned_abs() <= MAX_SAFE_INTEGER,
                (None, Some(v)) => v <= MAX_SAFE_INTEGER,
                _ => false,
            };
            if !in_range {
                return Err(Error::Invalid("integer outside the JS safe range"));
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
            keys.sort_by(|a, b| a.encode_utf16().cmp(b.encode_utf16()));
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

    /// The input below was serialized by the ROOT crate's
    /// `canonical_json::to_canonical_bytes` (src/canonical_json.rs, compiled
    /// standalone on 2026-10-13), and its output pinned here as hex. It
    /// exercises key order by UTF-16 code unit (U+1F600, a surrogate pair,
    /// sorts BEFORE U+FB01, the reverse of code-point order), every short
    /// escape, \u00XX control escapes, raw DEL, the safe-integer bounds and
    /// empty containers. If this test fails, the two implementations diverged.
    #[test]
    fn pinned_vector_from_root_rules() {
        let input = r#"{"v":1,"household":"00112233445566778899aabbccddeeff","epoch":7,"prev":"0000000000000000000000000000000000000000000000000000000000000000","threshold":2,"admins":[{"public_key":"ab","kind":"cli","alg":"ed25519","handle":"0102"}],"members":[{"not_after":null,"handle":"0a0b","kind":"phone","class":"intent","alg":"es256","public_key":"04ff"}],"é":"café \"q\" \\ \n\t\b\f\r\u0001\u001f\u007f","ﬁ":true,"😀":false,"Z":[-9007199254740991,9007199254740991,0,{}],"a":[]}"#;
        let expected_hex = concat!(
            "7b225a223a5b2d393030373139393235343734303939312c39303037313939323534373430393931",
            "2c302c7b7d5d2c2261223a5b5d2c2261646d696e73223a5b7b22616c67223a226564323535313922",
            "2c2268616e646c65223a2230313032222c226b696e64223a22636c69222c227075626c69635f6b65",
            "79223a226162227d5d2c2265706f6368223a372c22686f757365686f6c64223a2230303131323233",
            "33343435353636373738383939616162626363646465656666222c226d656d62657273223a5b7b22",
            "616c67223a226573323536222c22636c617373223a22696e74656e74222c2268616e646c65223a22",
            "30613062222c226b696e64223a2270686f6e65222c226e6f745f6166746572223a6e756c6c2c2270",
            "75626c69635f6b6579223a2230346666227d5d2c2270726576223a22303030303030303030303030",
            "30303030303030303030303030303030303030303030303030303030303030303030303030303030",
            "303030303030303030303030222c227468726573686f6c64223a322c2276223a312c22c3a9223a22",
            "636166c3a9205c22715c22205c5c205c6e5c745c625c665c725c75303030315c75303031667f222c",
            "22f09f9880223a66616c73652c22efac81223a747275657d",
        );
        let v: Value = serde_json::from_str(input).unwrap();
        let got = to_canonical_bytes(&v).unwrap();
        assert_eq!(crate::hex::encode(&got), expected_hex);
    }

    #[test]
    fn refuses_floats_and_unsafe_integers() {
        assert!(to_canonical_bytes(&json!({"x": 0.5})).is_err());
        assert!(to_canonical_bytes(&json!({"x": 9_007_199_254_740_992u64})).is_err());
        assert!(to_canonical_bytes(&json!({"x": -9_007_199_254_740_992i64})).is_err());
        assert!(to_canonical_bytes(&json!({"x": 9_007_199_254_740_991u64})).is_ok());
    }

    #[test]
    fn order_independent() {
        let a = json!({"b": 1, "a": {"d": 2, "c": [3]}});
        let b = json!({"a": {"c": [3], "d": 2}, "b": 1});
        assert_eq!(
            to_canonical_bytes(&a).unwrap(),
            to_canonical_bytes(&b).unwrap()
        );
        assert_eq!(
            to_canonical_bytes(&a).unwrap(),
            br#"{"a":{"c":[3],"d":2},"b":1}"#
        );
    }
}
