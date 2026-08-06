//! The alert relay — remote "pokes" without a cloud (docs/design/alert_relay.md).
//!
//! Subscribes the narrow, fingerprint-free topic list in `relay::SUBSCRIBE_TOPICS`,
//! maps messages to coarse metadata-only pokes via the pure core, debounces per
//! (class, device), and POSTs the poke to an ntfy topic URL. The topic name is
//! the secret — treat it like a password; the payload is one owner-facing
//! sentence and never event content, a timestamp, or a device fingerprint.
//!
//! Delivery is ntfy's plain form: body text plus Title/Priority/Tags headers,
//! exactly the `curl -d "..." ntfy.sh/<topic>` shape the design doc leads with.
//! HTTPS by default; `--allow-http` exists for a self-hosted LAN ntfy only.

use std::collections::HashMap;
use std::sync::mpsc::channel;
use std::time::{Duration, Instant};

use anyhow::{anyhow, bail, Result};
use clap::Parser;
use rumqttc::{Event, Incoming, MqttOptions, QoS};
use witness_kernel::relay::{evaluate, Debouncer, Poke, SUBSCRIBE_TOPICS};
use witness_kernel::transport::{
    parse_mqtt_endpoint, validate_loopback_addr, TlsBackend, TlsConfig, TlsMaterials,
};

#[derive(Parser, Debug)]
#[command(
    author,
    version,
    about = "SecuraCV alert relay: metadata-only pokes over ntfy"
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
    ntfy_url: String,

    /// Permit an http:// ntfy URL (self-hosted on the LAN only; the public
    /// internet gets TLS or nothing).
    #[arg(long, env = "NTFY_ALLOW_HTTP", default_value_t = false)]
    allow_http: bool,

    /// Optional link-home URL attached to every poke (your local UI, your
    /// Tailscale name — never a cloud page). Omitted when unset.
    #[arg(long, env = "RELAY_LINK_HOME")]
    link_home: Option<String>,

    /// Send one test poke and exit — the drill.
    ///
    /// It travels the identical path a real alert does: this binary, this
    /// config, this ntfy topic, your phone. That is the point. A test that
    /// stops short of the network proves the button is connected to itself
    /// and nothing else, which is the failure mode this flag exists to end.
    #[arg(long)]
    send_test: bool,
}

fn post_poke(args: &Args, poke: &Poke) -> Result<()> {
    let url = &args.ntfy_url;
    let scheme_ok = url.starts_with("https://") || (args.allow_http && url.starts_with("http://"));
    if !scheme_ok {
        bail!("ntfy URL must be https:// (pass --allow-http for a LAN self-host)");
    }
    // Bounded timeout so a hung ntfy server fails the poke instead of
    // wedging the relay (FR-4); a failure lands in the retry queue.
    let agent: ureq::Agent = ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_secs(30)))
        .build()
        .into();
    let mut req = agent
        .post(url)
        .header("Title", poke.title)
        .header("Priority", &poke.class.ntfy_priority().to_string())
        .header("Tags", poke.class.sev());
    if let Some(link) = &args.link_home {
        req = req.header("Click", link);
    }
    let mut response = req
        .send(poke.body.as_bytes())
        .map_err(|e| anyhow!("ntfy POST failed: {e}"))?;
    // Drain (capped) so the connection can be reused; the body is unused.
    let _ = response
        .body_mut()
        .with_config()
        .limit(64 * 1024)
        .read_to_vec();
    Ok(())
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let args = Args::parse();

    // The drill runs before any broker setup: what it proves is that THIS
    // config can reach the owner, and a broker that happens to be down must
    // not stop someone answering "will I be told?".
    if args.send_test {
        let poke = Poke::drill();
        post_poke(&args, &poke)?;
        println!("Test poke sent. If it does not arrive on your phone within a");
        println!("few seconds, the fault is between this hub and ntfy — not in");
        println!("your Canaries, which this drill deliberately did not involve.");
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

    // The topic name is the secret (the design doc's own rule), so logs get
    // the host only — journald and docker logs must never leak it.
    let redacted = args
        .ntfy_url
        .split('/')
        .take(3)
        .collect::<Vec<_>>()
        .join("/");
    log::info!(
        "alert relay up: {} topics -> {}/<redacted>",
        SUBSCRIBE_TOPICS.len(),
        redacted
    );

    let started = Instant::now();
    let mut debouncer = Debouncer::default();
    // Failed pokes wait here, keyed per lane (latest wins), and retry on a
    // cadence — a one-shot tamper alert must survive ntfy being briefly
    // down. Bounded by the lane count; entries expire after an hour of
    // failures rather than growing a forever-queue (FR-4).
    let mut pending: HashMap<(witness_kernel::relay::PokeClass, String), (Poke, u64)> =
        HashMap::new();
    const RETRY_SECS: u64 = 30;
    const GIVE_UP_SECS: u64 = 3600;
    let mut last_retry: u64 = 0;

    loop {
        let now = started.elapsed().as_secs();
        match rx.recv_timeout(Duration::from_secs(10)) {
            Ok((topic, payload)) => {
                if let Some(poke) = evaluate(&topic, &payload) {
                    if debouncer.ready(&poke, now) {
                        deliver(&args, poke, now, &mut debouncer, &mut pending);
                    } else {
                        log::debug!("debounced {} poke for '{}'", poke.class.sev(), poke.device);
                    }
                }
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                bail!("MQTT feed channel closed");
            }
        }
        // Retry lane: attempt held pokes on the cadence, newest state wins.
        if now.saturating_sub(last_retry) >= RETRY_SECS && !pending.is_empty() {
            last_retry = now;
            let held: Vec<_> = pending.drain().collect();
            for (key, (poke, first_failed)) in held {
                if now.saturating_sub(first_failed) > GIVE_UP_SECS {
                    log::warn!(
                        "dropping undeliverable {} poke after an hour of retries",
                        poke.class.sev()
                    );
                    continue;
                }
                match post_poke(&args, &poke) {
                    Ok(()) => {
                        debouncer.record(&poke, now);
                        log::info!("poked (retried): {} ({})", poke.title, poke.class.sev());
                    }
                    Err(e) => {
                        log::debug!("retry still failing: {e}");
                        pending.insert(key, (poke, first_failed));
                    }
                }
            }
        }
    }
}

/// Try to send; on success record the debounce, on failure hold for retry.
fn deliver(
    args: &Args,
    poke: Poke,
    now: u64,
    debouncer: &mut Debouncer,
    pending: &mut HashMap<(witness_kernel::relay::PokeClass, String), (Poke, u64)>,
) {
    match post_poke(args, &poke) {
        Ok(()) => {
            debouncer.record(&poke, now);
            log::info!("poked: {} ({})", poke.title, poke.class.sev());
        }
        Err(e) => {
            log::warn!("poke failed, holding for retry: {e}");
            let key = (poke.class, poke.device.clone());
            // Keep the earliest failure time if this lane is already held.
            let first = pending.get(&key).map(|(_, t)| *t).unwrap_or(now);
            pending.insert(key, (poke, first));
        }
    }
}
