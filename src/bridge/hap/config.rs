//! The bridge's configuration file — what the setup wizard writes down so the
//! answers only have to be given once.
//!
//! # Why a file and not just flags
//!
//! The flags are fine for one Canary and a demo. A real household has three
//! devices with names, a broker with credentials, a pacing choice and a
//! consent decision about class-scoped signals — and all of it has to survive
//! a reboot, a service restart, and the user forgetting what they typed. The
//! wizard's job is to ask once and write this; everything after is
//! `hap_bridge` with no arguments.
//!
//! # Canary order is load-bearing, and the file says so
//!
//! Accessory IDs are assigned by position, and controllers cache them. Move a
//! Canary up the list after pairing and the Home app starts reading the wrong
//! device — the porch tile showing the garage. Nothing can detect that from
//! the outside, so [`BridgeConfig::validate`] refuses duplicate IDs (which
//! would make the mapping ambiguous) and the written file carries a comment
//! saying to append rather than reorder.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::bridge::homekit::{HomeSignal, PacingConfig};

/// Where the config lives unless told otherwise.
pub const DEFAULT_PATH: &str = "hap.toml";

/// One Canary, as the user named it.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct CanaryConfig {
    /// The MQTT device id it publishes under (`securacv/<id>/…`).
    pub id: String,
    /// The name shown in the Home app.
    pub name: String,
}

/// How to reach the broker carrying the fleet's events.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct MqttConfig {
    pub host: String,
    pub port: u16,
    /// Topic prefix the fleet publishes under.
    pub prefix: String,
    /// Broker username, if it wants one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub username: Option<String>,
    /// Broker password.
    ///
    /// Stored in the same file as the accessory seed, which is already
    /// `0600` and already refuses to load if anyone else can read it — so
    /// this adds no new exposure. It is still the one field worth leaving
    /// out and supplying by environment where a household has the option.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub password: Option<String>,
}

impl Default for MqttConfig {
    fn default() -> Self {
        MqttConfig {
            host: "127.0.0.1".into(),
            port: 1883,
            prefix: "securacv".into(),
            username: None,
            password: None,
        }
    }
}

/// Everything the bridge needs to run unattended.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct BridgeConfig {
    /// The bridge accessory's name in the Home app.
    pub bridge_name: String,
    /// Where the identity, setup code and pairings live.
    pub state: PathBuf,
    /// Listen address.
    pub bind: String,
    /// Milliseconds between publications — the privacy/latency dial.
    pub tick_ms: u32,
    /// Class-scoped signals the operator explicitly turned on, by dictionary
    /// id. Empty is the dumb-PIR bar, and the default.
    #[serde(default)]
    pub enable_class: Vec<String>,
    #[serde(default)]
    pub mqtt: MqttConfig,
    /// The fleet, **in accessory-id order**. Append; never reorder.
    #[serde(default, rename = "canary")]
    pub canaries: Vec<CanaryConfig>,
}

impl Default for BridgeConfig {
    fn default() -> Self {
        BridgeConfig {
            bridge_name: "SecuraCV".into(),
            state: PathBuf::from("hap_state.json"),
            bind: "0.0.0.0:51826".into(),
            tick_ms: PacingConfig::default().tick_ms,
            enable_class: Vec::new(),
            mqtt: MqttConfig::default(),
            canaries: Vec::new(),
        }
    }
}

/// What is wrong with a configuration.
#[derive(Clone, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum ConfigError {
    /// No Canaries listed — the bridge would advertise an empty home.
    NoCanaries,
    /// Two Canaries share an MQTT id, so events cannot be attributed.
    DuplicateId(String),
    /// A Canary has an empty id or name.
    EmptyField,
    /// `tick_ms` outside the pacer's accepted range.
    TickOutOfRange(u32),
    /// A name in `enable_class` is not a class-scoped signal.
    UnknownClassSignal(String),
    /// `bind` is not a socket address.
    BadBindAddress(String),
}

impl std::fmt::Display for ConfigError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ConfigError::NoCanaries => write!(
                f,
                "no Canaries configured — run `hap_bridge setup` to add one"
            ),
            ConfigError::DuplicateId(id) => write!(
                f,
                "two Canaries share the MQTT id {id:?}; ids must be unique or \
                 their events cannot be told apart"
            ),
            ConfigError::EmptyField => write!(f, "a Canary has an empty id or name"),
            ConfigError::TickOutOfRange(ms) => write!(
                f,
                "tick_ms {ms} is outside {}..={} — pacing is a privacy \
                 parameter, so it is refused rather than clamped",
                PacingConfig::MIN_TICK_MS,
                PacingConfig::MAX_TICK_MS
            ),
            ConfigError::UnknownClassSignal(s) => {
                write!(f, "{s:?} is not a class-scoped signal")
            }
            ConfigError::BadBindAddress(a) => write!(f, "bind address {a:?} is not valid"),
        }
    }
}

impl std::error::Error for ConfigError {}

impl BridgeConfig {
    /// Check the configuration says something the bridge can actually do.
    ///
    /// Every one of these is a failure that would otherwise be silent or
    /// baffling at runtime — an empty home, two Canaries fighting over one
    /// topic, a pacing value quietly clamped, a consent list naming a signal
    /// that does not exist.
    pub fn validate(&self) -> Result<(), ConfigError> {
        if self.canaries.is_empty() {
            return Err(ConfigError::NoCanaries);
        }
        let mut seen = std::collections::BTreeSet::new();
        for c in &self.canaries {
            if c.id.trim().is_empty() || c.name.trim().is_empty() {
                return Err(ConfigError::EmptyField);
            }
            if !seen.insert(&c.id) {
                return Err(ConfigError::DuplicateId(c.id.clone()));
            }
        }
        if !(PacingConfig::MIN_TICK_MS..=PacingConfig::MAX_TICK_MS).contains(&self.tick_ms) {
            return Err(ConfigError::TickOutOfRange(self.tick_ms));
        }
        for name in &self.enable_class {
            if class_signal(name).is_none() {
                return Err(ConfigError::UnknownClassSignal(name.clone()));
            }
        }
        if self.bind.parse::<std::net::SocketAddr>().is_err() {
            return Err(ConfigError::BadBindAddress(self.bind.clone()));
        }
        Ok(())
    }

    /// The class-scoped signals this config turns on.
    pub fn class_signals(&self) -> Vec<HomeSignal> {
        self.enable_class
            .iter()
            .filter_map(|n| class_signal(n))
            .collect()
    }

    /// Serialize with a header explaining the one thing a user can break by
    /// hand-editing.
    pub fn to_toml(&self) -> Result<String, toml::ser::Error> {
        let body = toml::to_string_pretty(self)?;
        Ok(format!(
            "# SecuraCV → Apple Home. Written by `hap_bridge setup`.\n\
             #\n\
             # The order of [[canary]] entries fixes each device's HomeKit\n\
             # accessory id, and controllers cache those. APPEND new Canaries\n\
             # to the end; reordering them after pairing makes the Home app\n\
             # read the wrong device, and nothing will warn you.\n\
             #\n\
             # tick_ms is a privacy dial, not a performance knob: publication\n\
             # happens on that cadence whether or not anything happened, so it\n\
             # bounds how precisely an observer can place an event.\n\
             \n{body}"
        ))
    }
}

/// A stable digest of everything that shapes the `/accessories` document —
/// the fleet (ids and names, in order) and the class-signal consent. When it
/// moves between starts, the accessory database a controller cached is no
/// longer the truth, and the config number must bump so the Home app
/// re-reads `/accessories` instead of showing stale tiles. Order-sensitive
/// on purpose: accessory ids come from list position. Consent is in the
/// hash because iids stay stable across consent changes but the *document*
/// does not — enabling the class signals adds characteristics a cached
/// controller would otherwise never learn about.
pub fn fleet_shape_hash(canaries: &[CanaryConfig], enable_class: &[String]) -> String {
    use sha2::{Digest, Sha256};
    let mut hasher = Sha256::new();
    for name in enable_class {
        hasher.update(name.as_bytes());
        hasher.update([1u8]);
    }
    for c in canaries {
        hasher.update(c.id.as_bytes());
        hasher.update([0u8]);
        hasher.update(c.name.as_bytes());
        hasher.update(*b"\n");
    }
    hex::encode(hasher.finalize())
}

/// Merge a newly chosen fleet into an existing one **without moving anyone**.
///
/// Rerunning the wizard to add or rename a Canary must not renumber the ones
/// already paired. Accessory ids come from list position and controllers
/// cache them, so a re-sort would leave the Home app's "Porch" tile — and
/// every automation written against it — pointing at the garage. Nothing
/// downstream can detect that; it just silently starts lying.
///
/// So: existing entries keep their positions, a rename updates the name in
/// place, and genuinely new devices are appended.
///
/// Note what is deliberately *not* done — an existing Canary the user did not
/// re-select is **kept**, not dropped. Removing it would shift every device
/// after it up one, which is the same bug by another route. Deleting a Canary
/// is an edit to the file, made deliberately, with the consequence visible.
pub fn merge_fleet(existing: &[CanaryConfig], chosen: &[CanaryConfig]) -> Vec<CanaryConfig> {
    let mut merged: Vec<CanaryConfig> = existing.to_vec();
    for pick in chosen {
        match merged.iter_mut().find(|c| c.id == pick.id) {
            // Already known: keep the slot, take the (possibly new) name.
            Some(slot) => slot.name = pick.name.clone(),
            // New: the end of the list is the only safe place for it.
            None => merged.push(pick.clone()),
        }
    }
    merged
}

/// Resolve a class-scoped signal by its dictionary id.
///
/// Only class-scoped signals resolve. The rest of the vocabulary is not the
/// operator's to switch on from a config file — it is either at the dumb-PIR
/// bar already or, like tamper, deliberately not disableable.
pub fn class_signal(name: &str) -> Option<HomeSignal> {
    HomeSignal::ALL
        .into_iter()
        .find(|s| s.is_class_scoped() && s.as_str() == name)
}

/// Every class-scoped signal id, for help text and prompts.
pub fn class_signal_names() -> Vec<&'static str> {
    HomeSignal::ALL
        .into_iter()
        .filter(|s| s.is_class_scoped())
        .map(|s| s.as_str())
        .collect()
}

/// Read a config file.
///
/// Refuses one that group or other can read. This file can carry the broker
/// password, so it gets exactly the treatment the state file gets — and the
/// quickstart promises as much, which would be a lie if `save` set the mode
/// and `load` never checked it.
pub fn load(path: &Path) -> anyhow::Result<BridgeConfig> {
    super::store::refuse_if_group_or_world_readable(path, "your broker credentials")?;
    let text = std::fs::read_to_string(path)
        .map_err(|e| anyhow::anyhow!("could not read {}: {e}", path.display()))?;
    let cfg: BridgeConfig = toml::from_str(&text)
        .map_err(|e| anyhow::anyhow!("{} is not a valid config: {e}", path.display()))?;
    cfg.validate()?;
    Ok(cfg)
}

/// Write a config file, creating its directory if needed.
pub fn save(path: &Path, cfg: &BridgeConfig) -> anyhow::Result<()> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent)?;
        }
    }
    std::fs::write(path, cfg.to_toml()?)?;
    // The file can carry a broker password, so it gets the same treatment as
    // the state file rather than whatever the umask happened to be.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600))?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fleet_shape_hash_moves_on_every_kind_of_drift() {
        let canary = |id: &str, name: &str| CanaryConfig {
            id: id.into(),
            name: name.into(),
        };
        let base = vec![canary("porch-canary", "Porch Canary")];
        let same = vec![canary("porch-canary", "Porch Canary")];
        assert_eq!(
            fleet_shape_hash(&base, &[]),
            fleet_shape_hash(&same, &[]),
            "an unchanged fleet must not bump c# on every restart"
        );
        // Rename, add, and reorder each move the hash — each is a change a
        // paired controller must be told to re-read, or its tiles go stale
        // (accessory ids come from list position; names come from here).
        let renamed = vec![canary("porch-canary", "Front Door Canary")];
        assert_ne!(
            fleet_shape_hash(&base, &[]),
            fleet_shape_hash(&renamed, &[])
        );
        let grown = vec![
            canary("porch-canary", "Porch Canary"),
            canary("garage-canary", "Garage Canary"),
        ];
        assert_ne!(fleet_shape_hash(&base, &[]), fleet_shape_hash(&grown, &[]));
        let reordered = vec![
            canary("garage-canary", "Garage Canary"),
            canary("porch-canary", "Porch Canary"),
        ];
        assert_ne!(
            fleet_shape_hash(&grown, &[]),
            fleet_shape_hash(&reordered, &[])
        );
        // And the separator can't be confused by adversarial names.
        let tricky_a = vec![canary("a", "b\nc")];
        let tricky_b = vec![canary("a", "b"), canary("c", "")];
        assert_ne!(
            fleet_shape_hash(&tricky_a, &[]),
            fleet_shape_hash(&tricky_b, &[])
        );
        // Consent reshapes the /accessories document (the class-scoped
        // characteristics appear or vanish) even though iids hold still —
        // so it must move the hash too, or a cached controller never
        // learns the class signals were turned on.
        let class = vec!["motion_person".to_string()];
        assert_ne!(
            fleet_shape_hash(&base, &[]),
            fleet_shape_hash(&base, &class),
            "a consent-only change must bump c#"
        );
    }

    fn sample() -> BridgeConfig {
        BridgeConfig {
            canaries: vec![
                CanaryConfig {
                    id: "porch-canary".into(),
                    name: "Porch Canary".into(),
                },
                CanaryConfig {
                    id: "garage-canary".into(),
                    name: "Garage Canary".into(),
                },
            ],
            ..BridgeConfig::default()
        }
    }

    #[test]
    fn a_config_round_trips_through_toml() {
        let cfg = sample();
        let text = cfg.to_toml().expect("serializes");
        let back: BridgeConfig = toml::from_str(&text).expect("parses");
        assert_eq!(cfg, back);
    }

    /// Accessory ids come from position, so a round trip that reorders the
    /// fleet would silently repoint every tile in the Home app.
    #[test]
    fn canary_order_survives_a_round_trip() {
        let cfg = sample();
        let back: BridgeConfig = toml::from_str(&cfg.to_toml().expect("ser")).expect("de");
        let ids: Vec<&str> = back.canaries.iter().map(|c| c.id.as_str()).collect();
        assert_eq!(ids, vec!["porch-canary", "garage-canary"]);
    }

    #[test]
    fn the_written_file_warns_about_reordering() {
        let text = sample().to_toml().expect("ser");
        assert!(text.contains("APPEND"), "must warn about ordering");
        assert!(text.contains("privacy dial"), "must explain tick_ms");
    }

    #[test]
    fn a_valid_config_validates() {
        sample().validate().expect("valid");
    }

    #[test]
    fn an_empty_fleet_is_refused_with_the_fix_in_the_message() {
        let cfg = BridgeConfig::default();
        let err = cfg.validate().expect_err("must refuse");
        assert_eq!(err, ConfigError::NoCanaries);
        assert!(err.to_string().contains("hap_bridge setup"));
    }

    /// Two Canaries on one topic cannot be told apart, so the events would be
    /// attributed to whichever entry `find` reached first.
    #[test]
    fn duplicate_ids_are_refused() {
        let mut cfg = sample();
        cfg.canaries[1].id = "porch-canary".into();
        assert_eq!(
            cfg.validate(),
            Err(ConfigError::DuplicateId("porch-canary".into()))
        );
    }

    #[test]
    fn empty_fields_are_refused() {
        let mut cfg = sample();
        cfg.canaries[0].name = "   ".into();
        assert_eq!(cfg.validate(), Err(ConfigError::EmptyField));
    }

    /// Pacing is refused, not clamped — and the message says why, because a
    /// user who typed 50 deserves to know it is a privacy parameter.
    #[test]
    fn out_of_range_pacing_is_refused_and_explained() {
        let mut cfg = sample();
        cfg.tick_ms = 50;
        let err = cfg.validate().expect_err("must refuse");
        assert_eq!(err, ConfigError::TickOutOfRange(50));
        assert!(err.to_string().contains("refused rather than clamped"));

        cfg.tick_ms = 10_000_000;
        assert!(cfg.validate().is_err());
    }

    /// Only class-scoped signals may be enabled here. Naming any other signal
    /// is a typo or a misunderstanding, and either way should not be
    /// silently ignored.
    #[test]
    fn only_class_scoped_signals_can_be_enabled() {
        let mut cfg = sample();
        cfg.enable_class = vec!["motion_person".into()];
        cfg.validate().expect("class-scoped is fine");
        assert_eq!(cfg.class_signals(), vec![HomeSignal::MotionPerson]);

        for bad in ["motion", "tamper", "active", "nonsense"] {
            cfg.enable_class = vec![bad.into()];
            assert_eq!(
                cfg.validate(),
                Err(ConfigError::UnknownClassSignal(bad.into())),
                "{bad} must not be enable-able"
            );
        }
    }

    #[test]
    fn the_default_is_the_dumb_pir_bar() {
        assert!(
            BridgeConfig::default().enable_class.is_empty(),
            "class-scoped signals are off until a human says otherwise"
        );
    }

    #[test]
    fn a_bad_bind_address_is_refused() {
        let mut cfg = sample();
        cfg.bind = "not-an-address".into();
        assert!(matches!(
            cfg.validate(),
            Err(ConfigError::BadBindAddress(_))
        ));
    }

    #[test]
    fn class_signal_names_are_the_four_sanctioned_ones() {
        let names = class_signal_names();
        assert_eq!(names.len(), 4);
        for n in &names {
            assert!(n.starts_with("motion_"));
        }
        assert!(names.contains(&"motion_person"));
        assert!(names.contains(&"motion_package"));
    }

    /// The property the whole merge exists for: an already-paired Canary
    /// must never change position, because position is its accessory id.
    #[test]
    fn merging_never_moves_an_existing_canary() {
        let existing = sample().canaries;
        // The wizard rediscovers the fleet lexicographically — garage first.
        let chosen = vec![
            CanaryConfig {
                id: "garage-canary".into(),
                name: "Garage Canary".into(),
            },
            CanaryConfig {
                id: "porch-canary".into(),
                name: "Porch Canary".into(),
            },
        ];
        let merged = merge_fleet(&existing, &chosen);
        let ids: Vec<&str> = merged.iter().map(|c| c.id.as_str()).collect();
        assert_eq!(
            ids,
            vec!["porch-canary", "garage-canary"],
            "rediscovery must not re-sort a paired fleet"
        );
    }

    #[test]
    fn merging_appends_new_canaries_at_the_end() {
        let existing = sample().canaries;
        let chosen = vec![CanaryConfig {
            id: "shed-canary".into(),
            name: "Shed Canary".into(),
        }];
        let merged = merge_fleet(&existing, &chosen);
        let ids: Vec<&str> = merged.iter().map(|c| c.id.as_str()).collect();
        assert_eq!(ids, vec!["porch-canary", "garage-canary", "shed-canary"]);
    }

    #[test]
    fn merging_renames_in_place() {
        let existing = sample().canaries;
        let chosen = vec![CanaryConfig {
            id: "porch-canary".into(),
            name: "Front Door".into(),
        }];
        let merged = merge_fleet(&existing, &chosen);
        assert_eq!(merged[0].id, "porch-canary");
        assert_eq!(merged[0].name, "Front Door", "a rename keeps the slot");
        assert_eq!(merged.len(), 2, "renaming must not drop the other device");
    }

    /// Dropping an unselected device would shift everyone after it up one —
    /// the same silent repointing by another route.
    #[test]
    fn merging_keeps_a_canary_the_user_did_not_reselect() {
        let existing = sample().canaries;
        let chosen = vec![CanaryConfig {
            id: "porch-canary".into(),
            name: "Porch Canary".into(),
        }];
        let merged = merge_fleet(&existing, &chosen);
        assert_eq!(merged.len(), 2);
        assert_eq!(merged[1].id, "garage-canary");
    }

    #[test]
    fn merging_into_nothing_is_just_the_choice() {
        let chosen = sample().canaries;
        assert_eq!(merge_fleet(&[], &chosen), chosen);
    }

    #[test]
    fn save_and_load_round_trip_on_disk() {
        let dir = tempfile::tempdir().expect("tempdir");
        let path = dir.path().join("hap.toml");
        let cfg = sample();
        save(&path, &cfg).expect("saves");
        assert_eq!(load(&path).expect("loads"), cfg);
    }

    #[cfg(unix)]
    #[test]
    fn the_config_is_written_private() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tempfile::tempdir().expect("tempdir");
        let path = dir.path().join("hap.toml");
        save(&path, &sample()).expect("saves");
        let mode = std::fs::metadata(&path).expect("stat").permissions().mode();
        assert_eq!(mode & 0o777, 0o600, "it can hold a broker password");
    }

    /// The config can hold a broker password, so a world-readable one is
    /// refused exactly like the state file — and the quickstart says so,
    /// which would be a false promise if only `save` enforced it.
    #[cfg(unix)]
    #[test]
    fn a_world_readable_config_is_refused() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tempfile::tempdir().expect("tempdir");
        let path = dir.path().join("hap.toml");
        save(&path, &sample()).expect("saves");
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o644)).expect("chmod");

        let err = load(&path).expect_err("must refuse");
        let msg = err.to_string();
        assert!(msg.contains("644"), "got: {msg}");
        assert!(msg.contains("chmod 600"), "must say how to fix it: {msg}");
        assert!(
            msg.contains("broker credentials"),
            "must say what leaked: {msg}"
        );
    }

    /// Loading validates, so a hand-edited file that is subtly wrong fails at
    /// startup with a clear message rather than at 3am with none.
    #[test]
    fn load_rejects_an_invalid_file() {
        let dir = tempfile::tempdir().expect("tempdir");
        let path = dir.path().join("hap.toml");
        let mut cfg = sample();
        cfg.tick_ms = 1;
        std::fs::write(&path, cfg.to_toml().expect("ser")).expect("write");
        assert!(load(&path).is_err());
    }
}
