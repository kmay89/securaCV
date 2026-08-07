//! ESP-IDF NVS provisioning for generic release images.
//!
//! Vision release binaries intentionally contain placeholder network values.
//! The desktop flasher writes the real values into the image's own `nvs`
//! partition before flashing. Values never leave this function except as NVS
//! bytes, and callers must not log `Provisioning`.

use serde::Deserialize;

const PARTITION_TABLE_OFFSET: usize = 0x8000;
const PARTITION_MAGIC: u16 = 0x50aa;
const NVS_PAGE: usize = 4096;
const NVS_ITEMS: usize = 126;

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Provisioning {
    pub device_id: String,
    pub wifi_ssid: String,
    pub wifi_pass: String,
    pub mqtt_host: String,
    pub mqtt_port: u16,
    pub mqtt_user: String,
    pub mqtt_pass: String,
    // Which NVS encoding this firmware reads its Wi-Fi with (catalog
    // `wifi_nvs`, derived from the firmware source): "string" (Preferences
    // getString — sense/vision/display) or "blob" (getBytes + a wifi_en
    // bool — canary/wap). Defaults to "string" for old callers.
    #[serde(default)]
    pub wifi_nvs: String,
    // The device's local-API bearer credential ("cv_" + 32 base62 chars),
    // minted by the flasher so the fleet book can talk to the board later.
    // Both auth loaders check NVS BEFORE deriving their own token
    // (securacv_auth.cpp auth_load_or_derive; canary_wap.ino nvs_load_token),
    // so a seeded credential simply becomes the device's credential. Empty =
    // don't seed; the firmware derives one from its own key as always.
    #[serde(default)]
    pub api_token: String,
}

// Identity, broker, and network are INDEPENDENT capabilities (PR #1351's
// lesson, finished): a display is on-glass so it never gets a device id here,
// yet it reads a broker out of NVS — Wi-Fi + broker with no identity is its
// normal provisioning. A usb-secrets board provides all three. Each field is
// therefore validated and written only when present; which fields are
// REQUIRED is the frontend's decision per product. (Tying the broker to the
// device id here made the native flasher abort a display's whole flash —
// Wi-Fi included — with an error naming a field its UI deliberately hides.)

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Partition {
    offset: usize,
    size: usize,
}

fn u16le(bytes: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(
        bytes.get(offset..offset + 2)?.try_into().ok()?,
    ))
}

fn u32le(bytes: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        bytes.get(offset..offset + 4)?.try_into().ok()?,
    ))
}

fn find_nvs_partition(image: &[u8]) -> Result<Partition, String> {
    let table = image
        .get(PARTITION_TABLE_OFFSET..)
        .ok_or_else(|| "factory image has no ESP32 partition table".to_string())?;
    for offset in (0..table.len().min(0xc00)).step_by(32) {
        if u16le(table, offset) != Some(PARTITION_MAGIC) {
            break;
        }
        if table[offset + 2] == 0x01 && table[offset + 3] == 0x02 {
            let part_offset = u32le(table, offset + 4).unwrap_or(0) as usize;
            let part_size = u32le(table, offset + 8).unwrap_or(0) as usize;
            if part_size < NVS_PAGE || part_offset.checked_add(part_size).is_none() {
                return Err("factory image declares an invalid NVS partition".into());
            }
            return Ok(Partition {
                offset: part_offset,
                size: part_size,
            });
        }
    }
    Err("factory image has no NVS settings partition".into())
}

fn crc32_esp_rom(bytes: &[u8]) -> u32 {
    let mut crc = 0u32;
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb8_8320 & 0u32.wrapping_sub(crc & 1));
        }
    }
    !crc
}

fn validate(config: &Provisioning) -> Result<(), String> {
    let byte_len = |value: &str| value.len();
    if config.wifi_ssid.is_empty() {
        // Broker-only provisioning is legitimate (a display kept on its
        // on-glass Wi-Fi setup, but told its hub here) — an empty network is
        // "skip Wi-Fi", not an error. A password WITHOUT a network is the one
        // nonsensical shape.
        if !config.wifi_pass.is_empty() {
            return Err("Wi-Fi password given without a network name".into());
        }
    } else {
        if byte_len(&config.wifi_ssid) > 32 {
            return Err("Wi-Fi name must be 1–32 UTF-8 bytes".into());
        }
        if !config.wifi_pass.is_empty() && !(8..=63).contains(&byte_len(&config.wifi_pass)) {
            return Err("Wi-Fi password must be 8–63 bytes (or empty for an open network)".into());
        }
    }
    if !config.device_id.is_empty() && byte_len(&config.device_id) > 47 {
        return Err("device name must be 1–47 UTF-8 bytes".into());
    }
    if !config.mqtt_host.is_empty() {
        if byte_len(&config.mqtt_host) > 63 {
            return Err("MQTT host must be 1–63 UTF-8 bytes".into());
        }
        if byte_len(&config.mqtt_user) > 32 || byte_len(&config.mqtt_pass) > 64 {
            return Err("MQTT username or password is too long for the firmware".into());
        }
        if config.mqtt_port == 0 {
            return Err("MQTT port must be between 1 and 65535".into());
        }
    }
    if !config.api_token.is_empty() && !api_token_shape_ok(&config.api_token) {
        return Err("device API token must be \"cv_\" + 32 base62 characters".into());
    }
    Ok(())
}

/// The exact credential shape both firmware loaders accept: "cv_" + 32 base62
/// chars (auth_load_or_derive requires the prefix and >= 35 bytes; the wap's
/// nvs_load_token the same). Anything else would seed a token the device
/// ignores — worse than not seeding, because the flasher would keep a copy it
/// believes in.
fn api_token_shape_ok(token: &str) -> bool {
    token.len() == 35
        && token.starts_with("cv_")
        && token.as_bytes()[3..]
            .iter()
            .all(|b| b.is_ascii_alphanumeric())
}

struct NvsWriter {
    page: Vec<u8>,
    next: usize,
}

impl NvsWriter {
    fn new() -> Self {
        Self {
            page: vec![0xff; NVS_PAGE],
            next: 0,
        }
    }

    fn item_offset(index: usize) -> usize {
        64 + index * 32
    }

    fn mark_written(&mut self, index: usize) {
        self.page[32 + (index >> 2)] &= !(1 << ((index & 3) * 2));
    }

    fn header(&mut self, index: usize, namespace: u8, kind: u8, span: u8, key: &str) {
        let offset = Self::item_offset(index);
        self.page[offset] = namespace;
        self.page[offset + 1] = kind;
        self.page[offset + 2] = span;
        self.page[offset + 3] = 0xff;
        for (slot, byte) in self.page[offset + 8..offset + 24]
            .iter_mut()
            .zip(key.bytes())
        {
            *slot = byte;
        }
    }

    fn finish_item(&mut self, index: usize) {
        let offset = Self::item_offset(index);
        let mut covered = [0u8; 28];
        covered[..4].copy_from_slice(&self.page[offset..offset + 4]);
        covered[4..].copy_from_slice(&self.page[offset + 8..offset + 32]);
        let crc = crc32_esp_rom(&covered);
        self.page[offset + 4..offset + 8].copy_from_slice(&crc.to_le_bytes());
        self.mark_written(index);
    }

    fn ensure(&self, span: usize) -> Result<(), String> {
        if self.next + span > NVS_ITEMS {
            Err("provisioning values do not fit in one NVS page".into())
        } else {
            Ok(())
        }
    }

    fn namespace(&mut self, name: &str) -> Result<(), String> {
        self.ensure(1)?;
        let index = self.next;
        self.header(index, 0, 0x01, 1, name);
        self.page[Self::item_offset(index) + 24] = 1;
        self.finish_item(index);
        self.next += 1;
        Ok(())
    }

    fn string(&mut self, key: &str, value: &str) -> Result<(), String> {
        if key.len() > 15 {
            return Err(format!("NVS key {key} is too long"));
        }
        let mut payload = value.as_bytes().to_vec();
        payload.push(0);
        let payload_entries = payload.len().div_ceil(32);
        let span = 1 + payload_entries;
        self.ensure(span)?;
        let index = self.next;
        self.header(index, 1, 0x21, span as u8, key);
        let offset = Self::item_offset(index);
        self.page[offset + 24..offset + 26].copy_from_slice(&(payload.len() as u16).to_le_bytes());
        let payload_crc = crc32_esp_rom(&payload);
        self.page[offset + 28..offset + 32].copy_from_slice(&payload_crc.to_le_bytes());
        self.page[offset + 32..offset + 32 + payload.len()].copy_from_slice(&payload);
        self.finish_item(index);
        for payload_index in 1..span {
            self.mark_written(index + payload_index);
        }
        self.next += span;
        Ok(())
    }

    fn u8(&mut self, key: &str, value: u8) -> Result<(), String> {
        self.ensure(1)?;
        let index = self.next;
        self.header(index, 1, 0x01, 1, key);
        let offset = Self::item_offset(index);
        self.page[offset + 24] = value;
        self.finish_item(index);
        self.next += 1;
        Ok(())
    }

    // ESP-IDF v2 blob (Preferences putBytes): one BLOB_DATA chunk (0x42)
    // carrying the payload plus one BLOB_IDX (0x48) with the total size —
    // byte-identical to the browser flasher's writeBlob (flash-core.js).
    fn blob(&mut self, key: &str, bytes: &[u8]) -> Result<(), String> {
        let payload_entries = bytes.len().div_ceil(32);
        self.ensure(1 + payload_entries + 1)?;
        let index = self.next;
        self.header(index, 1, 0x42, (1 + payload_entries) as u8, key);
        let offset = Self::item_offset(index);
        // chunk index byte (header() writes 0xff): VER_0 chunk 0
        self.page[offset + 3] = 0;
        self.page[offset + 24..offset + 26].copy_from_slice(&(bytes.len() as u16).to_le_bytes());
        self.page[offset + 28..offset + 32].copy_from_slice(&crc32_esp_rom(bytes).to_le_bytes());
        self.page[offset + 32..offset + 32 + bytes.len()].copy_from_slice(bytes);
        self.finish_item(index);
        for payload_index in 1..=payload_entries {
            self.mark_written(index + payload_index);
        }
        self.next += 1 + payload_entries;

        let index = self.next;
        self.header(index, 1, 0x48, 1, key);
        let offset = Self::item_offset(index);
        self.page[offset + 24..offset + 28].copy_from_slice(&(bytes.len() as u32).to_le_bytes());
        self.page[offset + 28] = 1; // chunkCount
        self.page[offset + 29] = 0; // chunkStart (VER_0)
        self.finish_item(index);
        self.next += 1;
        Ok(())
    }

    fn u16(&mut self, key: &str, value: u16) -> Result<(), String> {
        self.ensure(1)?;
        let index = self.next;
        self.header(index, 1, 0x02, 1, key);
        let offset = Self::item_offset(index);
        self.page[offset + 24..offset + 26].copy_from_slice(&value.to_le_bytes());
        self.finish_item(index);
        self.next += 1;
        Ok(())
    }

    fn finish(mut self, partition_size: usize) -> Vec<u8> {
        self.page[0..4].copy_from_slice(&0xffff_fffeu32.to_le_bytes());
        self.page[4..8].copy_from_slice(&0u32.to_le_bytes());
        self.page[8] = 0xfe;
        let header_crc = crc32_esp_rom(&self.page[4..28]);
        self.page[28..32].copy_from_slice(&header_crc.to_le_bytes());
        let mut image = vec![0xff; partition_size];
        image[..NVS_PAGE].copy_from_slice(&self.page);
        image
    }
}

fn build_nvs(config: &Provisioning, partition_size: usize) -> Result<Vec<u8>, String> {
    validate(config)?;
    let mut writer = NvsWriter::new();
    writer.namespace("securacv")?;
    if !config.wifi_ssid.is_empty() {
        if config.wifi_nvs == "blob" {
            // canary/wap read Wi-Fi with getBytes and gate on a wifi_en bool —
            // a string under the same key would read back empty, and vice versa.
            writer.blob("wifi_ssid", config.wifi_ssid.as_bytes())?;
            writer.blob("wifi_pass", config.wifi_pass.as_bytes())?;
            writer.u8("wifi_en", 1)?;
            // These firmwares also latch a first-boot flag: without it a fully
            // seeded board still boots shouting "SETUP MODE" and raising its
            // captive portal even though the STA join succeeds. Seeded Wi-Fi IS
            // the setup, so mark it done (Preferences putBool stores a u8).
            writer.u8("setup_ok", 1)?;
        } else {
            writer.string("wifi_ssid", &config.wifi_ssid)?;
            writer.string("wifi_pass", &config.wifi_pass)?;
        }
    }
    // Per-capability writes, and empty is SKIPPED (absent key), never written:
    // the firmware loader treats a PRESENT key as the answer — even an empty
    // one — so writing "" here would erase a value the user set on-device.
    // This mirrors the browser's mqttProvisioningToNvs byte-for-byte.
    if !config.device_id.is_empty() {
        writer.string("dev_id", &config.device_id)?;
    }
    if !config.mqtt_host.is_empty() {
        writer.string("mqtt_host", &config.mqtt_host)?;
        writer.u16("mqtt_port", config.mqtt_port)?;
        if !config.mqtt_user.is_empty() {
            writer.string("mqtt_user", &config.mqtt_user)?;
        }
        if !config.mqtt_pass.is_empty() {
            writer.string("mqtt_pass", &config.mqtt_pass)?;
        }
    }
    if !config.api_token.is_empty() {
        // Both loaders read the token with Preferences getBytes — a BLOB, not
        // a string — under their own key: "api_token" (canary_config.h
        // NVS_KEY_TOKEN, the PIO canary) and "api_tkn" (canary_wap.ino
        // NVS_KEY_API_TKN). Seed both; each firmware reads its own and the
        // other 40-byte entry sits unread in the same namespace.
        writer.blob("api_token", config.api_token.as_bytes())?;
        writer.blob("api_tkn", config.api_token.as_bytes())?;
    }
    Ok(writer.finish(partition_size))
}

pub fn patch_factory_image(image: &mut Vec<u8>, config: &Provisioning) -> Result<(), String> {
    let partition = find_nvs_partition(image)?;
    let nvs = build_nvs(config, partition.size)?;
    let end = partition.offset + partition.size;
    if image.len() < end {
        image.resize(end, 0xff);
    }
    image[partition.offset..end].copy_from_slice(&nvs);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config() -> Provisioning {
        Provisioning {
            device_id: "canary_vision_a1b2".into(),
            wifi_ssid: "Bird House".into(),
            wifi_pass: "correct horse".into(),
            mqtt_host: "homeassistant.local".into(),
            mqtt_port: 1883,
            mqtt_user: "canary".into(),
            mqtt_pass: "broker-secret".into(),
            wifi_nvs: String::new(),
            api_token: String::new(),
        }
    }

    // A well-formed bearer credential for the token tests below.
    const TOKEN: &str = "cv_0123456789abcdefghijKLMNOPqrstuv";

    // The blob scheme (canary/wap firmwares: getBytes + wifi_en) — the seed
    // must carry BLOB_DATA + BLOB_IDX entries and the enable bool, and a
    // wifi-only config must skip every identity/broker key.
    #[test]
    fn blob_scheme_wifi_only_seed() {
        let cfg = Provisioning {
            device_id: String::new(),
            mqtt_host: String::new(),
            mqtt_user: String::new(),
            mqtt_pass: String::new(),
            wifi_nvs: "blob".into(),
            ..config()
        };
        let image = build_nvs(&cfg, 0x4000).expect("wifi-only blob seed builds");
        let page = &image[..4096];
        let has_item = |kind: u8, key: &str| {
            (0..126).any(|i| {
                let o = 64 + i * 32;
                page[o + 1] == kind
                    && &page[o + 8..o + 8 + key.len()] == key.as_bytes()
                    && matches!(page[o + 8 + key.len()], 0 | 0xff)
            })
        };
        assert!(has_item(0x42, "wifi_ssid") && has_item(0x48, "wifi_ssid"));
        assert!(has_item(0x42, "wifi_pass") && has_item(0x48, "wifi_pass"));
        assert!(has_item(0x01, "wifi_en"));
        // Seeded Wi-Fi IS the setup: the first-boot latch must be marked done,
        // or the board boots into SETUP MODE with its portal despite joining.
        assert!(has_item(0x01, "setup_ok"));
        assert!(
            !has_item(0x21, "wifi_ssid"),
            "no string twin beside the blob"
        );
        assert!(!has_item(0x21, "dev_id") && !has_item(0x21, "mqtt_host"));
    }

    // The display case (PR #1351 finished): on-glass boards have NO device id
    // — the UI hides the field — yet they read a broker from NVS. Wi-Fi +
    // broker with an empty identity must seed, not abort the flash.
    #[test]
    fn string_scheme_broker_without_identity_seeds() {
        let cfg = Provisioning {
            device_id: String::new(),
            mqtt_host: "homeassistant.local".into(),
            mqtt_user: String::new(),
            mqtt_pass: String::new(),
            ..config()
        };
        let image = build_nvs(&cfg, 0x4000).expect("display Wi-Fi + broker seed builds");
        let page = &image[..4096];
        let has_key = |key: &str| {
            (0..126).any(|i| {
                let o = 64 + i * 32;
                &page[o + 8..o + 8 + key.len()] == key.as_bytes()
                    && matches!(page[o + 8 + key.len()], 0 | 0xff)
            })
        };
        assert!(has_key("wifi_ssid") && has_key("wifi_pass"));
        assert!(has_key("mqtt_host") && has_key("mqtt_port"));
        // Empty means ABSENT — never write "" over a value set on-device.
        assert!(!has_key("dev_id"));
        assert!(!has_key("mqtt_user") && !has_key("mqtt_pass"));
    }

    // Broker-only: a display kept on its on-glass Wi-Fi setup can still be
    // told its hub (and name) here — an empty network is "skip Wi-Fi", never
    // an abort. No wifi keys, no wifi_en, no setup_ok latch.
    #[test]
    fn broker_only_without_wifi_seeds() {
        let cfg = Provisioning {
            wifi_ssid: String::new(),
            wifi_pass: String::new(),
            device_id: "canary_display_ab12".into(),
            mqtt_host: "homeassistant.local".into(),
            mqtt_user: String::new(),
            mqtt_pass: String::new(),
            ..config()
        };
        let image = build_nvs(&cfg, 0x4000).expect("broker-only seed builds");
        let page = &image[..4096];
        let has_key = |key: &str| {
            (0..126).any(|i| {
                let o = 64 + i * 32;
                &page[o + 8..o + 8 + key.len()] == key.as_bytes()
                    && matches!(page[o + 8 + key.len()], 0 | 0xff)
            })
        };
        assert!(has_key("dev_id") && has_key("mqtt_host") && has_key("mqtt_port"));
        assert!(!has_key("wifi_ssid") && !has_key("wifi_pass"));
        assert!(!has_key("wifi_en") && !has_key("setup_ok"));
    }

    // The one nonsensical shape: a password with no network to use it on.
    #[test]
    fn password_without_network_is_refused() {
        let cfg = Provisioning {
            wifi_ssid: String::new(),
            wifi_pass: "correct horse".into(),
            device_id: String::new(),
            mqtt_host: String::new(),
            mqtt_user: String::new(),
            mqtt_pass: String::new(),
            ..config()
        };
        assert!(build_nvs(&cfg, 0x4000).is_err());
    }

    // An open network's empty password must still be SEEDED (present-but-empty
    // key): the firmware loader treats a present key as the answer, and an
    // absent one falls back to the compiled ci-placeholder — which is exactly
    // the wrong password to try on an open AP.
    #[test]
    fn open_network_empty_password_key_is_present() {
        let cfg = Provisioning {
            wifi_pass: String::new(),
            device_id: String::new(),
            mqtt_host: String::new(),
            mqtt_user: String::new(),
            mqtt_pass: String::new(),
            ..config()
        };
        let image = build_nvs(&cfg, 0x4000).expect("open-network seed builds");
        let page = &image[..4096];
        let has_key = |key: &str| {
            (0..126).any(|i| {
                let o = 64 + i * 32;
                &page[o + 8..o + 8 + key.len()] == key.as_bytes()
                    && matches!(page[o + 8 + key.len()], 0 | 0xff)
            })
        };
        assert!(
            has_key("wifi_pass"),
            "empty wifi_pass must still write its key"
        );
    }

    // Wi-Fi-only with the default string scheme: network keys only.
    #[test]
    fn string_scheme_wifi_only_skips_identity() {
        let cfg = Provisioning {
            device_id: String::new(),
            mqtt_host: String::new(),
            mqtt_user: String::new(),
            mqtt_pass: String::new(),
            ..config()
        };
        let image = build_nvs(&cfg, 0x4000).expect("wifi-only string seed builds");
        let page = &image[..4096];
        let has_key = |key: &str| {
            (0..126).any(|i| {
                let o = 64 + i * 32;
                &page[o + 8..o + 8 + key.len()] == key.as_bytes()
                    && matches!(page[o + 8 + key.len()], 0 | 0xff)
            })
        };
        assert!(has_key("wifi_ssid") && has_key("wifi_pass"));
        assert!(!has_key("dev_id") && !has_key("mqtt_host") && !has_key("mqtt_port"));
    }

    // The API token is seeded as a BLOB under BOTH firmware key names — the
    // PIO canary reads "api_token", the wap reads "api_tkn", and each uses
    // Preferences getBytes, so a string twin would read back empty.
    #[test]
    fn api_token_seeds_both_variant_keys_as_blobs() {
        let cfg = Provisioning {
            api_token: TOKEN.into(),
            ..config()
        };
        let image = build_nvs(&cfg, 0x4000).expect("token seed builds");
        let page = &image[..4096];
        let has_item = |kind: u8, key: &str| {
            (0..126).any(|i| {
                let o = 64 + i * 32;
                page[o + 1] == kind
                    && &page[o + 8..o + 8 + key.len()] == key.as_bytes()
                    && matches!(page[o + 8 + key.len()], 0 | 0xff)
            })
        };
        assert!(has_item(0x42, "api_token") && has_item(0x48, "api_token"));
        assert!(has_item(0x42, "api_tkn") && has_item(0x48, "api_tkn"));
        assert!(
            !has_item(0x21, "api_token"),
            "no string twin beside the blob"
        );
        // The credential bytes themselves are in the page, prefix intact.
        assert!(page.windows(TOKEN.len()).any(|w| w == TOKEN.as_bytes()));
    }

    // Empty means absent — an unseeded board keeps deriving its own token
    // from its device key, exactly as before this field existed.
    #[test]
    fn no_api_token_writes_no_token_keys() {
        let image = build_nvs(&config(), 0x4000).expect("tokenless seed builds");
        let page = &image[..4096];
        let has_key = |key: &str| {
            (0..126).any(|i| {
                let o = 64 + i * 32;
                &page[o + 8..o + 8 + key.len()] == key.as_bytes()
                    && matches!(page[o + 8 + key.len()], 0 | 0xff)
            })
        };
        assert!(!has_key("api_token") && !has_key("api_tkn"));
    }

    // A malformed token is refused before the patch — seeding a credential
    // the firmware's loader would reject leaves the flasher holding a copy
    // of a token the device never adopts.
    #[test]
    fn malformed_api_token_is_refused() {
        for bad in [
            "cv_short",
            "xx_0123456789abcdefghijKLMNOPqrstuv",
            "cv_0123456789abcdefghijKLMNOPqrst!v",
            "cv_0123456789abcdefghijKLMNOPqrstuvw",
        ] {
            let cfg = Provisioning {
                api_token: bad.into(),
                ..config()
            };
            assert!(build_nvs(&cfg, 0x4000).is_err(), "accepted {bad:?}");
        }
        assert!(api_token_shape_ok(TOKEN));
    }

    fn factory() -> Vec<u8> {
        let mut image = vec![0xff; 0x20000];
        let table = PARTITION_TABLE_OFFSET;
        image[table..table + 2].copy_from_slice(&PARTITION_MAGIC.to_le_bytes());
        image[table + 2] = 0x01;
        image[table + 3] = 0x02;
        image[table + 4..table + 8].copy_from_slice(&0x9000u32.to_le_bytes());
        image[table + 8..table + 12].copy_from_slice(&0x5000u32.to_le_bytes());
        image[table + 12..table + 15].copy_from_slice(b"nvs");
        image
    }

    #[test]
    fn locates_and_patches_image_declared_nvs_partition() {
        let mut image = factory();
        patch_factory_image(&mut image, &config()).unwrap();
        assert_eq!(&image[0x9000..0x9004], &0xffff_fffeu32.to_le_bytes());
        assert_eq!(image[0x9008], 0xfe);
        assert!(image[0x9000..0xe000]
            .windows("wifi_ssid".len())
            .any(|w| w == b"wifi_ssid"));
        assert!(image[0x9000..0xe000]
            .windows("homeassistant.local".len())
            .any(|w| w == b"homeassistant.local"));
    }

    #[test]
    fn string_entries_use_preferences_compatible_nvs_type() {
        let nvs = build_nvs(&config(), 0x5000).unwrap();
        let wifi_header = nvs
            .windows("wifi_ssid".len())
            .position(|w| w == b"wifi_ssid")
            .unwrap()
            - 8;
        assert_eq!(nvs[wifi_header + 1], 0x21);
        assert!(nvs[wifi_header + 2] >= 2);
    }

    #[test]
    fn invalid_secrets_are_refused_before_patch() {
        let mut bad = config();
        bad.wifi_pass = "short".into();
        assert!(patch_factory_image(&mut factory(), &bad).is_err());
    }
}
