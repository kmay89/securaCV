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
}

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
    if config.device_id.is_empty() || byte_len(&config.device_id) > 47 {
        return Err("device name must be 1–47 UTF-8 bytes".into());
    }
    if config.wifi_ssid.is_empty() || byte_len(&config.wifi_ssid) > 32 {
        return Err("Wi-Fi name must be 1–32 UTF-8 bytes".into());
    }
    if !config.wifi_pass.is_empty() && !(8..=63).contains(&byte_len(&config.wifi_pass)) {
        return Err("Wi-Fi password must be 8–63 bytes (or empty for an open network)".into());
    }
    if config.mqtt_host.is_empty() || byte_len(&config.mqtt_host) > 63 {
        return Err("MQTT host must be 1–63 UTF-8 bytes".into());
    }
    if byte_len(&config.mqtt_user) > 32 || byte_len(&config.mqtt_pass) > 64 {
        return Err("MQTT username or password is too long for the firmware".into());
    }
    if config.mqtt_port == 0 {
        return Err("MQTT port must be between 1 and 65535".into());
    }
    Ok(())
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
    writer.string("dev_id", &config.device_id)?;
    writer.string("wifi_ssid", &config.wifi_ssid)?;
    writer.string("wifi_pass", &config.wifi_pass)?;
    writer.string("mqtt_host", &config.mqtt_host)?;
    writer.u16("mqtt_port", config.mqtt_port)?;
    writer.string("mqtt_user", &config.mqtt_user)?;
    writer.string("mqtt_pass", &config.mqtt_pass)?;
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
        }
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
