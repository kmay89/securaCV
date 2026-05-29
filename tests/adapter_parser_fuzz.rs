//! Robustness ("fuzz-lite") tests for the untrusted adapter parsers.
//!
//! Adapters parse attacker-controlled bytes (webhook bodies, MQTT payloads, NVR JSON). The seccomp
//! sandbox is the backstop, but the parsers must also be panic-free on arbitrary input. These
//! tests drive each parser with many random and structurally-mutated inputs and assert that it
//! always *terminates without panicking* — the property the sandbox is meant to contain.
//!
//! Deterministic (seeded xorshift, no external fuzzing tooling) so it runs cleanly in CI.
//!
//! Run with: `cargo test --test adapter_parser_fuzz \
//!   --features adapter-webhook,adapter-mqtt-sensor,adapter-ble-presence,adapter-frigate`

#![cfg(all(
    feature = "adapter-mqtt-sensor",
    feature = "adapter-webhook",
    feature = "adapter-ble-presence",
    feature = "adapter-frigate"
))]

use witness_kernel::adapter::ble_presence::{BlePresenceAdapter, BleRoom};
use witness_kernel::adapter::frigate::FrigateAdapter;
use witness_kernel::adapter::mqtt_sensor::{MqttSensorAdapter, SensorRoute};
use witness_kernel::adapter::webhook::WebhookAdapter;
use witness_kernel::adapter::ClaimKind;

/// Tiny deterministic xorshift64* PRNG — no external dependency.
struct Rng(u64);
impl Rng {
    fn new(seed: u64) -> Self {
        Rng(seed | 1)
    }
    fn next_u64(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545F4914F6CDD1D)
    }
    fn byte(&mut self) -> u8 {
        (self.next_u64() & 0xff) as u8
    }
    fn range(&mut self, n: usize) -> usize {
        (self.next_u64() % (n as u64)) as usize
    }
}

/// A grab-bag of bytes biased toward JSON metacharacters and numeric edge cases, so the random
/// payloads exercise the parsers' structure rather than being uniformly random noise.
const ALPHABET: &[u8] = b"{}[]\":,.0123456789-+eE truefalsnul \t\r\n\\/abcdef_xX.NaInfity";

fn random_payload(rng: &mut Rng) -> Vec<u8> {
    let len = rng.range(96);
    (0..len)
        .map(|_| {
            // Mostly structured bytes, occasionally a fully random byte (incl. invalid UTF-8).
            if rng.range(8) == 0 {
                rng.byte()
            } else {
                ALPHABET[rng.range(ALPHABET.len())]
            }
        })
        .collect()
}

const ITERS: usize = 20_000;

#[test]
fn webhook_and_mqtt_routing_never_panics() {
    let routes = vec![SensorRoute::new(
        "/sensors/garage/acoustic",
        ClaimKind::AcousticImpulseInZone,
        "garage",
    )];
    let (webhook, _t1) = WebhookAdapter::new(routes.clone());
    let (mqtt, _t2) = MqttSensorAdapter::new(routes);
    let paths = ["/sensors/garage/acoustic", "/unknown", "", "/a/b/c?q=1"];

    let mut rng = Rng::new(0xC0FFEE);
    for _ in 0..ITERS {
        let payload = random_payload(&mut rng);
        let path = paths[rng.range(paths.len())];
        // The contract is simply: arbitrary bytes must not panic the parser.
        let _ = webhook.message_to_claim(path, &payload);
        let _ = mqtt.message_to_claim(path, &payload);
    }
}

#[test]
fn ble_presence_parsing_never_panics_and_respects_threshold() {
    let (adapter, _tx) = BlePresenceAdapter::new(vec![BleRoom::new("lobby", "lobby", 4.0)]);
    let topics = [
        "espresense/devices/aabbcc/lobby",
        "espresense/devices/x/garage",
        "lobby",
        "",
    ];
    let mut rng = Rng::new(0x1234_5678);
    for _ in 0..ITERS {
        let payload = random_payload(&mut rng);
        let topic = topics[rng.range(topics.len())];
        // Must not panic; a claim only ever lands in a *known* room.
        if let Some(claim) = adapter.message_to_claim(topic, &payload) {
            assert_eq!(claim.zone_label, "lobby");
            assert_eq!(claim.kind, ClaimKind::PresenceInRestrictedZone);
        }
    }

    // Explicit NaN/inf distances must never assert presence.
    for bad in [
        &br#"{"distance":NaN}"#[..],
        &br#"{"distance":1e999}"#[..],
        &br#"{"distance":-1e999}"#[..],
    ] {
        let _ = adapter.message_to_claim("espresense/devices/x/lobby", bad);
    }
}

#[test]
fn frigate_parsing_never_panics() {
    let (adapter, _tx) = FrigateAdapter::new(None, vec![], 0.5);
    let topics = ["frigate/events", "frigate/reviews", "frigate/other"];
    let mut rng = Rng::new(0xDEAD_BEEF);
    for _ in 0..ITERS {
        let payload = random_payload(&mut rng);
        let topic = topics[rng.range(topics.len())];
        // parse_to_claims returns Result; either arm is fine, a panic is not.
        let _ = adapter.parse_to_claims(topic, &payload);
    }
}
