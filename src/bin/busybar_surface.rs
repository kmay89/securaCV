//! The BUSY Bar surface daemon — witness state on someone else's glass.
//!
//! Design of record: `docs/design/busybar_surface.md`. The decision layer is
//! `witness_kernel::surface::busybar` and it is pure; this binary is the only
//! thing here that owns a socket or a clock.
//!
//! What it does, once per breath step:
//!
//! 1. Drain the broker, feeding every message to `fleet_peers::PeerTable`
//!    (which does the TOFU pinning and the Ed25519 chain check) and to the
//!    card ingest.
//! 2. Refresh the timeline from the kernel's signed export bundle, on a much
//!    slower cadence.
//! 3. Resolve both displays and `POST /api/display/draw`.
//!
//! # This process cannot write to anything it watches
//!
//! Constraint 1 of the design — no control may affect witnessing — is not a
//! rule this file follows, it is a shape this file has:
//!
//! - The broker handle is [`SubscribeOnly`], whose inner client is private
//!   and which exposes `subscribe` and nothing else. There is no publish
//!   call to review, and no reviewer has to check whether one crept in.
//! - The kernel is reached with `GET` only ([`kernel_get`]). The Event API's
//!   one mutating-looking route, `POST /verify`, is deliberately not called:
//!   it does not alter the log, but it does make the kernel do work, and a
//!   dial detent must not be able to schedule kernel work.
//! - The bar is written to, because the bar is the glass. That is the whole
//!   permitted direction of travel.

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::mpsc::{channel, Receiver, TryRecvError};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, Context, Result};
use clap::Parser;
use rumqttc::{Event, Incoming, MqttOptions, QoS};
use serde::Deserialize;

use witness_kernel::fleet_peers::PeerTable;
use witness_kernel::surface::busybar::card::{Badge, Card, ClassOptions};
use witness_kernel::surface::busybar::controls::{apply, ControlSource, MqttControlSource};
use witness_kernel::surface::busybar::device::{self, DrawOutcome, Frame, DRAW_PATH};
use witness_kernel::surface::busybar::ingest;
use witness_kernel::surface::busybar::phrase::PublicPhrase;
use witness_kernel::surface::busybar::state::{
    compose, resolve_front, resolve_rear, DeviceCards, SurfaceState, TimelineEntry, Timings,
    TransportPath, WitnessView, MAX_TRACKED_DEVICES,
};
use witness_kernel::surface::busybar::ZoneTable;
use witness_kernel::transport::{
    parse_mqtt_endpoint, validate_loopback_addr, TlsBackend, TlsConfig, TlsMaterials,
};

/// Topics the surface subscribes.
///
/// Wider than the alert relay's list, and the difference is deliberate. The
/// relay refuses `health` and `chain` because it has a **cloud sink**: those
/// topics carry stable fingerprints (`public_key`, `fp`) that must never
/// enter a path leaving the LAN. This surface has no cloud sink and cannot
/// acquire one — `device::validate_url` refuses the vendor proxy — so it may
/// read them, and it must: they are what makes a trust badge honest rather
/// than decorative. Neither fingerprint is ever drawn on either display.
const SUBSCRIBE_TOPICS: &[&str] = &[
    "securacv/+/availability",
    "securacv/+/status",
    "securacv/+/health",
    "securacv/+/chain",
    "securacv/+/state",
    "securacv/+/meta",
    "securacv/+/tamper",
    "securacv/+/sensing",
    "securacv/fleet/ack",
];

#[derive(Parser, Debug)]
#[command(
    author,
    version,
    about = "SecuraCV BUSY Bar surface: witness state on a BUSY Bar's two displays"
)]
struct Args {
    /// Path to the surface config (see busybar_surface.example.toml).
    #[arg(long, env = "BUSYBAR_SURFACE_CONFIG")]
    config: PathBuf,

    /// Resolve the config, print what would be drawn once, and exit without
    /// touching the network. The dry run a first-time operator should make
    /// before pointing this at a device.
    #[arg(long, default_value_t = false)]
    check: bool,
}

/// The surface config file.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct FileConfig {
    device: DeviceConfig,
    broker: BrokerConfig,
    #[serde(default)]
    kernel: Option<KernelConfig>,
    #[serde(default)]
    surface: SurfaceSection,
    /// `zone_id` to the label the front matrix may show for it.
    #[serde(default)]
    zones: BTreeMap<String, String>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct DeviceConfig {
    /// The bar's local base URL. `http://10.0.4.20` over USB.
    url: String,
    /// The bar's HTTP access PIN, if one is set. Sent as `X-API-Token`.
    #[serde(default)]
    token: Option<String>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct BrokerConfig {
    #[serde(default = "default_broker")]
    addr: String,
    #[serde(default)]
    allow_remote: bool,
    #[serde(default)]
    username: Option<String>,
    #[serde(default)]
    password: Option<String>,
    #[serde(default)]
    use_tls: bool,
    #[serde(default)]
    tls_ca_path: Option<PathBuf>,
    #[serde(default)]
    tls_client_cert_path: Option<PathBuf>,
    #[serde(default)]
    tls_client_key_path: Option<PathBuf>,
    #[serde(default = "default_client_id")]
    client_id: String,
    /// Prefix for surface commands, e.g. `securacv/surface/busybar/cmd`.
    /// Unset means the surface accepts no commands at all.
    #[serde(default)]
    command_prefix: Option<String>,
    /// Topic carrying the household's own microphone/camera-live boolean.
    /// Unset (the default) means the on-call indicator never shows.
    #[serde(default)]
    on_call_topic: Option<String>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct KernelConfig {
    /// The witness Event API base URL. Loopback by default.
    url: String,
    /// File holding the API capability token.
    #[serde(default)]
    token_path: Option<PathBuf>,
    /// Timeline window, as the API's `last` parameter accepts it.
    #[serde(default = "default_window")]
    timeline_window: String,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct SurfaceSection {
    /// Show P1 wellbeing numerics on the rear display. Never the front.
    #[serde(default)]
    rear_shows_wellbeing: bool,
    #[serde(default)]
    presence_dwell_ms: Option<u64>,
    #[serde(default)]
    presence_debounce_ms: Option<u64>,
    #[serde(default)]
    pulse_step_ms: Option<u64>,
    #[serde(default)]
    pulse_steps: Option<u8>,
    #[serde(default)]
    watchdog_steps: Option<u32>,
    #[serde(default)]
    broker_stale_ms: Option<u64>,
    #[serde(default)]
    quiet_window_ms: Option<u64>,
}

fn default_broker() -> String {
    "127.0.0.1:1883".to_string()
}

fn default_client_id() -> String {
    "securacv-busybar-surface".to_string()
}

fn default_window() -> String {
    "24h".to_string()
}

/// The broker handle, with the publish half removed.
///
/// The inner client is private and no method on this type sends anything. A
/// reviewer checking constraint 1 reads this struct rather than auditing
/// every call site, and a future contributor who needs to publish has to
/// widen this type on purpose — which is exactly the moment the design doc
/// wants someone to stop and think.
struct SubscribeOnly(rumqttc::Client);

impl SubscribeOnly {
    fn subscribe(&self, topic: &str, qos: QoS) -> Result<()> {
        self.0
            .subscribe(topic, qos)
            .map_err(|e| anyhow!("subscribe {topic} failed: {e}"))
    }
}

fn http_agent() -> ureq::Agent {
    // Bounded timeout: a hung bar must fail this redraw, not wedge the loop
    // (FR-4). The drawing on the glass expires on its own either way.
    ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_secs(10)))
        .build()
        .into()
}

fn drain(response: &mut ureq::http::Response<ureq::Body>) {
    let _ = response
        .body_mut()
        .with_config()
        .limit(64 * 1024)
        .read_to_vec();
}

/// One `POST /api/display/draw`.
fn draw(
    agent: &ureq::Agent,
    base: &str,
    token: Option<&str>,
    frame: &Frame,
) -> Result<DrawOutcome> {
    let endpoint = format!("{}{}", base.trim_end_matches('/'), DRAW_PATH);
    let mut req = agent
        .post(&endpoint)
        .header("Content-Type", "application/json");
    if let Some(t) = token {
        req = req.header("X-API-Token", t);
    }
    match req.send(frame.body().to_string().as_bytes()) {
        Ok(mut response) => {
            let status = response.status().as_u16();
            drain(&mut response);
            Ok(DrawOutcome::from_status(status))
        }
        Err(ureq::Error::StatusCode(code)) => Ok(DrawOutcome::from_status(code)),
        Err(e) => Err(anyhow!("draw failed: {e}")),
    }
}

/// `DELETE /api/display/draw`, scoped to this surface.
fn clear(agent: &ureq::Agent, base: &str, token: Option<&str>) -> Result<()> {
    let endpoint = format!(
        "{}{}{}",
        base.trim_end_matches('/'),
        DRAW_PATH,
        device::clear_query()
    );
    let mut req = agent.delete(&endpoint);
    if let Some(t) = token {
        req = req.header("X-API-Token", t);
    }
    match req.call() {
        Ok(mut r) => {
            drain(&mut r);
            Ok(())
        }
        Err(e) => Err(anyhow!("clear failed: {e}")),
    }
}

/// A read-only kernel fetch. `GET` is the only verb this binary uses against
/// the kernel; see the module docs.
fn kernel_get(agent: &ureq::Agent, url: &str, token: Option<&str>) -> Result<String> {
    let mut req = agent.get(url);
    if let Some(t) = token {
        req = req.header("x-witness-token", t);
    }
    let mut response = req.call().map_err(|e| anyhow!("kernel GET failed: {e}"))?;
    // Bounded read (FR-4): a very large export must fail this refresh, not
    // this process. The rear says the timeline is unavailable, which is the
    // honest outcome.
    response
        .body_mut()
        .with_config()
        .limit(4 * 1024 * 1024)
        .read_to_string()
        .map_err(|e| anyhow!("kernel body read failed: {e}"))
}

/// Per-device state the ingest accumulates between redraws.
#[derive(Default)]
struct DeviceScratch {
    state_cards: Vec<Card>,
    tamper: Option<Card>,
    acoustic: Option<Card>,
}

fn now_ms(start: Instant) -> u64 {
    start.elapsed().as_millis() as u64
}

fn now_epoch_s() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let args = Args::parse();

    let raw = std::fs::read_to_string(&args.config)
        .with_context(|| format!("reading {}", args.config.display()))?;
    let cfg: FileConfig =
        toml::from_str(&raw).with_context(|| format!("parsing {}", args.config.display()))?;

    // Local-first, enforced before anything opens a socket.
    device::validate_url(&cfg.device.url).map_err(|e| anyhow!(e))?;
    let transport = TransportPath::classify(&cfg.device.url);

    let zones = ZoneTable::build(cfg.zones.iter()).map_err(|e| anyhow!(e.to_string()))?;

    let defaults = Timings::default();
    let s = &cfg.surface;
    let timings = Timings {
        presence_dwell_ms: s.presence_dwell_ms.unwrap_or(defaults.presence_dwell_ms),
        presence_debounce_ms: s
            .presence_debounce_ms
            .unwrap_or(defaults.presence_debounce_ms),
        pulse_step_ms: s.pulse_step_ms.unwrap_or(defaults.pulse_step_ms).max(250),
        pulse_steps: s.pulse_steps.unwrap_or(defaults.pulse_steps).max(2),
        watchdog_steps: s.watchdog_steps.unwrap_or(defaults.watchdog_steps).max(2),
        broker_stale_ms: s.broker_stale_ms.unwrap_or(defaults.broker_stale_ms),
        quiet_window_ms: s.quiet_window_ms.unwrap_or(defaults.quiet_window_ms),
    };
    let class = ClassOptions {
        rear_shows_wellbeing: s.rear_shows_wellbeing,
    };

    let kernel_token = match cfg.kernel.as_ref().and_then(|k| k.token_path.as_ref()) {
        Some(p) => Some(
            std::fs::read_to_string(p)
                .with_context(|| format!("reading kernel token {}", p.display()))?
                .trim()
                .to_string(),
        ),
        None => None,
    };

    if args.check {
        println!("BUSY Bar surface config OK");
        println!("  device      {} ({})", cfg.device.url, transport.label());
        println!("  broker      {}", cfg.broker.addr);
        println!(
            "  kernel      {}",
            cfg.kernel.as_ref().map_or("(none)", |k| k.url.as_str())
        );
        println!("  zones       {} labeled", zones.len());
        println!(
            "  rear P1     {}",
            if class.rear_shows_wellbeing {
                "shown (opted in)"
            } else {
                "hidden"
            }
        );
        println!(
            "  breath      {} steps of {} ms; drawings expire after {} ms",
            timings.pulse_steps,
            timings.pulse_step_ms,
            timings.draw_timeout_ms()
        );
        return Ok(());
    }

    let endpoint = parse_mqtt_endpoint(&cfg.broker.addr, cfg.broker.use_tls)?;
    if !cfg.broker.allow_remote {
        validate_loopback_addr(&endpoint, &cfg.broker.addr)?;
    }
    let tls_config = TlsConfig {
        backend: TlsBackend::default(),
        materials: TlsMaterials::load(
            cfg.broker.tls_ca_path.as_ref(),
            cfg.broker.tls_client_cert_path.as_ref(),
            cfg.broker.tls_client_key_path.as_ref(),
        )?,
    };
    tls_config.backend.validate_feature_support()?;

    let (tx, rx) = channel::<(String, Vec<u8>, bool)>();
    let client_id = cfg.broker.client_id.clone();
    let username = cfg.broker.username.clone();
    let password = cfg.broker.password.clone();
    let command_prefix = cfg.broker.command_prefix.clone();
    let on_call_topic = cfg.broker.on_call_topic.clone();
    let ep = endpoint;
    let tls = tls_config;
    std::thread::spawn(move || loop {
        let mut options = MqttOptions::new(&client_id, (ep.host.as_str(), ep.port));
        options.set_keep_alive(60);
        options.set_clean_start(true);
        if let Some(user) = username.as_ref() {
            options.set_credentials(user.clone(), password.clone().unwrap_or_default());
        }
        match tls.build_transport(&ep) {
            Ok(t) => {
                options.set_transport(t);
            }
            Err(e) => {
                log::error!("TLS transport setup failed: {e}");
                std::thread::sleep(Duration::from_secs(10));
                continue;
            }
        }
        let (client, mut connection) = rumqttc::ClientBuilder::new(options).capacity(64).build();
        let client = SubscribeOnly(client);
        let mut ok = true;
        for topic in SUBSCRIBE_TOPICS {
            if let Err(e) = client.subscribe(topic, QoS::AtLeastOnce) {
                log::warn!("{e}");
                ok = false;
            }
        }
        if let Some(prefix) = command_prefix.as_ref() {
            let wildcard = format!("{}/#", prefix.trim_end_matches('/'));
            if let Err(e) = client.subscribe(&wildcard, QoS::AtLeastOnce) {
                log::warn!("{e}");
                ok = false;
            }
        }
        if let Some(topic) = on_call_topic.as_ref() {
            if let Err(e) = client.subscribe(topic, QoS::AtLeastOnce) {
                log::warn!("{e}");
                ok = false;
            }
        }
        if ok {
            for event in connection.iter() {
                match event {
                    Ok(Event::Incoming(Incoming::Publish(p))) => {
                        let topic = String::from_utf8_lossy(&p.topic).into_owned();
                        if tx.send((topic, p.payload.to_vec(), p.retain)).is_err() {
                            return;
                        }
                    }
                    Ok(_) => {}
                    Err(e) => {
                        log::warn!("MQTT connection error: {e}; reconnecting");
                        break;
                    }
                }
            }
        }
        std::thread::sleep(Duration::from_secs(3));
    });

    let agent = http_agent();
    let start = Instant::now();
    let mut peers = PeerTable::default();
    let mut scratch: BTreeMap<String, DeviceScratch> = BTreeMap::new();
    let mut surface = SurfaceState::new();
    let mut controls = MqttControlSource::new();
    let mut view = WitnessView::default();
    let mut step: u32 = 0;
    let mut last_broker_ms: Option<u64> = None;
    let mut last_timeline_ms: u64 = 0;
    let mut on_call = false;
    let timeline_refresh_ms: u64 = 60_000;

    log::info!(
        "BUSY Bar surface: {} via {}, {} zones labeled, drawings expire after {} ms",
        cfg.device.url,
        transport.label(),
        zones.len(),
        timings.draw_timeout_ms()
    );

    loop {
        let t = now_ms(start);
        drain_broker(
            &rx,
            &mut peers,
            &mut scratch,
            &mut surface,
            &mut controls,
            &mut on_call,
            &zones,
            &cfg,
            &timings,
            t,
        );
        if !scratch.is_empty() || !peers.is_empty() {
            last_broker_ms = Some(t);
        }

        // The timeline, from a signed export bundle, on its own slow cadence.
        if let Some(k) = cfg.kernel.as_ref() {
            if t.saturating_sub(last_timeline_ms) >= timeline_refresh_ms || last_timeline_ms == 0 {
                last_timeline_ms = t;
                match refresh_timeline(&agent, k, kernel_token.as_deref(), &zones) {
                    Ok(entries) => {
                        view.verified_recent = entries.len() as u32;
                        view.timeline = entries;
                        view.link.kernel_live = Some(true);
                    }
                    Err(e) => {
                        log::warn!("timeline refresh failed: {e}");
                        view.link.kernel_live = Some(false);
                    }
                }
            }
        }

        view.link.broker_live =
            last_broker_ms.is_some_and(|last| t.saturating_sub(last) <= timings.broker_stale_ms);
        view.on_call = on_call;
        view.devices = build_devices(&peers, &scratch, &zones, now_epoch_s());

        while let Some(control) = controls.poll() {
            apply(&mut surface, &view, control, &timings, t);
        }
        surface.tick(t, &timings);

        let front = resolve_front(&surface, &view, &class, t);
        let rear = resolve_rear(&surface, &view, transport, t, now_epoch_s());

        match compose(front.as_ref(), &rear, step, &timings) {
            Some(frame) => match draw(&agent, &cfg.device.url, cfg.device.token.as_deref(), &frame)
            {
                Ok(DrawOutcome::Drawn) => {}
                Ok(DrawOutcome::RefusedLowerPriority) => {
                    // Something with a higher priority owns the display. Our
                    // content is NOT up; say so rather than assume it is.
                    log::debug!("draw refused: a higher-priority app owns the display");
                }
                Ok(DrawOutcome::Forbidden) => {
                    log::warn!(
                        "the bar answered 403: set the HTTP access PIN in [device].token \
                         (USB and loopback bypass the check; Wi-Fi does not)"
                    );
                }
                Ok(DrawOutcome::Failed { status }) => log::warn!("draw returned {status}"),
                Err(e) => log::warn!("{e}"),
            },
            None => {
                // Nothing of ours belongs on the glass. Clear rather than
                // draw a blank: a cleared display is the bar's own, and an
                // expired one would flicker back at every redraw.
                if let Err(e) = clear(&agent, &cfg.device.url, cfg.device.token.as_deref()) {
                    log::debug!("{e}");
                }
            }
        }

        step = step.wrapping_add(1);
        std::thread::sleep(Duration::from_millis(timings.pulse_step_ms));
    }
}

#[allow(clippy::too_many_arguments)]
fn drain_broker(
    rx: &Receiver<(String, Vec<u8>, bool)>,
    peers: &mut PeerTable,
    scratch: &mut BTreeMap<String, DeviceScratch>,
    surface: &mut SurfaceState,
    controls: &mut MqttControlSource,
    on_call: &mut bool,
    zones: &ZoneTable,
    cfg: &FileConfig,
    timings: &Timings,
    t: u64,
) {
    loop {
        let (topic, payload, retained) = match rx.try_recv() {
            Ok(m) => m,
            Err(TryRecvError::Empty) => return,
            Err(TryRecvError::Disconnected) => return,
        };

        if let Some(prefix) = cfg.broker.command_prefix.as_ref() {
            let prefix = prefix.trim_end_matches('/');
            if let Some(suffix) = topic.strip_prefix(&format!("{prefix}/")) {
                controls.offer(suffix, &payload, timings);
                continue;
            }
        }
        if cfg.broker.on_call_topic.as_deref() == Some(topic.as_str()) {
            let raw = String::from_utf8_lossy(&payload);
            let raw = raw.trim();
            *on_call = matches!(
                raw.to_ascii_lowercase().as_str(),
                "on" | "true" | "1" | "online" | "active"
            );
            continue;
        }
        if topic == "securacv/fleet/ack" {
            // A household acknowledgement made somewhere else clears the
            // card here too. One-way: this surface never publishes it.
            surface.clear_presence();
            continue;
        }

        peers.observe(&topic, &payload, retained, now_epoch_s());

        let mut parts = topic.split('/');
        let (Some("securacv"), Some(device_id), Some(leaf), None) =
            (parts.next(), parts.next(), parts.next(), parts.next())
        else {
            continue;
        };
        if scratch.len() >= MAX_TRACKED_DEVICES && !scratch.contains_key(device_id) {
            continue;
        }
        let entry = scratch.entry(device_id.to_string()).or_default();
        match leaf {
            "state" => {
                let cards = ingest::cards_from_state(&payload);
                let became_present = cards.iter().any(|c| {
                    c.id == "presence"
                        && matches!(
                            c.value,
                            witness_kernel::surface::busybar::card::CardValue::Binary(Some(true))
                        )
                });
                let was_present = entry.state_cards.iter().any(|c| {
                    c.id == "presence"
                        && matches!(
                            c.value,
                            witness_kernel::surface::busybar::card::CardValue::Binary(Some(true))
                        )
                });
                entry.state_cards = cards;
                // A retained publish is the broker replaying what it already
                // held; it proves the state, not a transition, so it must not
                // light the matrix (the same replay reasoning `PeerTable`
                // applies to a retained chain publish).
                if became_present && !was_present && !retained {
                    let zone = zone_for(zones, device_id, &payload);
                    surface.note_presence(device_id, zone, t, timings);
                }
            }
            "tamper" => entry.tamper = ingest::tamper_card(&payload),
            "sensing" => entry.acoustic = ingest::acoustic_card(&payload),
            _ => {}
        }
    }
}

/// Resolve a device's front label.
///
/// The zone id comes from the device's own retained `state` publish when it
/// carries one, and otherwise from the device id. Either way the *word* comes
/// from the operator's table and never from the payload.
fn zone_for(
    zones: &ZoneTable,
    device_id: &str,
    payload: &[u8],
) -> Option<witness_kernel::surface::busybar::phrase::ZoneWord> {
    let zone_id = serde_json::from_slice::<serde_json::Value>(payload)
        .ok()
        .and_then(|d| {
            d.get("zone")
                .or_else(|| d.get("zone_id"))
                .and_then(|v| v.as_str())
                .map(str::to_string)
        });
    zones
        .label(zone_id.as_deref().unwrap_or(device_id))
        .or_else(|| zones.label(device_id))
        .cloned()
}

fn build_devices(
    peers: &PeerTable,
    scratch: &BTreeMap<String, DeviceScratch>,
    zones: &ZoneTable,
    now_epoch: u64,
) -> Vec<DeviceCards> {
    let summary = peers.summary(now_epoch);
    let mut out = Vec::new();
    for peer in summary.peers.iter().take(MAX_TRACKED_DEVICES) {
        let badge = ingest::badge_from_chain_word(&peer.chain, peer.proven());
        let mut cards: Vec<Card> = scratch
            .get(&peer.device_id)
            .map(|s| s.state_cards.clone())
            .unwrap_or_default();
        cards.push(ingest::chain_card(
            peer.last_verified_length.unwrap_or(0),
            badge,
        ));
        if let Some(s) = scratch.get(&peer.device_id) {
            if let Some(c) = s.tamper.clone() {
                cards.push(c);
            }
            if let Some(c) = s.acoustic.clone() {
                cards.push(c);
            }
        }
        out.push(DeviceCards {
            device_id: peer.device_id.clone(),
            name: peer.name.clone(),
            zone: zones.label(&peer.device_id).cloned(),
            last_seen_secs_ago: Some(now_epoch.saturating_sub(peer.last_seen_epoch_s)),
            online: peer.proven_online(now_epoch),
            chain_len: peer.last_verified_length.unwrap_or(0),
            badge,
            cards,
        });
    }
    out
}

/// Fetch and classify the recent verified timeline.
///
/// Reads a **signed export bundle** rather than the raw event list: the
/// bundle is bounded by a time window (so this cannot pull an unbounded log
/// into memory) and it is the artifact whose signature means something. What
/// gets verified is the bundle, which is why `TimelineEntry::bundle_badge` is
/// named the way it is — the kernel's per-event records carry no signature of
/// their own, and the rear must not imply one.
fn refresh_timeline(
    agent: &ureq::Agent,
    kernel: &KernelConfig,
    token: Option<&str>,
    zones: &ZoneTable,
) -> Result<Vec<TimelineEntry>> {
    let url = format!(
        "{}/export/bundle?last={}",
        kernel.url.trim_end_matches('/'),
        kernel.timeline_window
    );
    let body = kernel_get(agent, &url, token)?;
    let doc: serde_json::Value = serde_json::from_str(&body).context("export bundle JSON")?;
    let artifact = doc
        .get("artifact")
        .ok_or_else(|| anyhow!("export bundle has no artifact"))?;
    // The bundle carries a device public key and a receipt entry; verifying
    // that pairing is the kernel verifier's job, not this surface's, so the
    // badge stays at `Signed` — a well-formed signature this process did not
    // itself check against a pinned key (AD-Core section 2.5, no
    // overclaiming).
    let bundle_badge =
        if doc.get("receipt_entry").is_some() && doc.get("device_public_key").is_some() {
            Badge::Signed
        } else {
            Badge::Unsigned
        };

    let mut out = Vec::new();
    for batch in artifact
        .get("batches")
        .and_then(|v| v.as_array())
        .map(Vec::as_slice)
        .unwrap_or_default()
    {
        for bucket in batch
            .get("buckets")
            .and_then(|v| v.as_array())
            .map(Vec::as_slice)
            .unwrap_or_default()
        {
            let bucket_start = bucket
                .get("time_bucket")
                .and_then(|b| b.get("start_epoch_s"))
                .and_then(serde_json::Value::as_u64)
                .unwrap_or(0);
            let bucket_size = bucket
                .get("time_bucket")
                .and_then(|b| b.get("size_s"))
                .and_then(serde_json::Value::as_u64)
                .unwrap_or(600);
            for event in bucket
                .get("events")
                .and_then(|v| v.as_array())
                .map(Vec::as_slice)
                .unwrap_or_default()
            {
                let zone_id = event
                    .get("zone_id")
                    .and_then(serde_json::Value::as_str)
                    .unwrap_or_default();
                out.push(TimelineEntry {
                    // The scrubber's front phrase is a presence card in the
                    // event's zone and nothing else: the event's own type
                    // string never reaches the matrix, so a future event kind
                    // cannot widen the public vocabulary by existing.
                    phrase: PublicPhrase::Presence {
                        zone: zones.label(zone_id).cloned(),
                    },
                    bucket_start_epoch_s: bucket_start,
                    bucket_size_s: bucket_size,
                    attestation: event
                        .get("attestation")
                        .and_then(serde_json::Value::as_str)
                        .map(str::to_string),
                    bundle_badge,
                });
                if out.len() >= MAX_TIMELINE_ENTRIES {
                    return Ok(out);
                }
            }
        }
    }
    Ok(out)
}

/// Most timeline entries the scrubber holds (FR-4).
const MAX_TIMELINE_ENTRIES: usize = 256;
