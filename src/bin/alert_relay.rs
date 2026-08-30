//! The alert relay — remote "pokes" without a cloud (docs/design/alert_relay.md).
//!
//! Subscribes the narrow, fingerprint-free topic list in `relay::SUBSCRIBE_TOPICS`,
//! maps messages to coarse metadata-only pokes via the pure core, debounces per
//! (class, device), and fans each poke out to the configured sinks. The payload
//! is one owner-facing sentence and never event content, a timestamp, or a
//! device fingerprint — the same contract on every sink.
//!
//! Two sinks today, both optional, at least one required:
//! - **ntfy** (`--ntfy-url`): plain form POST — body text plus
//!   Title/Priority/Tags headers, exactly the `curl -d "..." ntfy.sh/<topic>`
//!   shape the design doc leads with. The topic name is the secret — treat it
//!   like a password. HTTPS by default; `--allow-http` exists for a
//!   self-hosted LAN ntfy only.
//! - **Busy Bar** (`--busybar-url`): the desk status light's local
//!   `POST /api/display/draw` — severity color plus the fixed title as
//!   scrolling text, auto-expiring (`relay::busybar` is the pure mapping).
//!   LAN-only by construction: the vendor's cloud proxy is refused.
//!
//! Each sink runs the identical machinery — its own per-(class, device)
//! debounce and its own retry lane — so a bar that is unplugged never delays
//! or consumes the phone's poke, and vice versa.

use std::collections::HashMap;
use std::sync::mpsc::channel;
use std::time::{Duration, Instant};

use anyhow::{anyhow, bail, Result};
use clap::Parser;
use rumqttc::{Event, Incoming, MqttOptions, QoS};
use witness_kernel::relay::{busybar, evaluate, Debouncer, Poke, PokeClass, SUBSCRIBE_TOPICS};
use witness_kernel::transport::{
    parse_mqtt_endpoint, validate_loopback_addr, TlsBackend, TlsConfig, TlsMaterials,
};

#[derive(Parser, Debug)]
#[command(
    author,
    version,
    about = "SecuraCV alert relay: metadata-only pokes over ntfy and/or a Busy Bar"
)]
struct Args {
    /// MQTT broker address (host:port, mqtt:// or mqtts:// URL).
    #[arg(long, env = "MQTT_BROKER_ADDR", default_value = "127.0.0.1:1883")]
    mqtt_broker_addr: String,

    /// Allow a non-loopback broker address (containers / remote brokers).
    #[arg(long, env = "ALLOW_REMOTE_MQTT", default_value_t = false)]
    allow_remote_mqtt: bool,

    #[arg(long, env = "MQTT_USERNAME")]
    mqtt_username: Option<String>,

    #[arg(long, env = "MQTT_PASSWORD")]
    mqtt_password: Option<String>,

    /// Force TLS to the broker even for a bare host:port address.
    #[arg(long, env = "MQTT_USE_TLS", default_value_t = false)]
    mqtt_use_tls: bool,

    #[arg(long, env = "MQTT_TLS_CA_PATH")]
    mqtt_tls_ca_path: Option<std::path::PathBuf>,

    #[arg(long, env = "MQTT_TLS_CLIENT_CERT_PATH")]
    mqtt_tls_client_cert_path: Option<std::path::PathBuf>,

    #[arg(long, env = "MQTT_TLS_CLIENT_KEY_PATH")]
    mqtt_tls_client_key_path: Option<std::path::PathBuf>,

    /// TLS backend: classic or hybrid_pq (needs the pqc-tls feature).
    #[arg(long, env = "MQTT_TLS_BACKEND", default_value = "classic")]
    mqtt_tls_backend: TlsBackend,

    #[arg(long, env = "MQTT_CLIENT_ID", default_value = "securacv-alert-relay")]
    mqtt_client_id: String,

    /// The ntfy topic URL to POST pokes to, e.g. https://ntfy.sh/<unguessable>.
    /// The topic name is the secret — generate it, don't pick it.
    #[arg(long, env = "NTFY_URL")]
    ntfy_url: Option<String>,

    /// Permit an http:// ntfy URL (self-hosted on the LAN only; the public
    /// internet gets TLS or nothing).
    #[arg(long, env = "NTFY_ALLOW_HTTP", default_value_t = false)]
    allow_http: bool,

    /// The Busy Bar's local base URL, e.g. http://10.0.4.20 over USB or its
    /// Wi-Fi address. LAN only — a busy.app cloud URL is refused.
    #[arg(long, env = "BUSYBAR_URL")]
    busybar_url: Option<String>,

    /// The bar's HTTP-access PIN, if one is set in its web UI
    /// (Settings > HTTP Access). Sent as X-API-Token on the LAN.
    #[arg(long, env = "BUSYBAR_TOKEN")]
    busybar_token: Option<String>,

    /// Optional link-home URL attached to every ntfy poke (your local UI,
    /// your Tailscale name — never a cloud page). Omitted when unset.
    #[arg(long, env = "RELAY_LINK_HOME")]
    link_home: Option<String>,

    /// Send one test poke and exit — the drill.
    ///
    /// It travels the identical path a real alert does: this binary, this
    /// config, every configured sink. That is the point. A test that stops
    /// short of the network proves the button is connected to itself and
    /// nothing else, which is the failure mode this flag exists to end.
    #[arg(long)]
    send_test: bool,
}

fn http_agent() -> ureq::Agent {
    // Bounded timeout so a hung server fails the poke instead of wedging
    // the relay (FR-4); a failure lands in the sink's retry lane.
    ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_secs(30)))
        .build()
        .into()
}

/// Drain (capped) so the connection can be reused; the body is unused.
fn drain(response: &mut ureq::http::Response<ureq::Body>) {
    let _ = response
        .body_mut()
        .with_config()
        .limit(64 * 1024)
        .read_to_vec();
}

/// One delivery target. Every sink receives the same [`Poke`] and nothing
/// else — a sink renders the coarse vocabulary, it never widens it.
enum Sink {
    Ntfy {
        url: String,
        link_home: Option<String>,
    },
    BusyBar {
        url: String,
        token: Option<String>,
    },
}

impl Sink {
    fn name(&self) -> &'static str {
        match self {
            Sink::Ntfy { .. } => "ntfy",
            Sink::BusyBar { .. } => "busybar",
        }
    }

    /// The host part, for logs. The ntfy topic name is the secret (the
    /// design doc's own rule), so journald and docker logs must never see
    /// past the host.
    fn redacted(&self) -> String {
        let url = match self {
            Sink::Ntfy { url, .. } => url,
            Sink::BusyBar { url, .. } => url,
        };
        url.split('/').take(3).collect::<Vec<_>>().join("/")
    }

    fn deliver(&self, poke: &Poke) -> Result<()> {
        match self {
            Sink::Ntfy { url, link_home } => {
                let mut req = http_agent()
                    .post(url)
                    .header("Title", poke.title)
                    .header("Priority", &poke.class.ntfy_priority().to_string())
                    .header("Tags", poke.class.sev());
                if let Some(link) = link_home {
                    req = req.header("Click", link);
                }
                let mut response = req
                    .send(poke.body.as_bytes())
                    .map_err(|e| anyhow!("ntfy POST failed: {e}"))?;
                drain(&mut response);
                Ok(())
            }
            Sink::BusyBar { url, token } => {
                let endpoint = format!("{}{}", url.trim_end_matches('/'), busybar::DRAW_PATH);
                let mut req = http_agent()
                    .post(&endpoint)
                    .header("Content-Type", "application/json");
                if let Some(token) = token {
                    req = req.header("X-API-Token", token);
                }
                let mut response = req
                    .send(busybar::draw_payload(poke).to_string().as_bytes())
                    .map_err(|e| anyhow!("Busy Bar draw failed: {e}"))?;
                drain(&mut response);
                Ok(())
            }
        }
    }
}

/// One sink plus its own debounce and retry state. Sinks fail independently
/// (the bar can be unplugged while ntfy is fine), so each runs the full
/// machinery: ready-check, deliver, record only on success, hold for retry
/// on failure. One shared debouncer would let one sink's success consume
/// another sink's send slot.
struct Lane {
    sink: Sink,
    debouncer: Debouncer,
    pending: HashMap<(PokeClass, String), (Poke, u64)>,
}

impl Lane {
    fn new(sink: Sink) -> Lane {
        Lane {
            sink,
            debouncer: Debouncer::default(),
            pending: HashMap::new(),
        }
    }
}

fn build_sinks(args: &Args) -> Result<Vec<Sink>> {
    let mut sinks = Vec::new();
    if let Some(url) = &args.ntfy_url {
        let scheme_ok =
            url.starts_with("https://") || (args.allow_http && url.starts_with("http://"));
        if !scheme_ok {
            bail!("ntfy URL must be https:// (pass --allow-http for a LAN self-host)");
        }
        sinks.push(Sink::Ntfy {
            url: url.clone(),
            link_home: args.link_home.clone(),
        });
    }
    if let Some(url) = &args.busybar_url {
        busybar::validate_url(url).map_err(|e| anyhow!(e))?;
        sinks.push(Sink::BusyBar {
            url: url.clone(),
            token: args.busybar_token.clone(),
        });
    }
    if sinks.is_empty() {
        bail!("no sink configured: pass --ntfy-url and/or --busybar-url");
    }
    Ok(sinks)
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let args = Args::parse();
    let sinks = build_sinks(&args)?;

    // The drill runs before any broker setup: what it proves is that THIS
    // config can reach the owner, and a broker that happens to be down must
    // not stop someone answering "will I be told?".
    if args.send_test {
        let poke = Poke::drill();
        let mut failed = false;
        for sink in &sinks {
            match sink.deliver(&poke) {
                Ok(()) => println!("Test poke sent via {}.", sink.name()),
                Err(e) => {
                    failed = true;
                    eprintln!("Test poke via {} FAILED: {e}", sink.name());
                }
            }
        }
        println!("If a sent poke does not show up within a few seconds, the");
        println!("fault is between this hub and that sink — not in your");
        println!("Canaries, which this drill deliberately did not involve.");
        if failed {
            bail!("at least one sink failed the drill");
        }
        return Ok(());
    }

    let endpoint = parse_mqtt_endpoint(&args.mqtt_broker_addr, args.mqtt_use_tls)?;
    if !args.allow_remote_mqtt {
        validate_loopback_addr(&endpoint, &args.mqtt_broker_addr)?;
    }
    let tls_config = TlsConfig {
        backend: args.mqtt_tls_backend,
        materials: TlsMaterials::load(
            args.mqtt_tls_ca_path.as_ref(),
            args.mqtt_tls_client_cert_path.as_ref(),
            args.mqtt_tls_client_key_path.as_ref(),
        )?,
    };
    tls_config.backend.validate_feature_support()?;

    let (tx, rx) = channel::<(String, Vec<u8>)>();
    let client_id = args.mqtt_client_id.clone();
    let username = args.mqtt_username.clone();
    let password = args.mqtt_password.clone();
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
        let (client, mut connection) = rumqttc::ClientBuilder::new(options).capacity(32).build();
        let mut sub_failed = false;
        for topic in SUBSCRIBE_TOPICS {
            if let Err(e) = client.subscribe(*topic, QoS::AtLeastOnce) {
                log::warn!("subscribe {topic} failed: {e}");
                sub_failed = true;
            }
        }
        if !sub_failed {
            for event in connection.iter() {
                match event {
                    Ok(Event::Incoming(Incoming::Publish(p))) => {
                        let topic = String::from_utf8_lossy(&p.topic).into_owned();
                        let _ = tx.send((topic, p.payload.to_vec()));
                    }
                    Ok(_) => {}
                    Err(e) => {
                        log::warn!("MQTT connection error: {e}; reconnecting");
                        break;
                    }
                }
            }
        }
        // Reconnect rather than run with missing subscriptions.
        std::thread::sleep(Duration::from_secs(3));
    });

    let mut lanes: Vec<Lane> = sinks.into_iter().map(Lane::new).collect();
    log::info!(
        "alert relay up: {} topics -> {}",
        SUBSCRIBE_TOPICS.len(),
        lanes
            .iter()
            .map(|l| format!("{} ({}/<redacted>)", l.sink.name(), l.sink.redacted()))
            .collect::<Vec<_>>()
            .join(", ")
    );

    let started = Instant::now();
    // Failed pokes wait in each lane's pending map, keyed per (class, device)
    // (latest wins), and retry on a cadence — a one-shot tamper alert must
    // survive a sink being briefly down. Bounded by the lane count; entries
    // expire after an hour of failures rather than growing a forever-queue
    // (FR-4).
    const RETRY_SECS: u64 = 30;
    const GIVE_UP_SECS: u64 = 3600;
    let mut last_retry: u64 = 0;

    loop {
        let now = started.elapsed().as_secs();
        match rx.recv_timeout(Duration::from_secs(10)) {
            Ok((topic, payload)) => {
                if let Some(poke) = evaluate(&topic, &payload) {
                    for lane in &mut lanes {
                        if lane.debouncer.ready(&poke, now) {
                            deliver(lane, poke.clone(), now);
                        } else {
                            log::debug!(
                                "debounced {} poke for '{}' on {}",
                                poke.class.sev(),
                                poke.device,
                                lane.sink.name()
                            );
                        }
                    }
                }
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                bail!("MQTT feed channel closed");
            }
        }
        // Retry lane: attempt held pokes on the cadence, newest state wins.
        if now.saturating_sub(last_retry) >= RETRY_SECS
            && lanes.iter().any(|l| !l.pending.is_empty())
        {
            last_retry = now;
            for lane in &mut lanes {
                let held: Vec<_> = lane.pending.drain().collect();
                for (key, (poke, first_failed)) in held {
                    if now.saturating_sub(first_failed) > GIVE_UP_SECS {
                        log::warn!(
                            "dropping undeliverable {} poke for {} after an hour of retries",
                            poke.class.sev(),
                            lane.sink.name()
                        );
                        continue;
                    }
                    match lane.sink.deliver(&poke) {
                        Ok(()) => {
                            lane.debouncer.record(&poke, now);
                            log::info!(
                                "poked via {} (retried): {} ({})",
                                lane.sink.name(),
                                poke.title,
                                poke.class.sev()
                            );
                        }
                        Err(e) => {
                            log::debug!("{} retry still failing: {e}", lane.sink.name());
                            lane.pending.insert(key, (poke, first_failed));
                        }
                    }
                }
            }
        }
    }
}

/// Try to send on one lane; on success record its debounce, on failure hold
/// the poke in that lane's retry map. A failed delivery must not consume the
/// send slot (relay core rule), and one sink's outcome never touches another
/// sink's state.
fn deliver(lane: &mut Lane, poke: Poke, now: u64) {
    match lane.sink.deliver(&poke) {
        Ok(()) => {
            lane.debouncer.record(&poke, now);
            log::info!(
                "poked via {}: {} ({})",
                lane.sink.name(),
                poke.title,
                poke.class.sev()
            );
        }
        Err(e) => {
            log::warn!("{} poke failed, holding for retry: {e}", lane.sink.name());
            let key = (poke.class, poke.device.clone());
            // Keep the earliest failure time if this lane is already held.
            let first = lane.pending.get(&key).map(|(_, t)| *t).unwrap_or(now);
            lane.pending.insert(key, (poke, first));
        }
    }
}
