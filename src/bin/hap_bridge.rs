//! `hap_bridge` — put the fleet in Apple Home.
//!
//! This is bridge site B of `docs/design/apple_home_integration.md` made
//! runnable: it advertises a HomeKit accessory over mDNS, pairs with a
//! controller using a setup code, and publishes the paced projection of
//! coarse witness state. No Home Assistant, no cloud, no account — an Apple
//! TV or HomePod on the same network is enough.
//!
//! ```text
//! hap_bridge setup     # find your hubs and Canaries, answer three questions
//! hap_bridge           # run it
//! ```
//!
//! # What it publishes, and what it cannot
//!
//! Only the closed vocabulary in `src/bridge/homekit.rs`: motion, occupancy,
//! contact, tamper, liveness, low battery — plus, and only if a human turns
//! them on, the four class-scoped motion signals that carry the sanctioned
//! `ObjectClass` word. There is no zone, no timestamp, no count, no
//! confidence and no identity field, because none of those exist to project.
//!
//! Publication happens on a **metronome**, never on an event. That is
//! Invariant III: the finest external time resolution anything downstream
//! can learn is one tick, and the publication rate itself carries no
//! information. `tick_ms` is therefore a privacy dial, not a performance
//! knob — coarsen it and external timing blurs; shorten it and automations
//! feel snappier.

use std::collections::{BTreeSet, HashMap};
use std::io::{IsTerminal, Write as _};
use std::net::SocketAddr;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver, SyncSender};
use std::time::{Duration, Instant};

use anyhow::{anyhow, Context, Result};
use clap::{Parser, Subcommand, ValueEnum};
use rumqttc::{Event, Incoming, MqttOptions, QoS};

use witness_kernel::bridge::hap::config::{self, BridgeConfig, CanaryConfig, MqttConfig};
use witness_kernel::bridge::hap::server::{self, serve, state_for, Fleet, CATEGORY_BRIDGE};
use witness_kernel::bridge::hap::{discover, qr, store, tty, wizard};
use witness_kernel::bridge::homekit::{HomeSignal, PacingConfig};
use witness_kernel::detect::ObjectClass;
use witness_kernel::EventType;

/// A `(device id, observation)` pair on its way from MQTT to a projection.
type ObservationMsg = (String, Observation);

/// One coarse fact learned from the fleet, on its way to a projection.
enum Observation {
    Event(EventType, Option<ObjectClass>),
    Level(HomeSignal, bool),
}

/// How many observations may wait for the next tick.
///
/// FR-4: the MQTT thread produces without waiting and the metronome consumes
/// once per tick, so an unbounded channel is a memory leak with a publisher
/// on the other end of it. Overflow is dropped and counted rather than
/// buffered — a bridge that runs out of memory tells a home nothing at all,
/// which is strictly worse than one that missed a beat and said so.
const OBSERVATION_QUEUE: usize = 1024;

/// How many observations one tick will drain.
///
/// Without this, a publisher faster than the tick keeps the drain loop fed
/// forever and publication never happens — the cadence would stall exactly
/// when the fleet is busiest, which is both a liveness bug and a timing leak.
const DRAIN_PER_TICK: usize = 4096;

/// How long the wizard listens for Canaries and Apple hubs.
const DISCOVERY_WINDOW: Duration = Duration::from_secs(4);

#[derive(Parser, Debug)]
#[command(
    name = "hap_bridge",
    about = "Publish the fleet's coarse witness state into Apple Home over HAP",
    long_about = "Put your Canaries in the Home app.\n\n\
                  Run `hap_bridge setup` first — it looks for your Apple TVs \
                  and your Canaries, asks three questions, and shows you a QR \
                  code to scan. After that, `hap_bridge` with no arguments \
                  runs what you set up."
)]
struct Cli {
    #[command(subcommand)]
    command: Option<Command>,

    /// The config file written by `hap_bridge setup`.
    #[arg(long, default_value = config::DEFAULT_PATH, env = "HAP_CONFIG", global = true)]
    config: PathBuf,

    /// How to draw the setup QR.
    #[arg(long, value_enum, default_value_t = QrStyle::Ansi, global = true)]
    qr: QrStyle,

    // ---- overrides for a one-off run, without a config file ----
    /// A Canary to bridge, as `<mqtt-device-id>=<display name>`. Repeatable.
    /// Overrides the config's fleet entirely when given.
    #[arg(long = "canary", value_parser = parse_canary)]
    canaries: Vec<(String, String)>,

    /// Where the accessory identity, setup code and pairings live.
    #[arg(long, env = "HAP_STATE")]
    state: Option<PathBuf>,

    /// Address to listen on.
    #[arg(long)]
    bind: Option<String>,

    /// The bridge accessory's name in the Home app.
    #[arg(long)]
    bridge_name: Option<String>,

    /// Milliseconds between publications — the privacy/latency dial.
    #[arg(long)]
    tick_ms: Option<u32>,

    /// MQTT broker host carrying the fleet's events.
    #[arg(long, env = "MQTT_HOST")]
    mqtt_host: Option<String>,

    /// MQTT broker port.
    #[arg(long, env = "MQTT_PORT")]
    mqtt_port: Option<u16>,

    /// Topic prefix the fleet publishes under.
    #[arg(long, env = "MQTT_TOPIC_PREFIX")]
    mqtt_prefix: Option<String>,

    /// MQTT username.
    #[arg(long, env = "MQTT_USERNAME")]
    mqtt_username: Option<String>,

    /// MQTT password.
    #[arg(long, env = "MQTT_PASSWORD", hide_env_values = true)]
    mqtt_password: Option<String>,

    /// Turn on a class-scoped signal. Repeatable.
    #[arg(long = "enable-class", value_parser = parse_class_signal)]
    enable_class: Vec<HomeSignal>,

    /// Run without an MQTT source: the accessory still pairs and still
    /// publishes on the metronome, but every signal stays clear.
    #[arg(long)]
    no_mqtt: bool,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Find your Apple hubs and Canaries, then write a config. Start here.
    Setup,
    /// Show the setup code and QR for an accessory you already configured.
    Code,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, ValueEnum)]
enum QrStyle {
    /// ANSI background colors. Correct on any terminal theme.
    Ansi,
    /// Block glyphs for a light-background terminal.
    Light,
    /// Block glyphs for a dark-background terminal.
    Dark,
    /// Print the code and URI only.
    Off,
}

impl QrStyle {
    fn style(self) -> Option<qr::Style> {
        match self {
            QrStyle::Ansi => Some(qr::Style::Ansi),
            QrStyle::Light => Some(qr::Style::Blocks),
            QrStyle::Dark => Some(qr::Style::BlocksInverted),
            QrStyle::Off => None,
        }
    }
}

fn parse_canary(s: &str) -> Result<(String, String), String> {
    match s.split_once('=') {
        Some((id, name)) if !id.is_empty() && !name.is_empty() => {
            Ok((id.to_string(), name.to_string()))
        }
        _ => Err(format!(
            "expected `<mqtt-device-id>=<display name>`, got {s:?}"
        )),
    }
}

fn parse_class_signal(s: &str) -> Result<HomeSignal, String> {
    config::class_signal(s).ok_or_else(|| {
        format!(
            "unknown class-scoped signal {s:?}; expected one of {:?}",
            config::class_signal_names()
        )
    })
}

/// Map an event-type string onto the sanctioned vocabulary.
///
/// Accepts the serde variant name the kernel publishes; anything unknown is
/// ignored rather than guessed at, because forcing an unrecognized event
/// into "motion" would be a false statement about someone's home.
fn parse_event_type(s: &str) -> Option<EventType> {
    serde_json::from_str::<EventType>(&format!("\"{s}\"")).ok()
}

/// Read a named boolean field out of a JSON payload, if it is there.
///
/// Returns `None` when the field is absent or unreadable, so a snapshot that
/// simply does not carry this field leaves the signal alone instead of
/// asserting that it is false.
fn json_bool_field(payload: &[u8], field: &str) -> Option<bool> {
    let value: serde_json::Value = serde_json::from_slice(payload).ok()?;
    match value.get(field)? {
        serde_json::Value::Bool(b) => Some(*b),
        serde_json::Value::String(s) => Some(is_truthy_word(s)),
        serde_json::Value::Number(n) => Some(n.as_f64().is_some_and(|f| f != 0.0)),
        _ => None,
    }
}

/// Map a payload's class word onto the sanctioned [`ObjectClass`] vocabulary.
///
/// Anything unrecognized becomes `None`, never a guess: the class word is the
/// one sanctioned step past the dumb-PIR bar, so it is asserted only when the
/// pipeline actually said it. `Unknown` maps to `None` too — the projection
/// treats it as no class at all.
fn parse_object_class(s: &str) -> Option<ObjectClass> {
    match s.trim().to_ascii_lowercase().as_str() {
        "person" => Some(ObjectClass::Person),
        "vehicle" => Some(ObjectClass::Vehicle),
        "animal" => Some(ObjectClass::Animal),
        "package" => Some(ObjectClass::Package),
        _ => None,
    }
}

/// Read a payload as a boolean. Accepts the plain words a device might send
/// and the JSON object the kernel sends, and treats anything it does not
/// recognize as "no" — failing closed, so a malformed payload cannot assert
/// a signal.
fn truthy(payload: &[u8]) -> bool {
    let Ok(text) = std::str::from_utf8(payload) else {
        return false;
    };
    let text = text.trim();
    if let Ok(value) = serde_json::from_str::<serde_json::Value>(text) {
        if let Some(b) = value.as_bool() {
            return b;
        }
        if let Some(obj) = value.as_object() {
            for key in ["state", "value", "active", "detected", "tampered"] {
                match obj.get(key) {
                    Some(serde_json::Value::Bool(b)) => return *b,
                    Some(serde_json::Value::String(s)) => return is_truthy_word(s),
                    _ => {}
                }
            }
        }
    }
    is_truthy_word(text)
}

fn is_truthy_word(s: &str) -> bool {
    matches!(
        s.trim().to_ascii_lowercase().as_str(),
        "1" | "on" | "true" | "yes" | "online" | "open" | "detected" | "active"
    )
}

fn main() -> Result<()> {
    env_logger::init();
    let cli = Cli::parse();
    match cli.command {
        Some(Command::Setup) => run_setup(&cli),
        Some(Command::Code) => show_code(&cli),
        None => run_serve(&cli),
    }
}

// ---------------------------------------------------------------- the wizard

fn prompt(question: &str, default: &str) -> Result<String> {
    if default.is_empty() {
        print!("{question} ");
    } else {
        print!("{question} [{default}] ");
    }
    std::io::stdout().flush()?;
    let mut line = String::new();
    if std::io::stdin().read_line(&mut line)? == 0 {
        // EOF — a piped or non-interactive run. Take the default rather than
        // spinning forever on a closed stdin.
        return Ok(default.to_string());
    }
    let t = line.trim().to_string();
    Ok(if t.is_empty() { default.to_string() } else { t })
}

/// Where the state file goes when the user did not say.
///
/// Beside the config, absolutized. A bare `hap_state.json` is relative to the
/// working directory, so a config at `~/.config/securacv/hap.toml` later
/// started by a service from `/` would mint a **brand-new accessory identity**
/// — and every existing pairing in the house would stop working, with nothing
/// to explain why.
fn default_state_beside(config: &Path) -> PathBuf {
    let dir = config
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .map(Path::to_path_buf)
        .unwrap_or_else(|| PathBuf::from("."));
    // `canonicalize` fails when the directory does not exist yet — which is
    // the common case, since the wizard creates it when it saves. Falling
    // back to the relative path would reintroduce the very bug this avoids,
    // so resolve against the working directory by hand instead.
    let dir = dir.canonicalize().unwrap_or_else(|_| {
        std::env::current_dir()
            .map(|cwd| cwd.join(&dir))
            .unwrap_or(dir)
    });
    dir.join("hap_state.json")
}

/// Ask for something that must not appear on screen.
///
/// The broker password went through the ordinary prompt, which echoes — so it
/// sat in plain sight, in the scrollback, and in any terminal recording.
///
/// The termios handling lives in [`tty`], not here: getting echo *back* on is
/// the hard half, and it has to survive Ctrl-C, which kills the process
/// without running destructors. See that module for why a `Drop` guard alone
/// is not enough. Where echo cannot be suppressed at all — piped input, no
/// terminal — the prompt still runs, and says so, because a visible password
/// beats a setup that will not continue.
fn prompt_secret(question: &str) -> Result<String> {
    let guard = match tty::hide_stdin_echo() {
        Ok(g) => Some(g),
        Err(e) => {
            println!("      (echo cannot be turned off here, so this will be visible: {e})");
            None
        }
    };
    let hidden = guard.is_some();

    print!("{question} ");
    let flushed = std::io::stdout().flush();
    let mut line = String::new();
    let read = std::io::stdin().read_line(&mut line);
    drop(guard);

    // The Enter that ended the line was swallowed along with the echo, so
    // supply the newline it would have drawn — but only when it was actually
    // eaten, or an echoing fallback gets a stray blank line.
    if hidden {
        println!();
    }

    flushed?;
    read?;
    Ok(line.trim().to_string())
}

fn step(n: u8, of: u8, title: &str) {
    println!("\n\x1b[1m[{n}/{of}]\x1b[0m {title}");
}

fn run_setup(cli: &Cli) -> Result<()> {
    if !std::io::stdin().is_terminal() {
        return Err(anyhow!(
            "`hap_bridge setup` is interactive and stdin is not a terminal.\n\
             Write {} by hand, or run setup from a shell.",
            cli.config.display()
        ));
    }

    // Rerunning setup on a paired bridge must not renumber anyone: accessory
    // ids come from list position and controllers cache them. So the existing
    // answers are loaded and become the defaults, and the fleet is MERGED
    // rather than replaced.
    let existing = if cli.config.exists() {
        match config::load(&cli.config) {
            Ok(c) => Some(c),
            Err(e) => {
                println!(
                    "\x1b[33m•\x1b[0m Could not read {}: {e}",
                    cli.config.display()
                );
                println!("  Starting fresh. The old file will be overwritten.\n");
                None
            }
        }
    } else {
        None
    };
    let base = existing.clone().unwrap_or_default();

    println!("\x1b[1mSecuraCV → Apple Home\x1b[0m");
    println!("Three questions and a QR code.\n");
    if existing.is_some() {
        println!(
            "\x1b[32m✓\x1b[0m Found your existing setup ({} {} already configured).",
            base.canaries.len(),
            if base.canaries.len() == 1 {
                "Canary"
            } else {
                "Canaries"
            }
        );
        println!("  Your answers below become the new defaults. Devices you already");
        println!("  have keep their place in the list — renaming is safe, reordering");
        println!("  is not, so nothing gets moved.\n");
    }
    println!("Your Canaries will appear in the Home app as ordinary sensors —");
    println!("motion, occupancy, contact — that Siri and automations understand.");
    println!("No video leaves. There is no field in the vocabulary for it.");

    // ---- 1. Is there a hub? ------------------------------------------------
    step(1, 4, "Looking for an Apple TV or HomePod…");
    let devices = discover::find_apple_devices(DISCOVERY_WINDOW);
    let summary = discover::summarize(&devices);
    let hubs = devices.iter().filter(|d| d.kind.is_home_hub()).count();
    if hubs > 0 {
        println!("      \x1b[32m✓\x1b[0m {summary}");
        println!("      (Being on the network is not the same as being signed in");
        println!("       as a Home Hub — that is a setting only the Home app knows.)");
    } else {
        println!("      \x1b[33m•\x1b[0m {summary}");
        println!("      Pairing will still work from your iPhone on this network.");
        println!("      Without a hub, though, automations do not run and you");
        println!("      cannot check the fleet while you are out. An Apple TV or");
        println!("      HomePod signed into your Apple Account fixes that.");
    }

    // ---- 2. Which Canaries? ------------------------------------------------
    step(2, 4, "Looking for Canaries…");
    let mut mqtt = base.mqtt.clone();
    let use_mqtt = wizard::parse_yes_no(
        &prompt("      Is your fleet publishing to an MQTT broker?", "y")?,
        true,
    );

    let mut fleet: Vec<CanaryConfig> = Vec::new();
    if use_mqtt {
        mqtt.host = prompt("      Broker host:", &mqtt.host)?;
        mqtt.port = prompt("      Broker port:", &mqtt.port.to_string())?
            .parse()
            .unwrap_or(1883);
        mqtt.prefix = prompt("      Topic prefix:", &mqtt.prefix)?;
        // A saved login is offered explicitly rather than silently carried
        // over. Cloning `base.mqtt` and then only *setting* on a non-empty
        // answer made the prompt below a lie: pressing Enter at "blank for
        // none" kept the old credentials, so anonymous access was
        // unreachable and a stale secret outlived a broker change.
        //
        // Asked as its own question rather than by reserving a word like
        // "none" at the username prompt — any such word is also a username
        // somebody has.
        let keep_saved = match &mqtt.username {
            Some(user) => wizard::parse_yes_no(
                &prompt(&format!("      Saved login for \"{user}\". Keep it?"), "y")?,
                true,
            ),
            None => false,
        };
        if !keep_saved {
            let user = prompt("      Username (blank for none):", "")?;
            if user.is_empty() {
                // "None" has to mean none, including for the password — it
                // belonged to a login the user just declined.
                mqtt.username = None;
                mqtt.password = None;
            } else {
                mqtt.username = Some(user);
                let pass = prompt_secret("      Password:")?;
                // Likewise blank: a password-less login, not the previous
                // password, which may be for an entirely different broker.
                mqtt.password = (!pass.is_empty()).then_some(pass);
            }
        }

        println!("      Listening for {}s…", DISCOVERY_WINDOW.as_secs());
        let found = discover_canaries(&mqtt, DISCOVERY_WINDOW);
        if found.is_empty() {
            println!("      \x1b[33m•\x1b[0m Nothing announced itself.");
            println!("        That usually means the broker details are off, or the");
            println!("        fleet has not published since it last started.");
            println!("        You can name them by hand instead.");
        } else {
            println!("      \x1b[32m✓\x1b[0m Found {}:", found.len());
            for (i, id) in found.iter().enumerate() {
                println!("           {}. {id}", i + 1);
            }
            let pick = prompt("      Bridge which? (numbers, or blank for all)", "")?;
            for idx in wizard::parse_selection(&pick, found.len()) {
                let id = &found[idx];
                let suggested = wizard::suggest_name(id);
                let name = prompt(&format!("      Name for {id}:"), &suggested)?;
                fleet.push(CanaryConfig {
                    id: id.clone(),
                    name,
                });
            }
        }
    }

    // Adding by hand is *required* only when there is nothing at all —
    // neither discovered now nor configured before. On a re-run it is offered,
    // because the usual reason to run setup again is to add a device.
    loop {
        let required = fleet.is_empty() && base.canaries.is_empty();
        if required {
            println!("      Add a Canary by hand.");
        } else if !wizard::parse_yes_no(&prompt("      Add a Canary by hand?", "n")?, false) {
            break;
        }
        let id = prompt("      MQTT device id:", "")?;
        if id.is_empty() {
            if required {
                return Err(anyhow!(
                    "no Canaries chosen — nothing to publish, so setup stopped here"
                ));
            }
            break;
        }
        let name = prompt("      Name in the Home app:", &wizard::suggest_name(&id))?;
        fleet.push(CanaryConfig { id, name });
    }

    // ---- 3. How much to tell it? ------------------------------------------
    step(3, 4, "How much should Apple Home be told?");
    println!("      Always on: motion, occupancy, contact, tamper, liveness,");
    println!("      battery. That is what a hardware PIR sensor publishes.");
    println!();
    println!("      One step further is the *kind* of thing that moved —");
    println!("      person, vehicle, animal, package. Still not identity: no");
    println!("      face, no plate, no name, and no field for one. But it is");
    println!("      more than a dumb sensor says, so it is off unless you ask.");
    let want_class = wizard::parse_yes_no(
        &prompt("      Tell Apple Home what kind of thing moved?", "n")?,
        false,
    );
    let enable_class: Vec<String> = if want_class {
        config::class_signal_names()
            .into_iter()
            .map(String::from)
            .collect()
    } else {
        Vec::new()
    };

    // ---- 4. How fast? ------------------------------------------------------
    step(4, 4, "How quickly should it react?");
    println!("      The bridge publishes on a metronome — on that cadence");
    println!("      whether or not anything happened. That is what stops the");
    println!("      traffic itself from revealing when something did.");
    println!();
    for (i, (label, _)) in wizard::PACING_CHOICES.iter().enumerate() {
        println!("           {}. {label}", i + 1);
    }
    let tick_ms = loop {
        let answer = prompt("      Choose:", "1")?;
        match wizard::parse_pacing(&answer, PacingConfig::default().tick_ms) {
            Some(ms) => break ms,
            None => println!("      Not a number — pick 1, 2, 3, or a value in ms."),
        }
    };

    // ---- write it out ------------------------------------------------------
    let state_path = cli
        .state
        .clone()
        .unwrap_or_else(|| default_state_beside(&cli.config));
    let cfg = BridgeConfig {
        bridge_name: cli.bridge_name.clone().unwrap_or(base.bridge_name),
        state: cli.state.clone().unwrap_or(if existing.is_some() {
            base.state
        } else {
            state_path
        }),
        bind: cli.bind.clone().unwrap_or(base.bind),
        tick_ms,
        enable_class,
        mqtt,
        // Existing devices keep their slots; only genuinely new ones are
        // appended. See `config::merge_fleet` for why this is not a re-sort.
        canaries: config::merge_fleet(&base.canaries, &fleet),
    };
    cfg.validate()?;
    config::save(&cli.config, &cfg)
        .with_context(|| format!("could not write {}", cli.config.display()))?;
    println!("\n\x1b[32m✓\x1b[0m Saved to {}", cli.config.display());

    // ---- the payoff --------------------------------------------------------
    let state = store::load_or_create(&cfg.state)?;
    print_pairing(&cfg, &state, cli.qr)?;

    if wizard::parse_yes_no(&prompt("\nStart the bridge now?", "y")?, true) {
        serve_with(&cfg, cli.no_mqtt, cli.qr, false)?;
    } else {
        println!(
            "\nWhen you are ready:  hap_bridge --config {}",
            cli.config.display()
        );
    }
    Ok(())
}

/// Listen for whatever announces itself on the fleet's topics.
///
/// `status` and `state` are retained, so a broker replays them the moment we
/// subscribe — which is why a few seconds is enough to find a fleet that has
/// been quietly running for months.
fn discover_canaries(mqtt: &MqttConfig, window: Duration) -> Vec<String> {
    let client_id = format!("securacv-hap-discover-{}", std::process::id());
    let mut options = MqttOptions::new(client_id, (mqtt.host.as_str(), mqtt.port));
    options.set_keep_alive(30);
    options.set_clean_start(true);
    if let Some(user) = &mqtt.username {
        options.set_credentials(user, mqtt.password.clone().unwrap_or_default());
    }
    let (client, mut connection) = rumqttc::ClientBuilder::new(options).capacity(64).build();
    for suffix in ["status", "state", "events"] {
        let _ = client.subscribe(format!("{}/+/{suffix}", mqtt.prefix), QoS::AtMostOnce);
    }

    let mut ids: BTreeSet<String> = BTreeSet::new();
    let deadline = Instant::now() + window;
    // `iter()` blocks until the next event, so on a broker with nothing
    // retained it would sit past the advertised window until the keepalive
    // fires — a wizard step that promises four seconds and takes thirty.
    // A timed receive bounds it to the window we told the user about.
    while let Some(remaining) = deadline.checked_duration_since(Instant::now()) {
        let Ok(event) = connection.recv_timeout(remaining) else {
            break;
        };
        if let Ok(Event::Incoming(Incoming::Publish(p))) = event {
            if let Ok(topic) = std::str::from_utf8(p.topic.as_ref()) {
                let mut parts = topic.split('/');
                if let (Some(_), Some(device)) = (parts.next(), parts.next()) {
                    if !device.is_empty() && device != "+" {
                        ids.insert(device.to_string());
                    }
                }
            }
        }
    }
    let _ = client.disconnect();
    ids.into_iter().collect()
}

// ------------------------------------------------------------------ printing

fn print_pairing(cfg: &BridgeConfig, state: &store::PersistedState, style: QrStyle) -> Result<()> {
    let uri = server::setup_uri(&state.setup_code, &state.setup_id, CATEGORY_BRIDGE);
    println!();
    if let Some(s) = style.style() {
        match qr::render(&uri, s) {
            Ok(code) => print!("{code}"),
            Err(e) => log::warn!("could not draw the setup QR: {e}"),
        }
        println!();
    }
    println!("      \x1b[1mSetup code  {}\x1b[0m", state.setup_code);
    println!("      Device ID   {}", state.device_id);
    println!();
    println!("  On your iPhone: \x1b[1mHome → + → Add Accessory\x1b[0m");
    if style.style().is_some() {
        println!("  Scan the code above, or tap \x1b[1mMore options…\x1b[0m and pick");
        println!("  \"{}\", then type the setup code.", cfg.bridge_name);
    } else {
        println!(
            "  \x1b[1mMore options…\x1b[0m → \"{}\" → type the setup code.",
            cfg.bridge_name
        );
    }
    println!();
    println!("  iOS will say \"uncertified accessory\" and offer \x1b[1mAdd Anyway\x1b[0m.");
    println!("  That is expected: this is not an Apple-certified product, and");
    println!("  every Homebridge and HomeSpan user sees the same prompt.");
    println!();
    println!("  The setup code is a password you type once, not a secret to");
    println!("  keep forever — but anyone who has it can pair while the bridge");
    println!("  is unpaired, so treat it like a door key until you have.");
    Ok(())
}

fn show_code(cli: &Cli) -> Result<()> {
    let cfg = effective_config(cli)?;
    let state = store::load_or_create(&cfg.state)?;
    if !state.pairings.is_empty() {
        println!(
            "\x1b[33m•\x1b[0m Already paired with {} controller(s).",
            state.pairings.len()
        );
        println!("  A paired accessory refuses new pairings by design. Remove it");
        println!("  from the Home app first if you want to pair again.\n");
    }
    print_pairing(&cfg, &state, cli.qr)
}

// -------------------------------------------------------------------- serving

/// The config file, with any command-line flags laid over the top.
fn effective_config(cli: &Cli) -> Result<BridgeConfig> {
    let mut cfg = if cli.config.exists() {
        config::load(&cli.config)?
    } else if cli.canaries.is_empty() {
        return Err(anyhow!(
            "no config at {} and no --canary given.\n\nRun `hap_bridge setup` — it will \
             find your Apple TVs and your Canaries, ask three questions, and show you a \
             QR code to scan.",
            cli.config.display()
        ));
    } else {
        BridgeConfig::default()
    };

    if !cli.canaries.is_empty() {
        cfg.canaries = cli
            .canaries
            .iter()
            .map(|(id, name)| CanaryConfig {
                id: id.clone(),
                name: name.clone(),
            })
            .collect();
    }
    if let Some(v) = &cli.state {
        cfg.state = v.clone();
    }
    if let Some(v) = &cli.bind {
        cfg.bind = v.clone();
    }
    if let Some(v) = &cli.bridge_name {
        cfg.bridge_name = v.clone();
    }
    if let Some(v) = cli.tick_ms {
        cfg.tick_ms = v;
    }
    if let Some(v) = &cli.mqtt_host {
        cfg.mqtt.host = v.clone();
    }
    if let Some(v) = cli.mqtt_port {
        cfg.mqtt.port = v;
    }
    if let Some(v) = &cli.mqtt_prefix {
        cfg.mqtt.prefix = v.clone();
    }
    if cli.mqtt_username.is_some() {
        cfg.mqtt.username = cli.mqtt_username.clone();
    }
    if cli.mqtt_password.is_some() {
        cfg.mqtt.password = cli.mqtt_password.clone();
    }
    if !cli.enable_class.is_empty() {
        cfg.enable_class = cli
            .enable_class
            .iter()
            .map(|s| s.as_str().to_string())
            .collect();
    }
    cfg.validate()?;
    Ok(cfg)
}

fn run_serve(cli: &Cli) -> Result<()> {
    let cfg = effective_config(cli)?;
    serve_with(&cfg, cli.no_mqtt, cli.qr, true)
}

fn serve_with(cfg: &BridgeConfig, no_mqtt: bool, style: QrStyle, show_pairing: bool) -> Result<()> {
    let mut state = store::load_or_create(&cfg.state)
        .with_context(|| format!("failed to open HAP state {}", cfg.state.display()))?;
    let identity = state.identity()?;

    // Only shout the setup code while it is still usable. Once a controller
    // has paired, printing it on every restart is noise at best and a code
    // sitting in a log at worst.
    if show_pairing && state.pairings.is_empty() {
        print_pairing(cfg, &state, style)?;
    } else if show_pairing {
        println!(
            "SecuraCV → Apple Home — paired with {} controller(s). \
             `hap_bridge code` shows the setup code.",
            state.pairings.len()
        );
    }

    let pacing = PacingConfig {
        tick_ms: cfg.tick_ms,
        ..PacingConfig::default()
    };
    witness_kernel::bridge::homekit::Projection::new(pacing)
        .map_err(|e| anyhow!("tick_ms {}: {e}", cfg.tick_ms))?;

    let canaries: Vec<(&str, &str)> = cfg
        .canaries
        .iter()
        .map(|c| (c.name.as_str(), c.id.as_str()))
        .collect();
    // Never-rot: if the fleet's shape moved since the last start (a Canary
    // added, removed, renamed, or reordered in hap.toml), bump the config
    // number now — the TXT record advertises the new c# from first packet,
    // and paired controllers re-read /accessories instead of trusting a
    // cache that no longer describes this fleet. No sync button: the
    // accessory database says when it changed.
    let shape = config::fleet_shape_hash(&cfg.canaries, &cfg.enable_class);
    if state.fleet_hash != shape {
        state.config_number += 1;
        state.fleet_hash = shape;
        store::save(&cfg.state, &state)?;
        log::info!(
            "fleet shape changed since last start; config number is now {}",
            state.config_number
        );
    }

    let mut server_state = state_for(identity, &cfg.bridge_name, &state.setup_code, &canaries);
    server_state.pairings = state.pairing_store();
    server_state.config_number = state.config_number;

    // device id → accessory id, fixed by the order the canaries were listed.
    let aid_of: HashMap<String, u64> = cfg
        .canaries
        .iter()
        .enumerate()
        .map(|(i, c)| (c.id.clone(), 2 + i as u64))
        .collect();
    let aids: Vec<u64> = (0..cfg.canaries.len()).map(|i| 2 + i as u64).collect();

    let mut fleet = Fleet::new(aids.iter().copied(), pacing)
        .map_err(|e| anyhow!("could not build the fleet: {e}"))?;

    for signal in cfg.class_signals() {
        for aid in &aids {
            if let Some(p) = fleet.projection_mut(*aid) {
                p.set_enabled(signal, true);
            }
        }
        log::info!(
            "class-scoped signal {} enabled: the Home app will see the coarse object class",
            signal.as_str()
        );
    }
    for (aid, signals) in server::enabled_summary(&server_state) {
        log::info!("accessory {aid} publishes: {}", signals.join(", "));
    }

    let (tx, rx): (SyncSender<ObservationMsg>, Receiver<ObservationMsg>) =
        mpsc::sync_channel(OBSERVATION_QUEUE);
    if !no_mqtt {
        spawn_mqtt(&cfg.mqtt, tx)?;
    } else {
        log::warn!("--no-mqtt: the accessory will pair but every signal stays clear");
    }

    let bind: SocketAddr = cfg
        .bind
        .parse()
        .map_err(|_| anyhow!("bind address {:?} is not valid", cfg.bind))?;
    let state_path = cfg.state.clone();
    let mut persisted_pairings = state.pairings.clone();
    let mut persisted_config = state.config_number;

    let server = serve(bind, server_state, pacing, move |s| {
        for _ in 0..DRAIN_PER_TICK {
            let Ok((device, observation)) = rx.try_recv() else {
                break;
            };
            let Some(aid) = aid_of.get(&device) else {
                continue;
            };
            if let Some(projection) = fleet.projection_mut(*aid) {
                match observation {
                    Observation::Event(e, class) => projection.observe_event(e, class),
                    Observation::Level(sig, on) => projection.set_level(sig, on),
                }
            }
        }
        let changed = fleet.tick(s);

        // Compare CONTENTS, not the count. An AddPairing that replaces an
        // existing controller's key or demotes it from admin changes neither
        // the table length nor the config number, so a length check would
        // skip the save and the old permission would come back on restart.
        state.absorb(&s.pairings, s.config_number);
        if state.pairings != persisted_pairings || state.config_number != persisted_config {
            match store::save(&state_path, &state) {
                Ok(()) => {
                    persisted_pairings = state.pairings.clone();
                    persisted_config = state.config_number;
                }
                Err(e) => log::error!("could not persist HAP pairings: {e}"),
            }
        }
        changed
    })
    .with_context(|| format!("failed to bind {}", cfg.bind))?;

    log::info!(
        "HAP bridge listening on {} — {} {} bridged, tick {} ms",
        server.local_addr(),
        cfg.canaries.len(),
        if cfg.canaries.len() == 1 {
            "Canary"
        } else {
            "Canaries"
        },
        cfg.tick_ms
    );

    let (stop_tx, stop_rx) = mpsc::channel::<()>();
    ctrlc::set_handler(move || {
        let _ = stop_tx.send(());
    })?;
    let _ = stop_rx.recv();
    log::info!("stopping");
    server.stop();
    Ok(())
}

/// Subscribe to the fleet's MQTT topics and translate them into
/// [`Observation`]s.
fn spawn_mqtt(mqtt: &MqttConfig, tx: SyncSender<ObservationMsg>) -> Result<()> {
    let client_id = format!("securacv-hap-{}", std::process::id());
    let mut options = MqttOptions::new(client_id, (mqtt.host.as_str(), mqtt.port));
    options.set_keep_alive(60);
    options.set_clean_start(true);
    if let Some(user) = &mqtt.username {
        options.set_credentials(user, mqtt.password.clone().unwrap_or_default());
    }

    let (client, mut connection) = rumqttc::ClientBuilder::new(options).capacity(64).build();
    let prefix = mqtt.prefix.clone();
    // `state` is where presence actually lives. The firmware publishes a
    // retained per-variant snapshot there (canary-sense `topics.h` builds
    // `securacv/<id>/state`, and its HA discovery reads
    // `value_json.presence`); there is no `presence` topic on the wire, so
    // subscribing to one would leave Home occupancy permanently false.
    for suffix in ["events", "state", "tamper", "status"] {
        client
            .subscribe(format!("{prefix}/+/{suffix}"), QoS::AtMostOnce)
            .with_context(|| format!("failed to subscribe to {prefix}/+/{suffix}"))?;
    }

    std::thread::spawn(move || {
        for event in connection.iter() {
            match event {
                Ok(Event::Incoming(Incoming::Publish(p))) => {
                    // rumqttc-next hands topics back as bytes; a topic that
                    // is not UTF-8 is not one we published, so skip it.
                    let Ok(topic) = std::str::from_utf8(p.topic.as_ref()) else {
                        continue;
                    };
                    let mut parts = topic.split('/');
                    let (Some(_), Some(device), Some(kind)) =
                        (parts.next(), parts.next(), parts.next())
                    else {
                        continue;
                    };
                    let observation = match kind {
                        "events" => {
                            let value =
                                serde_json::from_slice::<serde_json::Value>(&p.payload).ok();
                            let event = value.as_ref().and_then(|v| {
                                ["event_type", "type", "event"].iter().find_map(|k| {
                                    v.get(*k)
                                        .and_then(|x| x.as_str())
                                        .and_then(parse_event_type)
                                })
                            });
                            // The coarse class rides along when the pipeline
                            // knew one. It only ever reaches the wire if the
                            // matching class-scoped signal was explicitly
                            // enabled — the projection enforces that, not us.
                            let class = value.as_ref().and_then(|v| {
                                ["object_class", "class", "label"].iter().find_map(|k| {
                                    v.get(*k)
                                        .and_then(|x| x.as_str())
                                        .and_then(parse_object_class)
                                })
                            });
                            event.map(|e| Observation::Event(e, class))
                        }
                        "state" => json_bool_field(&p.payload, "presence")
                            .map(|on| Observation::Level(HomeSignal::Occupancy, on)),
                        // Tamper latches: a witness reporting that it was
                        // interfered with must stay reported until a human
                        // clears it, so a "false" here is deliberately not a
                        // way to un-say it.
                        "tamper" if truthy(&p.payload) => {
                            Some(Observation::Level(HomeSignal::Tamper, true))
                        }
                        "status" => {
                            Some(Observation::Level(HomeSignal::Active, truthy(&p.payload)))
                        }
                        _ => None,
                    };
                    if let Some(observation) = observation {
                        // Bounded, and non-blocking on purpose: blocking here
                        // would let a slow tick apply backpressure all the way
                        // into the MQTT event loop and stall the broker
                        // connection. A full queue drops the observation and
                        // says so.
                        match tx.try_send((device.to_string(), observation)) {
                            Ok(()) => {}
                            Err(mpsc::TrySendError::Full(_)) => {
                                log::warn!(
                                    "observation queue full ({OBSERVATION_QUEUE}); dropped an \
                                     update from {device} — the fleet is publishing faster than \
                                     the tick can drain"
                                );
                            }
                            Err(mpsc::TrySendError::Disconnected(_)) => break,
                        }
                    }
                }
                Ok(_) => {}
                Err(e) => {
                    // rumqttc reconnects on its own; log once per failure so
                    // a broker outage is visible without flooding.
                    log::warn!("MQTT: {e}");
                    std::thread::sleep(Duration::from_secs(5));
                }
            }
        }
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn canary_arguments_need_both_halves() {
        assert_eq!(
            parse_canary("canary-1=Porch Canary").expect("ok"),
            ("canary-1".to_string(), "Porch Canary".to_string())
        );
        assert!(parse_canary("canary-1").is_err());
        assert!(parse_canary("=Porch").is_err());
        assert!(parse_canary("canary-1=").is_err());
    }

    /// Only class-scoped signals may be turned on this way; the rest are not
    /// the operator's to widen from a flag.
    #[test]
    fn only_class_scoped_signals_can_be_enabled() {
        assert_eq!(
            parse_class_signal("motion_person").expect("ok"),
            HomeSignal::MotionPerson
        );
        assert!(parse_class_signal("motion").is_err());
        assert!(parse_class_signal("tamper").is_err());
        assert!(parse_class_signal("nonsense").is_err());
    }

    #[test]
    fn event_types_round_trip_through_their_serde_names() {
        assert_eq!(
            parse_event_type("BoundaryCrossingObjectLarge"),
            Some(EventType::BoundaryCrossingObjectLarge)
        );
        assert_eq!(parse_event_type("NotAnEvent"), None);
        assert_eq!(parse_event_type(""), None);
    }

    #[test]
    fn truthy_reads_the_shapes_a_device_might_send() {
        for yes in [
            &b"1"[..],
            b"on",
            b"true",
            b"ONLINE",
            b"detected",
            br#"{"state":"on"}"#,
            br#"{"value":true}"#,
            br#"true"#,
        ] {
            assert!(
                truthy(yes),
                "expected truthy: {:?}",
                std::str::from_utf8(yes)
            );
        }
        for no in [
            &b"0"[..],
            b"off",
            b"false",
            b"offline",
            br#"{"state":"off"}"#,
            b"",
        ] {
            assert!(
                !truthy(no),
                "expected falsey: {:?}",
                std::str::from_utf8(no)
            );
        }
    }

    /// A payload we cannot read must not assert a signal — an unreadable
    /// sensor is not a triggered one.
    #[test]
    fn unreadable_payloads_fail_closed() {
        assert!(!truthy(&[0xFF, 0xFE, 0x00]));
        assert!(!truthy(b"{ not json"));
        assert!(!truthy(br#"{"unrelated":"on"}"#));
    }

    /// Only the four sanctioned coarse classes parse. Anything else — most
    /// importantly anything identity-shaped — is `None`, never a guess.
    #[test]
    fn only_sanctioned_object_classes_parse() {
        assert_eq!(parse_object_class("person"), Some(ObjectClass::Person));
        assert_eq!(parse_object_class("Vehicle"), Some(ObjectClass::Vehicle));
        assert_eq!(parse_object_class(" animal "), Some(ObjectClass::Animal));
        assert_eq!(parse_object_class("package"), Some(ObjectClass::Package));
        for other in ["unknown", "face", "license_plate", "alice", ""] {
            assert_eq!(parse_object_class(other), None, "{other} must not parse");
        }
    }

    /// Presence rides inside the `state` snapshot; a snapshot without the
    /// field must leave the signal alone rather than asserting "not present".
    #[test]
    fn presence_is_read_from_the_state_snapshot() {
        assert_eq!(
            json_bool_field(br#"{"presence":true,"rssi":-40}"#, "presence"),
            Some(true)
        );
        assert_eq!(
            json_bool_field(br#"{"presence":false}"#, "presence"),
            Some(false)
        );
        assert_eq!(
            json_bool_field(br#"{"presence":"on"}"#, "presence"),
            Some(true)
        );
        // Absent, or unreadable: say nothing.
        assert_eq!(json_bool_field(br#"{"other":true}"#, "presence"), None);
        assert_eq!(json_bool_field(b"{ not json", "presence"), None);
        assert_eq!(json_bool_field(b"", "presence"), None);
    }

    /// The QR style flag must map onto a real renderer for every variant
    /// except the deliberate opt-out.
    #[test]
    fn every_qr_style_resolves_except_off() {
        assert_eq!(QrStyle::Ansi.style(), Some(qr::Style::Ansi));
        assert_eq!(QrStyle::Light.style(), Some(qr::Style::Blocks));
        assert_eq!(QrStyle::Dark.style(), Some(qr::Style::BlocksInverted));
        assert_eq!(QrStyle::Off.style(), None);
    }

    /// A relative state path is how every pairing in a house silently dies:
    /// a config in `~/.config` later started by a service from `/` would mint
    /// a brand-new accessory identity. The default must be absolute, and
    /// beside the config, even when that directory does not exist yet.
    #[test]
    fn the_default_state_path_is_absolute_and_beside_the_config() {
        let p = default_state_beside(Path::new("cfg/hap.toml"));
        assert!(p.is_absolute(), "got {p:?}");
        assert!(p.ends_with("cfg/hap_state.json"), "got {p:?}");

        let bare = default_state_beside(Path::new("hap.toml"));
        assert!(bare.is_absolute(), "got {bare:?}");
        assert!(bare.ends_with("hap_state.json"), "got {bare:?}");
    }

    #[test]
    fn the_cli_parses_its_own_help() {
        use clap::CommandFactory;
        Cli::command().debug_assert();
    }
}
