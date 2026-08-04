//! `hap_bridge` — put the fleet in Apple Home.
//!
//! This is bridge site B of `docs/design/apple_home_integration.md` made
//! runnable: it advertises a HomeKit accessory over mDNS, pairs with a
//! controller using a setup code, and publishes the paced projection of
//! coarse witness state. No Home Assistant, no cloud, no account — an Apple
//! TV or HomePod on the same network is enough.
//!
//! ```text
//! hap_bridge --canary canary-1="Porch Canary" --canary canary-2="Garage Canary"
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
//! information. `--tick-ms` is therefore a privacy dial, not a performance
//! knob — coarsen it and external timing blurs; shorten it and automations
//! feel snappier.

use std::collections::HashMap;
use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::mpsc::{self, Receiver, SyncSender};

use anyhow::{anyhow, Context, Result};
use clap::Parser;
use rumqttc::{Event, Incoming, MqttOptions, QoS};

use witness_kernel::bridge::hap::server::{self, serve, state_for, Fleet, CATEGORY_BRIDGE};
use witness_kernel::bridge::hap::store;
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

#[derive(Parser, Debug)]
#[command(
    name = "hap_bridge",
    about = "Publish the fleet's coarse witness state into Apple Home over HAP"
)]
struct Args {
    /// A Canary to bridge, as `<mqtt-device-id>=<display name>`. Repeat for
    /// each device; order fixes the HAP accessory ids, so keep it stable.
    #[arg(long = "canary", value_parser = parse_canary, required = true)]
    canaries: Vec<(String, String)>,

    /// Where the accessory identity, setup code and pairings live. Created
    /// `0600` on first run; back it up, because losing it means every
    /// controller in the house has to pair again.
    #[arg(long, default_value = "hap_state.json", env = "HAP_STATE")]
    state: PathBuf,

    /// Address to listen on. Port 0 asks the OS to choose.
    #[arg(long, default_value = "0.0.0.0:51826")]
    bind: SocketAddr,

    /// The name of the bridge accessory itself, as the Home app shows it.
    #[arg(long, default_value = "SecuraCV")]
    bridge_name: String,

    /// Milliseconds between publications — the privacy/latency dial.
    #[arg(long, default_value_t = 1000)]
    tick_ms: u32,

    /// MQTT broker host carrying the fleet's events.
    #[arg(long, default_value = "127.0.0.1", env = "MQTT_HOST")]
    mqtt_host: String,

    /// MQTT broker port.
    #[arg(long, default_value_t = 1883, env = "MQTT_PORT")]
    mqtt_port: u16,

    /// Topic prefix the fleet publishes under.
    #[arg(long, default_value = "securacv", env = "MQTT_TOPIC_PREFIX")]
    mqtt_prefix: String,

    /// MQTT username, if the broker requires one.
    #[arg(long, env = "MQTT_USERNAME")]
    mqtt_username: Option<String>,

    /// MQTT password.
    #[arg(long, env = "MQTT_PASSWORD", hide_env_values = true)]
    mqtt_password: Option<String>,

    /// Turn on a class-scoped signal. Repeatable. These carry the coarse
    /// `ObjectClass` word — the one sanctioned step past what a hardware PIR
    /// sensor would publish — and are off until you say otherwise.
    #[arg(long = "enable-class", value_parser = parse_class_signal)]
    enable_class: Vec<HomeSignal>,

    /// Print the setup code and pairing URI, then exit without serving.
    #[arg(long)]
    show_code: bool,

    /// Run without an MQTT source. The accessory still pairs and still
    /// publishes on the metronome; every signal simply stays clear. Useful
    /// for proving a pairing works before wiring the fleet to it.
    #[arg(long)]
    no_mqtt: bool,
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
    HomeSignal::ALL
        .into_iter()
        .find(|sig| sig.is_class_scoped() && sig.as_str() == s)
        .ok_or_else(|| {
            let allowed: Vec<&str> = HomeSignal::ALL
                .into_iter()
                .filter(|s| s.is_class_scoped())
                .map(|s| s.as_str())
                .collect();
            format!("unknown class-scoped signal {s:?}; expected one of {allowed:?}")
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
    let args = Args::parse();

    let mut state = store::load_or_create(&args.state)
        .with_context(|| format!("failed to open HAP state {}", args.state.display()))?;
    let identity = state.identity()?;

    let uri = server::setup_uri(&state.setup_code, &state.setup_id, CATEGORY_BRIDGE);
    println!("SecuraCV → Apple Home");
    println!("  Setup code : {}", state.setup_code);
    println!("  Pairing URI: {uri}");
    println!("  Device ID  : {}", state.device_id);
    println!();
    println!(
        "In the Home app: Add Accessory → More options… → pick \"{}\",",
        args.bridge_name
    );
    println!("then enter the setup code above. It is not a secret to protect");
    println!("forever — it is a password to type once — but anyone who has it");
    println!("can pair while the bridge is unpaired, so treat it like a door key.");
    if args.show_code {
        return Ok(());
    }

    let pacing = PacingConfig {
        tick_ms: args.tick_ms,
        ..PacingConfig::default()
    };
    // Refuse an out-of-range dial rather than clamping it: pacing is a
    // privacy parameter, so a caller who asked for something impossible must
    // be told, not quietly overruled.
    witness_kernel::bridge::homekit::Projection::new(pacing)
        .map_err(|e| anyhow!("--tick-ms {}: {e}", args.tick_ms))?;

    let canaries: Vec<(&str, &str)> = args
        .canaries
        .iter()
        .map(|(id, name)| (name.as_str(), id.as_str()))
        .collect();
    let mut server_state = state_for(identity, &args.bridge_name, &state.setup_code, &canaries);
    server_state.pairings = state.pairing_store();
    server_state.config_number = state.config_number;

    // device id → accessory id, fixed by the order the canaries were given.
    let aid_of: HashMap<String, u64> = args
        .canaries
        .iter()
        .enumerate()
        .map(|(i, (id, _))| (id.clone(), 2 + i as u64))
        .collect();

    let aids: Vec<u64> = aid_of.values().copied().collect();
    let mut fleet = Fleet::new(aids.iter().copied(), pacing)
        .map_err(|e| anyhow!("could not build the fleet: {e}"))?;

    // Class-scoped signals are opt-in, per device, and the choice is logged
    // so it is never a silent widening.
    for signal in &args.enable_class {
        for aid in &aids {
            if let Some(p) = fleet.projection_mut(*aid) {
                p.set_enabled(*signal, true);
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
    if !args.no_mqtt {
        spawn_mqtt(&args, tx)?;
    } else {
        log::warn!("--no-mqtt: the accessory will pair but every signal stays clear");
    }

    let state_path = args.state.clone();
    let mut persisted_pairings = state.pairings.clone();
    let mut persisted_config = state.config_number;
    let server = serve(args.bind, server_state, pacing, move |s| {
        // Drain everything learned since the last beat. Observations only
        // ever mark state pending; nothing here publishes. Publication is
        // the tick below, and it happens whether or not this drained
        // anything — that is the cover traffic.
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

        // Persist a pairing change as soon as it happens: a controller that
        // paired and then lost power to the bridge must still be paired.
        //
        // Compare CONTENTS, not the count. An AddPairing that replaces an
        // existing controller's key or demotes it from admin changes neither
        // the table length nor the config number, so a length check would
        // skip the save and the old key — or the old admin permission —
        // would come back on the next restart.
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
    .with_context(|| format!("failed to bind {}", args.bind))?;

    log::info!(
        "HAP bridge listening on {} — {} {} bridged, tick {} ms",
        server.local_addr(),
        args.canaries.len(),
        if args.canaries.len() == 1 {
            "Canary"
        } else {
            "Canaries"
        },
        args.tick_ms
    );

    // Serve until interrupted.
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
fn spawn_mqtt(args: &Args, tx: SyncSender<ObservationMsg>) -> Result<()> {
    let client_id = format!("securacv-hap-{}", std::process::id());
    let mut options = MqttOptions::new(client_id, (args.mqtt_host.as_str(), args.mqtt_port));
    options.set_keep_alive(60);
    options.set_clean_start(true);
    if let Some(user) = &args.mqtt_username {
        options.set_credentials(user, args.mqtt_password.clone().unwrap_or_default());
    }

    let (client, mut connection) = rumqttc::ClientBuilder::new(options).capacity(64).build();
    let prefix = args.mqtt_prefix.clone();
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
                    std::thread::sleep(std::time::Duration::from_secs(5));
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
        assert_eq!(
            json_bool_field(br#"{"presence":1}"#, "presence"),
            Some(1.0 != 0.0)
        );
        // Absent, or unreadable: say nothing.
        assert_eq!(json_bool_field(br#"{"other":true}"#, "presence"), None);
        assert_eq!(json_bool_field(b"{ not json", "presence"), None);
        assert_eq!(json_bool_field(b"", "presence"), None);
    }
}
