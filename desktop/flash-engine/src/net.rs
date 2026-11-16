//! The two HTTPS fetches a release flash makes: the live release manifest and
//! the factory image it names. rustls (no OpenSSL on any platform); TLS
//! identifies GitHub, and [`crate::release`] then identifies the bytes.
//!
//! These are the defaults behind [`crate::host::FlashHost::get_manifest`] and
//! [`crate::host::FlashHost::download`]; the error words are part of the
//! frontends' contract (see the `HTTP <code>` note below), so both apps get
//! them from this one place.

use serde_json::Value;
use std::time::Duration;

/// GET the release manifest as JSON — what the UI shows as the published
/// version of each product (mirrors the website's manifest state), and what
/// the flash pipeline resolves its factory image from. Callers gate `url`
/// against the bundled catalog first ([`crate::flash::fetch_manifest`]).
pub async fn get_manifest(url: String, user_agent: &'static str) -> Result<Value, String> {
    let client = reqwest::Client::builder()
        .user_agent(user_agent)
        .timeout(Duration::from_secs(30))
        .build()
        .map_err(|e| e.to_string())?;
    let resp = client
        .get(&url)
        .send()
        .await
        .map_err(|e| format!("couldn't reach the release manifest: {e}"))?;
    // The `HTTP <code>` token is a CONTRACT, not just prose: the frontends
    // match it to tell "the release we're pinned to has no images" (a real
    // answer — someone must cut that release) apart from a transport failure
    // above (offline, DNS, TLS), which proves nothing about whether the
    // release exists. Reword freely, but keep `HTTP <code>` in it or the UI
    // silently falls back to the cautious wording for every failure.
    if !resp.status().is_success() {
        return Err(format!(
            "no published release yet (manifest returned HTTP {}). You can still flash a local .bin.",
            resp.status().as_u16()
        ));
    }
    resp.json::<Value>()
        .await
        .map_err(|e| format!("release manifest is malformed: {e}"))
}

/// How many bytes a download may hold before it is cut off: the declared
/// size when there is one (`total > 0`), and never more than `max_bytes`
/// either way. Pure, so the bound is tested without a server.
pub fn download_limit(total: u64, max_bytes: u64) -> u64 {
    if total > 0 {
        total.min(max_bytes)
    } else {
        max_bytes
    }
}

/// The refusal for a body that ran past [`download_limit`]: it names which
/// bound was crossed, and the number.
pub fn download_overrun(total: u64, max_bytes: u64) -> String {
    if total > 0 && total <= max_bytes {
        format!("download exceeded the declared size ({total} bytes)")
    } else {
        format!("download exceeded the {max_bytes}-byte limit for a release asset")
    }
}

/// Download a release asset in chunks, calling `on_chunk(done, total)` after
/// each one. Chunked, not `bytes()`: on a slow link the transfer runs inside a
/// 300 s window with — otherwise — no output at all, which reads as a hang.
/// The release manifest's size is the honest `total` (the Content-Length can
/// be the compressed size behind a proxy); only a caller with no expected size
/// falls back to the Content-Length. Returns the bytes and that total.
///
/// The body is held in memory, so it is bounded: nothing is reserved past
/// `max_bytes` up front, and the transfer stops with an error the moment the
/// bytes received pass the declared size (or `max_bytes`, when no size is
/// known) — a server that sends more than the release said is not sending
/// the release, and the size and SHA-256 checks only run once the whole body
/// is in hand.
pub async fn download<F>(
    url: String,
    expected_size: u64,
    max_bytes: u64,
    user_agent: &'static str,
    mut on_chunk: F,
) -> Result<(Vec<u8>, u64), String>
where
    F: FnMut(usize, u64) + Send,
{
    // The image is a few MB; guard the connect so a dead network fails fast,
    // but give the transfer itself generous headroom on a slow link.
    let client = reqwest::Client::builder()
        .user_agent(user_agent)
        .connect_timeout(Duration::from_secs(15))
        .timeout(Duration::from_secs(300))
        .build()
        .map_err(|e| e.to_string())?;
    let mut resp = client
        .get(&url)
        .send()
        .await
        .map_err(|e| format!("download failed: {e}"))?
        .error_for_status()
        .map_err(|e| format!("download failed: {e}"))?;
    let total = if expected_size > 0 {
        expected_size
    } else {
        resp.content_length().unwrap_or(0)
    };
    let limit = download_limit(total, max_bytes);
    let mut downloaded: Vec<u8> = Vec::with_capacity(limit as usize);
    while let Some(chunk) = resp
        .chunk()
        .await
        .map_err(|e| format!("download failed: {e}"))?
    {
        if downloaded.len() as u64 + chunk.len() as u64 > limit {
            return Err(download_overrun(total, max_bytes));
        }
        downloaded.extend_from_slice(&chunk);
        on_chunk(downloaded.len(), total);
    }
    Ok((downloaded, total))
}

#[cfg(test)]
mod tests {
    use super::{download_limit, download_overrun};

    const CAP: u64 = crate::image::LOCAL_IMAGE_MAX_BYTES;

    #[test]
    fn a_download_is_bounded_by_its_declared_size_and_by_the_cap() {
        // A declared size is the bound, as long as it is under the cap…
        assert_eq!(download_limit(4_194_304, CAP), 4_194_304);
        assert_eq!(download_limit(CAP, CAP), CAP);
        // …a declared size past the cap never reserves or holds more than
        // the cap (2^40 used to go straight into Vec::with_capacity)…
        assert_eq!(download_limit(1 << 40, CAP), CAP);
        assert_eq!(download_limit(u64::MAX, CAP), CAP);
        // …and with no size known at all, the cap is the bound.
        assert_eq!(download_limit(0, CAP), CAP);
    }

    #[test]
    fn the_overrun_names_the_bound_that_was_crossed() {
        assert_eq!(
            download_overrun(1000, CAP),
            "download exceeded the declared size (1000 bytes)"
        );
        let unknown = download_overrun(0, CAP);
        assert!(unknown.contains(&CAP.to_string()), "{unknown}");
        assert!(unknown.contains("limit"), "{unknown}");
        assert_eq!(download_overrun(1 << 40, CAP), unknown);
    }
}
