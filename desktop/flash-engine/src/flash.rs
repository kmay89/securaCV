//! The ESP32 flash pipeline both desktop apps run: detect the chip, fetch the
//! pinned release manifest, then resolve → download → verify → provision →
//! (map the change) → (erase) → write → receipt.
//!
//! Each step is the Flasher's own, moved here verbatim when the Lab gained a
//! native flash path, so the two apps cannot flash differently: the same
//! closed set of manifest URLs, the same two chip guards (catalog and
//! release), the same release-origin and size/SHA-256/Ed25519 checks, the
//! same "settings sealed" wording, the same first-contact erase and the same
//! espflash invocation. The app supplies only the [`FlashHost`] — its sidecar
//! spawn and its event emitter — and its Tauri command wrappers keep the same
//! names, arguments and DTOs in both apps (desktop_parity.test.js).
//!
//! Events, identical in both apps: `flash:log` (one console line, a string),
//! `flash:progress` (`{stage, done, total}` while downloading) and
//! `flash:changemap` (what the install will touch, when a safety copy exists).

use crate::catalog::{Catalog, DEV_FLASH_MANIFEST_URL};
use crate::host::{run_capture, run_streaming, run_streaming_with_tail, FlashHost};
use crate::image::stage_firmware;
use crate::provisioning::Provisioning;
use crate::{changemap, intake, port_hint, provisioning, release, rescue, sidecar};
use serde::Serialize;
use serde_json::{json, Value};

/// What a successful write reports — the ESP32 receipt the frontends render
/// and keep. Field names and order are the wire contract of both apps.
#[derive(Serialize)]
pub struct FlashReceipt {
    pub target: &'static str,
    pub product_id: String,
    pub version: String,
    pub release_sha256: String,
    pub installed_sha256: String,
    pub bytes_written: usize,
    pub release_verification: &'static str,
    /// "stable" | "dev" for manifest flashes, "local" for a file off this
    /// computer's disk — so the receipt names which train the bytes came from.
    pub channel: &'static str,
    pub chip_write_verified: bool,
    pub provisioned: bool,
}

/// What `detect_chip` reports: the canonical chip name — the "you can't pick the
/// wrong image" guard, since the UI only offers products whose `chip` matches —
/// plus the flash size in bytes when board-info named it, which the rescue
/// bench's full-chip backup needs. Both come from the one board-info call.
#[derive(Serialize)]
pub struct ChipInfo {
    chip: String,
    flash_bytes: Option<u64>,
    mac: Option<String>,
    /// The read-only MAC sanity check (blank / multicast / locally
    /// administered). Reported here rather than kept to ourselves: a board
    /// whose address was never programmed is a clone or a reject, and the
    /// user deserves to know before they trust it with a network.
    mac_check: Option<serde_json::Value>,
}

/// The `flash` command's arguments, exactly as both apps' commands take them
/// from the webview (the frontend's `invoke("flash", {...})` keys, camelCased
/// by Tauri).
pub struct FlashRequest {
    pub port: String,
    pub product_id: String,
    pub manifest_url: String,
    pub baud: u32,
    pub detected_chip: String,
    pub provisioning: Option<Provisioning>,
    pub erase_first: Option<bool>,
    // The safety copy taken moments ago, if there is one. Present = draw the
    // change map from it; absent = the user skipped the copy or the board
    // wouldn't read, and no map is the truthful outcome.
    pub backup_path: Option<String>,
}

/// Ask the connected board which ESP32 it is (and how much flash it carries).
pub async fn detect_chip<H: FlashHost>(
    host: &H,
    bundled: &Catalog,
    port: String,
) -> Result<ChipInfo, String> {
    let (code, out) = run_capture(host, sidecar::board_info_args(port)).await?;
    // Check the exit code before parsing: a *failed* board-info can still print
    // a chip name in its error text, which would otherwise read as a false
    // positive detection.
    if code != 0 {
        // On Linux, a failed board-info is more often the OS refusing or
        // holding the port than a board out of download mode — when the
        // output names that cause, lead with its real fix instead of the
        // BOOT/RESET ritual (which can't help and reads as the board's
        // fault). Linux-only: the hint text is a Linux fix.
        if cfg!(target_os = "linux") {
            if let Some(hint) = port_hint::linux_open_hint(&out) {
                return Err(format!("{hint}\n\nespflash said:\n{}", out.trim()));
            }
        }
        return Err(format!(
            "couldn't read the chip (espflash exit {code}). Put the board in download mode (hold BOOT, tap RESET, release BOOT) and try again.\n\nespflash said:\n{}",
            out.trim()
        ));
    }
    match bundled.canonical_chip(&out) {
        Some(chip) => {
            let mac = rescue::parse_mac(&out);
            let mac_check = mac.as_deref().map(|m| {
                let f = intake::mac_checks(m);
                json!({ "level": f.level, "label": f.label, "detail": f.detail })
            });
            Ok(ChipInfo {
                chip,
                flash_bytes: rescue::parse_flash_size(&out),
                mac,
                mac_check,
            })
        }
        None => Err(format!(
            "couldn't recognize the chip from espflash's output:\n{}",
            out.trim()
        )),
    }
}

/// Fetch the live release manifest so the UI can show what version is
/// currently published for each product (mirrors the website's manifest
/// state). Reachable straight from the webview, so the closed-set gate
/// ([`Catalog::manifest_url_allowed`]) runs HERE before any socket opens.
pub async fn fetch_manifest<H: FlashHost>(
    host: &H,
    bundled: &Catalog,
    manifest_url: String,
) -> Result<Value, String> {
    if !bundled.manifest_url_allowed(&manifest_url) {
        return Err("refusing an unbundled manifest URL".into());
    }
    host.get_manifest(&manifest_url).await
}

/// The words a failed espflash write ends in — its exit code, the can't-brick
/// reassurance, and espflash's own last lines (`tail`), which the frontends
/// classify (permission / busy / not in download mode / cable).
pub fn write_failure(code: i32, tail: &[String]) -> String {
    format!(
        "espflash exited with code {code}. The board can't be bricked — put it back in download mode and try again.\n{}",
        tail.join("\n")
    )
}

/// Resolve → download → flash. Streams every line espflash prints over the
/// `flash:log` event so the UI is a live console, then returns Ok on a clean
/// exit or a human error otherwise.
pub async fn flash<H: FlashHost>(
    host: &H,
    bundled: &Catalog,
    request: FlashRequest,
) -> Result<FlashReceipt, String> {
    let FlashRequest {
        port,
        product_id,
        manifest_url,
        baud,
        detected_chip,
        provisioning,
        erase_first,
        backup_path,
    } = request;
    let emit = |line: String| host.emit("flash:log", line);

    // 1) Resolve the official factory image for this exact product.
    emit(format!("→ resolving verified release for {product_id}…"));
    let catalog: Value = serde_json::from_str(bundled.raw())
        .map_err(|e| format!("bundled catalog is corrupt: {e}"))?;
    // Exactly two manifests are ever resolved: the catalog's pinned stable
    // release, and the fixed fw-dev-latest constant. Anything else is an
    // unbundled URL and is refused before a byte moves.
    let channel =
        if catalog.get("manifest_url").and_then(Value::as_str) == Some(manifest_url.as_str()) {
            "stable"
        } else if manifest_url == DEV_FLASH_MANIFEST_URL {
            "dev"
        } else {
            return Err("refusing an unbundled firmware manifest URL".into());
        };
    if channel == "dev" {
        emit("→ DEV CHANNEL: resolving the rolling fw-dev-latest prerelease, not the pinned stable release".into(),
        );
    }
    let product = catalog
        .get("products")
        .and_then(Value::as_array)
        .and_then(|products| {
            products
                .iter()
                .find(|product| product.get("id").and_then(Value::as_str) == Some(&product_id))
        })
        .ok_or_else(|| format!("{product_id} is not in the bundled product catalog"))?;
    let catalog_chip = product
        .get("chip")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("{product_id} has no chip guard in the catalog"))?;
    if bundled.canonical_chip(catalog_chip) != bundled.canonical_chip(&detected_chip) {
        return Err(format!(
            "the catalog requires {catalog_chip}, but {detected_chip} was detected; refusing to write"
        ));
    }
    let needs_provisioning =
        product.get("provisioning").and_then(Value::as_str) == Some("usb-secrets");
    // The generated catalog derives this from the product's real serial-command
    // implementation. Missing fields fail closed for older/unknown catalogs.
    let expects_serial_receipt = product
        .get("serial_receipt")
        .and_then(Value::as_bool)
        .unwrap_or(true);
    if needs_provisioning && provisioning.is_none() {
        return Err(
            "this firmware uses generic release placeholders; Wi-Fi and MQTT provisioning is required before flash"
                .into(),
        );
    }

    let manifest = fetch_manifest(host, bundled, manifest_url).await?;
    if manifest.get("schema").and_then(Value::as_str) != Some("securacv-flash-1") {
        return Err("release manifest has an unexpected schema".into());
    }
    let entry = manifest
        .get("products")
        .and_then(|p| p.get(&product_id))
        .ok_or_else(|| format!("the release doesn't offer {product_id} yet"))?;
    let manifest_chip = entry
        .get("chipFamily")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("{product_id} has no chip family in the release"))?;
    if bundled.canonical_chip(manifest_chip) != bundled.canonical_chip(&detected_chip) {
        return Err(format!(
            "the release offers {manifest_chip}, but {detected_chip} is connected; refusing to write"
        ));
    }
    let factory_url = entry
        .get("factory")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("{product_id} has no factory image in the release"))?
        .to_string();
    if !bundled
        .release_origin()
        .is_some_and(|origin| factory_url.starts_with(origin))
    {
        return Err(
            "release image URL is outside the bundled SecuraCV GitHub release origin".into(),
        );
    }
    let version = entry
        .get("version")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("{product_id} has no version in the release"))?
        .to_string();
    let expected_size = entry
        .get("size")
        .and_then(Value::as_u64)
        .filter(|size| *size > 0)
        .ok_or_else(|| format!("{product_id} has an invalid release size"))?;
    let expected_sha = entry
        .get("sha256")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("{product_id} has no SHA-256 in the release"))?;
    emit(format!("→ version {version}"));

    // 2) Download it to a temp file. reqwest verifies TLS; GitHub serves the
    //    asset from the release the CI published.
    emit(format!("→ downloading {factory_url}"));
    // Chunked, not bytes(): on a slow link the transfer runs inside a 300 s
    // window with — before this — no output at all, which reads as a hang.
    // Every ~200 ms a structured `flash:progress` event carries done/total so
    // the UI can show a real bar; the release size is the honest total (the
    // Content-Length can be the compressed size behind a proxy). The fetch
    // itself is net::download (the host's default).
    let mut last_tick = std::time::Instant::now();
    let (downloaded, total) = host
        .download(&factory_url, expected_size, |done, total| {
            if last_tick.elapsed().as_millis() >= 200 {
                last_tick = std::time::Instant::now();
                host.emit(
                    "flash:progress",
                    serde_json::json!({ "stage": "download", "done": done, "total": total }),
                );
            }
        })
        .await?;
    host.emit(
        "flash:progress",
        serde_json::json!({ "stage": "download", "done": downloaded.len(), "total": total }),
    );

    // TLS identifies GitHub; these checks identify the actual release bytes.
    let release_sha = release::verify_size_and_sha(&downloaded, expected_size, expected_sha)?;
    let release_pubkey = catalog
        .get("release_pubkey")
        .and_then(Value::as_str)
        .ok_or_else(|| "bundled catalog has no release public key".to_string())?;
    let release_verification = release::verify_signature(
        downloaded.len(),
        &release_sha,
        entry.get("signature").and_then(Value::as_str),
        release_pubkey,
    )?;
    emit(format!(
        "✓ release verified: SHA-256 {}… ({release_verification})",
        &release_sha[..16]
    ));

    // Provision only after verifying the untouched release. The installed hash
    // records the exact per-device image we actually hand to espflash.
    let mut bytes = downloaded.to_vec();
    let provisioned = if let Some(config) = provisioning.as_ref() {
        provisioning::patch_factory_image(&mut bytes, config)?;
        // Name what was ACTUALLY sealed. This line used to claim "Wi-Fi + MQTT"
        // whenever any provisioning reached it — and for an on-glass display the
        // broker host is prefilled for the user, so a config carrying an EMPTY
        // network still arrived here and still printed the Wi-Fi claim. The
        // network field is not `required` for those products (their firmware can
        // be set up on the glass), so leaving it blank is silent by design; the
        // owner read a tick saying their network was baked in, watched the board
        // come up in its on-screen wizard anyway, and had no way to tell which of
        // the two things had actually happened. A receipt that overstates what
        // was written costs more than one that says less.
        let wifi = !config.wifi_ssid.is_empty();
        let broker = !config.mqtt_host.is_empty();
        let what = match (wifi, broker) {
            (true, true) => "network + hub",
            (true, false) => "network",
            (false, true) => "hub",
            (false, false) => "settings",
        };
        emit(format!(
            "✓ {what} sealed into the image's settings partition (values not logged)"
        ));
        if !wifi {
            emit("  no network baked in — this board asks for Wi-Fi itself on first boot".into());
        }
        true
    } else {
        false
    };
    let installed_sha = release::sha256_hex(&bytes);

    let safe_id: String = product_id
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect();
    // Keep the private file handle alive until espflash exits. Its RAII guard
    // removes the path on success and on every ordinary error return.
    let staged = stage_firmware(&bytes, &safe_id)?;
    let path = staged.path();
    emit(format!(
        "→ {} bytes staged (installed SHA-256 {}…), writing to the board…",
        bytes.len(),
        &installed_sha[..16]
    ));

    // 2a) The change map, computed HERE because this is the only moment both
    // sides exist: the safety copy has every byte that is on the board, and
    // `bytes` is the verified image about to replace them. A separate command
    // would have to download and verify the image a second time.
    //
    // Entirely best-effort and never fatal: a missing or unreadable backup
    // (the copy was skipped, or the board wouldn't read) means no map, which
    // is the honest answer. Failing the install because we couldn't draw a
    // picture of it would be absurd.
    if let Some(bp) = backup_path.as_deref().filter(|p| !p.is_empty()) {
        if let Ok(old) = std::fs::read(bp) {
            // Both facts the verdict needs are known right here: whether this
            // install erases the whole chip first (so regions the image never
            // reaches do NOT survive), and whether we just wrote the user's
            // own network into the replacement NVS (so a differing settings
            // region means "replaced with what you asked for", not "cleared").
            let erase_all = erase_first.unwrap_or(false);
            let baked_wifi = provisioning
                .as_ref()
                .map(|p| !p.wifi_ssid.is_empty())
                .unwrap_or(false);

            // Free intake check while we hold the whole chip: does the flash
            // really hold what it claims? A relabeled part (a 4 MB die sold as
            // 16 MB) ACCEPTS writes past its real end and discards them, so
            // the install "succeeds" and the board can't boot, with no error
            // at any layer. This costs no serial time — the bytes are already
            // here — and it is the last moment the write can still be stopped.
            // The safety copy was read with the chip's DECLARED size, so the
            // dump's own length is that claim — no extra argument needed.
            let declared = old.len() as u64;
            if declared >= 0x2000 {
                let f = intake::flash_alias_verdict(&old, declared);
                if f.level == "stop" {
                    emit(format!("✗ {}", f.label));
                    if let Some(d) = &f.detail {
                        emit(format!("  {d}"));
                    }
                    return Err(format!(
                        "{} {} Nothing was written.",
                        f.label,
                        f.detail.unwrap_or_default()
                    ));
                }
                // "clear" is the only level that earns a tick. An
                // inconclusive check (a blank chip, where a mirror and an
                // honest part read identically) is missing evidence, and
                // missing evidence dressed as a pass is the failure this
                // whole module exists to avoid — so it gets a warning marker
                // and keeps its explanation.
                if f.level == "clear" {
                    emit(format!("✓ {}", f.label));
                } else {
                    emit(format!("⚠ {}", f.label));
                    if let Some(d) = &f.detail {
                        emit(format!("  {d}"));
                    }
                }
            }
            if let Some(map) = changemap::diff_install(&old, &bytes, erase_all) {
                let had_wifi = old.windows(9).any(|w| w == b"wifi_ssid");
                let verdict = changemap::settings_verdict(&map, had_wifi, baked_wifi);
                host.emit(
                    "flash:changemap",
                    json!({
                        "layoutChanged": map.layout_changed,
                        "settings": verdict.map(|(kept, text)| json!({ "kept": kept, "text": text })),
                        "rows": map.rows.iter().map(|r| json!({
                            "label": r.label,
                            "kind": r.kind,
                            "offset": r.offset,
                            "size": r.size,
                            "verdict": r.verdict.as_str(),
                            "changedPct": r.changed_pct,
                            "before": r.before,
                            "after": r.after,
                        })).collect::<Vec<_>>(),
                    }),
                );
            }
        }
    }

    // 2b) First contact: wipe the WHOLE chip before writing. `write-bin` only
    //     touches the regions the image covers, so a board that arrived
    //     carrying somebody else's firmware would keep whatever sat in the
    //     partitions we don't write — on a board the user now believes is
    //     theirs. Erasing first is the only way that leftover goes away.
    //
    //     This mirrors the browser flasher, which forces the same erase on a
    //     board it has never written (canary-local/assets/intake.js:
    //     isFirstContact). The browser decides it by reading the board;
    //     espflash reports nothing about resident firmware, so here it comes
    //     from the step-1 checkbox.
    if erase_first.unwrap_or(false) {
        emit("→ first contact with this board — erasing the whole chip before writing".into());
        let code = run_streaming(host, rescue::erase_flash_args(&port), "flash:log").await?;
        if code != 0 {
            return Err(format!(
                "the full erase failed (espflash exit {code}). Nothing was written. The board can't be bricked — put it back in download mode and try again."
            ));
        }
        emit("✓ chip erased — nothing of the old firmware is left".into());
    }

    // 3) Flash the merged factory image at 0x0. A factory image already carries
    //    the bootloader/partition table at their real offsets, so 0x0 is right.
    //    espflash hard-resets the board when it's done.
    let args = rescue::write_bin_args(&port, &path.to_string_lossy(), baud);
    // Keep espflash's last words (run_streaming_with_tail): the frontends
    // classify a failure by its text, and without the tail every failure is
    // `unknown`. (read_region already did this.)
    let (code, tail) = run_streaming_with_tail(host, args, "flash:log").await?;
    // (`staged` removes the private image on scope exit.)

    if code == 0 {
        let message = if expects_serial_receipt {
            "✓ chip write verified — reopening serial for the live boot receipt."
        } else {
            "✓ chip write verified — this firmware does not require a live receipt."
        };
        emit(message.into());
        Ok(FlashReceipt {
            target: "esp32-host",
            product_id,
            version,
            release_sha256: release_sha,
            installed_sha256: installed_sha,
            bytes_written: bytes.len(),
            release_verification,
            channel,
            chip_write_verified: true,
            provisioned,
        })
    } else {
        Err(write_failure(code, &tail))
    }
}

#[cfg(test)]
mod tests {
    //! The whole pipeline, run against an in-memory host: every refusal the
    //! Flasher has always made is asserted to happen BEFORE the step it
    //! protects (no fetch, no download, no espflash), and the one espflash
    //! write that does happen is asserted argument by argument.
    use super::*;
    use crate::host::Emit;
    use ed25519_dalek::{Signer, SigningKey};
    use std::future::Future;
    use std::sync::Mutex;
    use std::task::{Context, Poll, Waker};

    /// Every host future below is ready on first poll, so a spin is a
    /// complete executor for these tests (no runtime dependency).
    fn block_on<F: Future>(fut: F) -> F::Output {
        let mut fut = std::pin::pin!(fut);
        let mut cx = Context::from_waker(Waker::noop());
        loop {
            if let Poll::Ready(v) = fut.as_mut().poll(&mut cx) {
                return v;
            }
        }
    }

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }

    /// The fixture's pinned manifest. Its release origin (everything through
    /// `/releases/download/`) is DERIVED, as the apps derive theirs — the
    /// desktop-parity test forbids a literal origin prefix anywhere in the
    /// native source, tests included.
    const MANIFEST_URL: &str =
        "https://github.com/example/fleet/releases/download/fw-v9.9.9/manifest-flash.json";

    struct Fixture {
        catalog: &'static Catalog,
        manifest_url: String,
        image: Vec<u8>,
        manifest: Value,
    }

    /// A catalog with a pinned test key, a manifest that key signed, and the
    /// image the manifest describes.
    fn fixture() -> Fixture {
        let key = SigningKey::from_bytes(&[7u8; 32]);
        let manifest_url = MANIFEST_URL.to_string();
        let raw = json!({
            "manifest_url": manifest_url,
            "release_pubkey": hex(key.verifying_key().as_bytes()),
            "chips": { "ESP32-S3": {}, "ESP32-C3": {} },
            "products": [
                { "id": "canary-s3", "chip": "ESP32-S3", "provisioning": "ap", "serial_receipt": true },
                { "id": "canary-quiet", "chip": "ESP32-S3", "provisioning": "ap", "serial_receipt": false },
                { "id": "canary-vision", "chip": "ESP32-C3", "provisioning": "usb-secrets" }
            ]
        })
        .to_string();
        let catalog: &'static Catalog =
            Box::leak(Box::new(Catalog::new(Box::leak(raw.into_boxed_str()))));
        let origin = catalog.release_origin().expect("fixture origin derives");
        let image: Vec<u8> = (0..4096u32).map(|i| (i % 251) as u8).collect();
        let sha = release::sha256_hex(&image);
        let mut message = Vec::from((image.len() as u32).to_le_bytes());
        message.extend_from_slice(&Sha::digest(&image));
        let signature = hex(&key.sign(&message).to_bytes());
        let entry = |chip: &str, factory: String| {
            json!({
                "chipFamily": chip, "factory": factory, "version": "9.9.9",
                "size": image.len(), "sha256": sha, "signature": signature
            })
        };
        let manifest = json!({
            "schema": "securacv-flash-1",
            "products": {
                "canary-s3": entry("ESP32-S3", format!("{origin}fw-v9.9.9/canary-s3.bin")),
                "canary-quiet": entry("ESP32-S3", format!("{origin}fw-v9.9.9/canary-quiet.bin")),
            }
        });
        Fixture {
            catalog,
            manifest_url,
            image,
            manifest,
        }
    }

    /// sha2's digest, without naming the version in every test.
    struct Sha;
    impl Sha {
        fn digest(bytes: &[u8]) -> Vec<u8> {
            use sha2::Digest;
            sha2::Sha256::digest(bytes).to_vec()
        }
    }

    /// Records everything; answers espflash call `n` with `espflash[n]`.
    struct Recorder {
        events: Mutex<Vec<(String, Value)>>,
        espflash_calls: Mutex<Vec<Vec<String>>>,
        espflash: Vec<(i32, &'static str)>,
        /// Whether the staged image still existed while espflash "ran".
        staged_seen: Mutex<Vec<bool>>,
        fetched: Mutex<Vec<String>>,
        manifest: Value,
        image: Vec<u8>,
    }

    impl Recorder {
        fn new(fx: &Fixture) -> Self {
            Recorder {
                events: Mutex::new(Vec::new()),
                espflash_calls: Mutex::new(Vec::new()),
                espflash: Vec::new(),
                staged_seen: Mutex::new(Vec::new()),
                fetched: Mutex::new(Vec::new()),
                manifest: fx.manifest.clone(),
                image: fx.image.clone(),
            }
        }
        fn log(&self) -> Vec<String> {
            self.events
                .lock()
                .unwrap()
                .iter()
                .filter(|(e, _)| e == "flash:log")
                .map(|(_, v)| v.as_str().unwrap().to_string())
                .collect()
        }
        fn calls(&self) -> Vec<Vec<String>> {
            self.espflash_calls.lock().unwrap().clone()
        }
    }

    impl Emit for Recorder {
        fn emit<P: Serialize + Clone>(&self, event: &str, payload: P) {
            let value = serde_json::to_value(payload).unwrap();
            self.events.lock().unwrap().push((event.to_string(), value));
        }
    }

    impl FlashHost for Recorder {
        const USER_AGENT: &'static str = "flash-engine-tests";

        fn espflash<F>(
            &self,
            args: Vec<String>,
            mut on_output: F,
        ) -> impl Future<Output = Result<i32, String>> + Send
        where
            F: FnMut(&[u8]) + Send,
        {
            let n = {
                let mut calls = self.espflash_calls.lock().unwrap();
                calls.push(args.clone());
                calls.len() - 1
            };
            if args.first().map(String::as_str) == Some("write-bin") {
                let exists = std::path::Path::new(&args[2]).exists();
                self.staged_seen.lock().unwrap().push(exists);
            }
            let (code, out) = self.espflash.get(n).copied().unwrap_or((0, ""));
            on_output(out.as_bytes());
            async move { Ok(code) }
        }

        fn get_manifest(&self, url: &str) -> impl Future<Output = Result<Value, String>> + Send {
            self.fetched.lock().unwrap().push(url.to_string());
            let manifest = self.manifest.clone();
            async move { Ok(manifest) }
        }

        fn download<F>(
            &self,
            url: &str,
            expected_size: u64,
            mut on_chunk: F,
        ) -> impl Future<Output = Result<(Vec<u8>, u64), String>> + Send
        where
            F: FnMut(usize, u64) + Send,
        {
            self.fetched.lock().unwrap().push(url.to_string());
            on_chunk(self.image.len(), expected_size);
            let image = self.image.clone();
            async move { Ok((image, expected_size)) }
        }
    }

    fn request(fx: &Fixture, product: &str, chip: &str) -> FlashRequest {
        FlashRequest {
            port: "/dev/ttyACM0".into(),
            product_id: product.into(),
            manifest_url: fx.manifest_url.clone(),
            baud: 460_800,
            detected_chip: chip.into(),
            provisioning: None,
            erase_first: None,
            backup_path: None,
        }
    }

    #[test]
    fn a_verified_release_is_written_once_at_offset_zero_and_receipted() {
        let fx = fixture();
        let host = Recorder::new(&fx);
        let receipt = block_on(flash(
            &host,
            fx.catalog,
            request(&fx, "canary-s3", "esp32s3"),
        ))
        .unwrap();

        // Exactly one espflash run: the write, at 0x0, on the asked port+baud,
        // from a staged file that existed while espflash ran and is gone now.
        let calls = host.calls();
        assert_eq!(calls.len(), 1, "{calls:?}");
        let write = &calls[0];
        assert_eq!(&write[..2], ["write-bin", "0x0"]);
        assert_eq!(&write[3..], ["--port", "/dev/ttyACM0", "--baud", "460800"]);
        assert_eq!(*host.staged_seen.lock().unwrap(), vec![true]);
        assert!(
            !std::path::Path::new(&write[2]).exists(),
            "staged image left behind"
        );

        // The manifest, then the image — nothing else was fetched.
        assert_eq!(
            *host.fetched.lock().unwrap(),
            vec![
                fx.manifest_url.clone(),
                format!(
                    "{}fw-v9.9.9/canary-s3.bin",
                    fx.catalog.release_origin().unwrap()
                )
            ]
        );

        let sha = release::sha256_hex(&fx.image);
        assert_eq!(receipt.target, "esp32-host");
        assert_eq!(receipt.product_id, "canary-s3");
        assert_eq!(receipt.version, "9.9.9");
        assert_eq!(receipt.release_sha256, sha);
        assert_eq!(receipt.installed_sha256, sha, "no provisioning, same bytes");
        assert_eq!(receipt.bytes_written, fx.image.len());
        assert_eq!(receipt.release_verification, "ed25519+sha256");
        assert_eq!(receipt.channel, "stable");
        assert!(receipt.chip_write_verified);
        assert!(!receipt.provisioned);

        let log = host.log();
        assert!(log
            .iter()
            .any(|l| l.starts_with("✓ release verified: SHA-256 ")
                && l.ends_with("(ed25519+sha256)")));
        assert_eq!(
            log.last().map(String::as_str),
            Some("✓ chip write verified — reopening serial for the live boot receipt.")
        );
        // The final progress event is the whole image against the release size.
        let events = host.events.lock().unwrap();
        let last_progress = events
            .iter()
            .rev()
            .find(|(e, _)| e == "flash:progress")
            .unwrap();
        assert_eq!(
            last_progress.1,
            json!({ "stage": "download", "done": fx.image.len(), "total": fx.image.len() })
        );
        // No safety copy was offered, so no change map is drawn.
        assert!(!events.iter().any(|(e, _)| e == "flash:changemap"));
    }

    #[test]
    fn a_firmware_without_a_serial_receipt_says_so() {
        let fx = fixture();
        let host = Recorder::new(&fx);
        block_on(flash(
            &host,
            fx.catalog,
            request(&fx, "canary-quiet", "ESP32-S3"),
        ))
        .unwrap();
        assert_eq!(
            host.log().last().map(String::as_str),
            Some("✓ chip write verified — this firmware does not require a live receipt.")
        );
    }

    #[test]
    fn an_unbundled_manifest_is_refused_before_any_io() {
        let fx = fixture();
        let host = Recorder::new(&fx);
        let mut req = request(&fx, "canary-s3", "ESP32-S3");
        req.manifest_url = format!("{MANIFEST_URL}.evil");
        let err = block_on(flash(&host, fx.catalog, req)).err().unwrap();
        assert_eq!(err, "refusing an unbundled firmware manifest URL");
        assert!(host.fetched.lock().unwrap().is_empty());
        assert!(host.calls().is_empty());

        // The webview-reachable fetch applies the same closed set.
        let err = block_on(fetch_manifest(
            &host,
            fx.catalog,
            "https://example.com/m.json".into(),
        ))
        .err()
        .unwrap();
        assert_eq!(err, "refusing an unbundled manifest URL");
        assert!(host.fetched.lock().unwrap().is_empty());
    }

    #[test]
    fn the_dev_channel_is_the_one_other_manifest_and_is_announced() {
        let fx = fixture();
        let host = Recorder::new(&fx);
        let mut req = request(&fx, "canary-s3", "ESP32-S3");
        req.manifest_url = DEV_FLASH_MANIFEST_URL.into();
        let receipt = block_on(flash(&host, fx.catalog, req)).unwrap();
        assert_eq!(receipt.channel, "dev");
        assert!(host.log().iter().any(|l| l.starts_with("→ DEV CHANNEL:")));
        assert_eq!(host.fetched.lock().unwrap()[0], DEV_FLASH_MANIFEST_URL);
    }

    #[test]
    fn the_catalog_chip_guard_refuses_before_the_manifest_is_fetched() {
        let fx = fixture();
        let host = Recorder::new(&fx);
        let err = block_on(flash(
            &host,
            fx.catalog,
            request(&fx, "canary-s3", "ESP32-C3"),
        ))
        .err()
        .unwrap();
        assert_eq!(
            err,
            "the catalog requires ESP32-S3, but ESP32-C3 was detected; refusing to write"
        );
        assert!(host.fetched.lock().unwrap().is_empty());
        assert!(host.calls().is_empty());
    }

    #[test]
    fn provisioned_firmware_refuses_to_flash_without_its_settings() {
        let fx = fixture();
        let host = Recorder::new(&fx);
        let err = block_on(flash(
            &host,
            fx.catalog,
            request(&fx, "canary-vision", "ESP32-C3"),
        ))
        .err()
        .unwrap();
        assert!(
            err.contains("provisioning is required before flash"),
            "{err}"
        );
        assert!(host.fetched.lock().unwrap().is_empty());
    }

    #[test]
    fn the_release_chip_guard_and_the_origin_guard_refuse_before_download() {
        let fx = fixture();
        let mut host = Recorder::new(&fx);
        host.manifest["products"]["canary-s3"]["chipFamily"] = json!("ESP32-C3");
        let err = block_on(flash(
            &host,
            fx.catalog,
            request(&fx, "canary-s3", "ESP32-S3"),
        ))
        .err()
        .unwrap();
        assert_eq!(
            err,
            "the release offers ESP32-C3, but ESP32-S3 is connected; refusing to write"
        );
        assert_eq!(
            host.fetched.lock().unwrap().len(),
            1,
            "manifest only, no download"
        );

        let mut host = Recorder::new(&fx);
        host.manifest["products"]["canary-s3"]["factory"] =
            json!("https://example.com/releases/download/fw-v9.9.9/canary-s3.bin");
        let err = block_on(flash(
            &host,
            fx.catalog,
            request(&fx, "canary-s3", "ESP32-S3"),
        ))
        .err()
        .unwrap();
        assert_eq!(
            err,
            "release image URL is outside the bundled SecuraCV GitHub release origin"
        );
        assert_eq!(
            host.fetched.lock().unwrap().len(),
            1,
            "manifest only, no download"
        );
        assert!(host.calls().is_empty());
    }

    #[test]
    fn bytes_that_do_not_match_the_release_are_never_written() {
        let fx = fixture();
        let mut host = Recorder::new(&fx);
        host.image[0] ^= 0xff;
        let err = block_on(flash(
            &host,
            fx.catalog,
            request(&fx, "canary-s3", "ESP32-S3"),
        ))
        .err()
        .unwrap();
        assert!(err.starts_with("SHA-256 mismatch"), "{err}");
        assert!(host.calls().is_empty());

        // A signature that no longer covers the bytes fails closed too.
        let mut host = Recorder::new(&fx);
        host.manifest["products"]["canary-s3"]["signature"] = json!("00".repeat(64));
        let err = block_on(flash(
            &host,
            fx.catalog,
            request(&fx, "canary-s3", "ESP32-S3"),
        ))
        .err()
        .unwrap();
        assert_eq!(
            err,
            "the image failed Ed25519 release-signature verification"
        );
        assert!(host.calls().is_empty());
    }

    #[test]
    fn first_contact_erases_the_whole_chip_before_the_write() {
        let fx = fixture();
        let host = Recorder::new(&fx);
        let mut req = request(&fx, "canary-s3", "ESP32-S3");
        req.erase_first = Some(true);
        block_on(flash(&host, fx.catalog, req)).unwrap();
        let calls = host.calls();
        assert_eq!(calls.len(), 2);
        assert_eq!(calls[0], rescue::erase_flash_args("/dev/ttyACM0"));
        assert_eq!(calls[1][0], "write-bin");

        // An erase that fails stops everything: nothing is written.
        let mut host = Recorder::new(&fx);
        host.espflash = vec![(1, "Error: erase failed\n")];
        let mut req = request(&fx, "canary-s3", "ESP32-S3");
        req.erase_first = Some(true);
        let err = block_on(flash(&host, fx.catalog, req)).err().unwrap();
        assert!(err.starts_with("the full erase failed (espflash exit 1). Nothing was written."));
        assert_eq!(host.calls().len(), 1, "no write after a failed erase");
    }

    #[test]
    fn a_failed_write_carries_espflashs_last_eight_lines() {
        let fx = fixture();
        let mut host = Recorder::new(&fx);
        host.espflash = vec![(
            2,
            "line 1\nline 2\nline 3\nline 4\r\nline 5\nline 6\nline 7\nline 8\n  line 9  \n\n",
        )];
        let err = block_on(flash(
            &host,
            fx.catalog,
            request(&fx, "canary-s3", "ESP32-S3"),
        ))
        .err()
        .unwrap();
        let tail: Vec<String> = (2..=9).map(|i| format!("line {i}")).collect();
        assert_eq!(err, write_failure(2, &tail));
        assert!(err.starts_with(
            "espflash exited with code 2. The board can't be bricked — put it back in download mode and try again.\nline 2\n"
        ));
        // Every line also streamed to the console, untrimmed but non-empty.
        assert!(host.log().iter().any(|l| l == "  line 9  "));
    }

    #[test]
    fn a_counterfeit_capacity_stops_the_install_from_the_safety_copy() {
        // The safety copy of a "2 MB" part whose upper half mirrors the lower
        // half — a relabeled 1 MB die. The alias check runs on the copy the
        // app already holds, before a byte is written.
        let fx = fixture();
        let host = Recorder::new(&fx);
        let half: Vec<u8> = (0..1024 * 1024u32)
            .map(|i| (i.wrapping_mul(2_654_435_761) >> 13) as u8)
            .collect();
        let mut dump = half.clone();
        dump.extend_from_slice(&half);
        let backup = tempfile_with(&dump);
        let mut req = request(&fx, "canary-s3", "ESP32-S3");
        req.backup_path = Some(backup.path().to_string_lossy().into_owned());
        let err = block_on(flash(&host, fx.catalog, req)).err().unwrap();
        assert!(err.ends_with("Nothing was written."), "{err}");
        assert!(
            host.calls().is_empty(),
            "nothing may be written to an aliased part"
        );
    }

    #[test]
    fn a_safety_copy_draws_the_change_map() {
        let fx = fixture();
        let host = Recorder::new(&fx);
        // A believable old image: the new one, a byte changed.
        let mut old = fx.image.clone();
        old[10] ^= 1;
        let backup = tempfile_with(&old);
        let mut req = request(&fx, "canary-s3", "ESP32-S3");
        req.backup_path = Some(backup.path().to_string_lossy().into_owned());
        block_on(flash(&host, fx.catalog, req)).unwrap();
        let events = host.events.lock().unwrap();
        // Whether a map can be drawn depends on the image carrying a
        // partition table; this one has none, so no map is the honest answer
        // and the write still happens.
        assert_eq!(
            events.iter().any(|(e, _)| e == "flash:changemap"),
            changemap::diff_install(&old, &fx.image, false).is_some()
        );
        assert_eq!(host.calls().len(), 1);
    }

    fn tempfile_with(bytes: &[u8]) -> tempfile::NamedTempFile {
        use std::io::Write;
        let mut f = tempfile::NamedTempFile::new().unwrap();
        f.write_all(bytes).unwrap();
        f
    }

    #[test]
    fn detect_chip_reads_the_chip_the_flash_size_and_the_mac() {
        let fx = fixture();
        let mut host = Recorder::new(&fx);
        host.espflash = vec![(
            0,
            "Chip type:         esp32s3 (revision v0.2)\nFlash size:        8MB\nMAC address:       dc:54:75:c1:22:30\n",
        )];
        let info = block_on(detect_chip(&host, fx.catalog, "/dev/ttyACM0".into())).unwrap();
        assert_eq!(
            host.calls(),
            vec![sidecar::board_info_args("/dev/ttyACM0".into())]
        );
        let info = serde_json::to_value(info).unwrap();
        assert_eq!(info["chip"], "ESP32-S3");
        assert_eq!(info["flash_bytes"], 8 * 1024 * 1024);
        assert_eq!(info["mac"], "dc:54:75:c1:22:30");
        assert!(info["mac_check"]["level"].is_string());
    }

    #[test]
    fn a_failed_board_info_is_never_read_as_a_chip() {
        // The error text names a chip; the exit code wins.
        let fx = fixture();
        let mut host = Recorder::new(&fx);
        host.espflash = vec![(1, "Error: Failed to connect to the device (esp32s3?)\n")];
        let err = block_on(detect_chip(&host, fx.catalog, "/dev/ttyACM0".into()))
            .err()
            .unwrap();
        assert!(
            err.starts_with("couldn't read the chip (espflash exit 1)."),
            "{err}"
        );
        assert!(err.contains("esp32s3?"), "espflash's own words are kept");

        // On Linux a permission failure leads with its real fix.
        let mut host = Recorder::new(&fx);
        host.espflash = vec![(1, "Error: Permission denied (os error 13)\n")];
        let err = block_on(detect_chip(&host, fx.catalog, "/dev/ttyACM0".into()))
            .err()
            .unwrap();
        if cfg!(target_os = "linux") {
            assert!(err.starts_with(port_hint::PERMISSION_HINT), "{err}");
        }
    }
}
