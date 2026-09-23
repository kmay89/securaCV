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

/// Download a release asset in chunks, calling `on_chunk(done, total)` after
/// each one. Chunked, not `bytes()`: on a slow link the transfer runs inside a
/// 300 s window with — otherwise — no output at all, which reads as a hang.
/// The release manifest's size is the honest `total` (the Content-Length can
/// be the compressed size behind a proxy); only a caller with no expected size
/// falls back to the Content-Length. Returns the bytes and that total.
pub async fn download<F>(
    url: String,
    expected_size: u64,
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
    let mut downloaded: Vec<u8> = Vec::with_capacity(total as usize);
    while let Some(chunk) = resp
        .chunk()
        .await
        .map_err(|e| format!("download failed: {e}"))?
    {
        downloaded.extend_from_slice(&chunk);
        on_chunk(downloaded.len(), total);
    }
    Ok((downloaded, total))
}
