//! Entry points of the custody fuzz targets (`fuzz/fuzz_targets/custody_*.rs`).
//!
//! Every input here is one a stranger can hand a holder or an operator: a
//! share file, a custody container, a recipient file or a recipient key
//! file. Beyond "no panic", each decoder must agree with its own writer:
//! whatever decodes re-encodes to the same bytes. Nothing here derives a key
//! (the TEK1 parameter check runs before any KDF, and no target calls one).

use super::container::{decode_mkc1_header, encode_mkc1_header};
use super::recipient::{parse_tek1, tek1_from_text, RecipientPublic, RECIPIENT_FILE_MAX};
use super::share::ShareFile;

/// `custody_share_decode`: the text form and the binary form, then the
/// container copy's header.
pub fn share_decode(data: &[u8]) {
    if let Ok(file) = ShareFile::from_text(data) {
        // The text form is canonical: lowercase hex, one LF, nothing else.
        assert_eq!(
            file.to_text().as_bytes(),
            data,
            "text form does not round-trip"
        );
        check_share(&file);
    }
    if let Ok(file) = ShareFile::decode(data) {
        assert_eq!(file.encode(), data, "binary form does not round-trip");
        check_share(&file);
    }
}

fn check_share(file: &ShareFile) {
    let again = ShareFile::decode(&file.encode()).expect("a share we encoded failed to decode");
    assert_eq!(&again, file, "decode/encode is not stable");
    let hd = file.header_bytes();
    assert_eq!(&file.encode()[..hd.len()], &hd[..]);
    if !file.header.container.is_empty() {
        let _ = decode_mkc1_header(&file.header.container);
    }
}

/// `custody_container_decode`: the bounded header parse (no key).
pub fn container_decode(data: &[u8]) {
    if let Ok(header) = decode_mkc1_header(data) {
        let prefix = encode_mkc1_header(&header);
        assert_eq!(
            &data[..prefix.len()],
            &prefix[..],
            "the header does not re-encode to its own bytes"
        );
        assert_eq!(decode_mkc1_header(data).ok(), Some(header));
    }
}

/// `custody_recipient_decode`: the recipient JSON (8 KiB cap), then the
/// TEK1 header, from its text form and as raw bytes.
pub fn recipient_decode(data: &[u8]) {
    if data.len() <= RECIPIENT_FILE_MAX {
        if let Ok(public) = RecipientPublic::from_json(data) {
            let text = public.to_json_text();
            let again = RecipientPublic::from_json(text.as_bytes())
                .expect("a recipient file we wrote failed to parse");
            assert_eq!(again, public);
            assert_eq!(again.to_json_text(), text);
            let _ = public.check_binding_ed25519();
        }
    }
    if let Ok(tek1) = tek1_from_text(data) {
        let _ = parse_tek1(&tek1);
    }
    let _ = parse_tek1(data);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::vault::custody::kat::{fx, h, s};

    const SHARE_TEXT: &[u8] =
        include_bytes!("../../../fuzz/seeds/custody_share_decode/alice_transport.share");
    const SHARE_AT_REST: &[u8] =
        include_bytes!("../../../fuzz/seeds/custody_share_decode/alice_at_rest.bin");
    const MKC1_CLASSICAL: &[u8] =
        include_bytes!("../../../fuzz/seeds/custody_container_decode/classical.mkc1");
    const MKC1_HYBRID: &[u8] =
        include_bytes!("../../../fuzz/seeds/custody_container_decode/hybrid.mkc1");
    const RECIPIENT: &[u8] =
        include_bytes!("../../../fuzz/seeds/custody_recipient_decode/alice.recipient");
    const TEK1_TEXT: &[u8] =
        include_bytes!("../../../fuzz/seeds/custody_recipient_decode/alice_tek1.txt");

    #[test]
    fn fuzz_seeds_are_the_fixture_and_replay_clean() {
        // The seeds are the fixture's bytes, so they decode (a seed the
        // decoder refuses would fuzz from nothing).
        assert_eq!(
            SHARE_TEXT,
            s(&fx()["share_files"]["alice"]["text"]).as_bytes()
        );
        assert_eq!(
            SHARE_AT_REST,
            &h(&fx()["share_file_at_rest"]["file_hex"])[..]
        );
        assert_eq!(
            MKC1_CLASSICAL,
            &h(&fx()["mkc1"]["classical"]["container_hex"])[..]
        );
        assert_eq!(
            MKC1_HYBRID,
            &h(&fx()["mkc1"]["hybrid"]["container_hex"])[..]
        );
        assert_eq!(
            RECIPIENT,
            s(&fx()["recipients"]["alice"]["recipient_json"]).as_bytes()
        );
        assert_eq!(TEK1_TEXT, s(&fx()["tek1"]["file_text"]).as_bytes());
        assert!(ShareFile::from_text(SHARE_TEXT).is_ok());
        assert!(ShareFile::decode(SHARE_AT_REST).is_ok());
        assert!(decode_mkc1_header(MKC1_CLASSICAL).is_ok());
        assert!(decode_mkc1_header(MKC1_HYBRID).is_ok());
        assert!(RecipientPublic::from_json(RECIPIENT).is_ok());
        assert!(parse_tek1(&tek1_from_text(TEK1_TEXT).unwrap()).is_ok());
        share_decode(SHARE_TEXT);
        share_decode(SHARE_AT_REST);
        container_decode(MKC1_CLASSICAL);
        container_decode(MKC1_HYBRID);
        recipient_decode(RECIPIENT);
        recipient_decode(TEK1_TEXT);
        // Truncations and single-byte edits of every seed, as a fuzzer's
        // first mutations would make them: no panic, no unstable decode.
        for seed in [
            SHARE_TEXT,
            SHARE_AT_REST,
            MKC1_CLASSICAL,
            MKC1_HYBRID,
            RECIPIENT,
            TEK1_TEXT,
        ] {
            for cut in (0..seed.len()).step_by(37) {
                share_decode(&seed[..cut]);
                container_decode(&seed[..cut]);
                recipient_decode(&seed[..cut]);
            }
            for at in (0..seed.len()).step_by(53) {
                let mut edited = seed.to_vec();
                edited[at] ^= 0xa5;
                share_decode(&edited);
                container_decode(&edited);
                recipient_decode(&edited);
            }
        }
    }
}
