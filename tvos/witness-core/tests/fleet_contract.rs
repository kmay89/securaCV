//! The anti-drift gate for the `GET /api/fleet` contract.
//!
//! `tests/vectors.rs` pins the chain MATH to the kernel's fixtures. This file
//! pins the fleet CONTRACT — what the Wall's normalizer hands Swift for the
//! bytes a source actually serves — to `tests/fixtures/fleet_contract_vectors.json`.
//! It exists because the contract's one default drifted three ways at once:
//! `tvos/discovery/DISCOVERY.md` said a device omitting `online` was online,
//! this crate agreed, and the Swift decoder and the phone said the opposite.
//! A default that lives only in prose and in code can do that; one pinned in a
//! fixture cannot drift without a test going red.
//!
//! Change the contract doc first, then the fixture, then the code.

use std::path::PathBuf;

use securacv_witness_core::fleet::parse_fleet;
use serde_json::Value;

fn load_vectors() -> Vec<Value> {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join("fleet_contract_vectors.json");
    let raw = std::fs::read_to_string(&path).unwrap_or_else(|e| {
        panic!(
            "could not read the fleet contract vectors at {}: {e}\n\
             These fixtures are what keep the Wall's reading of /api/fleet pinned \
             to tvos/discovery/DISCOVERY.md. If they moved, update this path — \
             never delete the check.",
            path.display()
        )
    });
    let doc: Value = serde_json::from_str(&raw).expect("vectors file is not valid JSON");
    doc["vectors"]
        .as_array()
        .expect("vectors file has no `vectors` array")
        .clone()
}

#[test]
fn every_fleet_vector_normalizes_to_its_pinned_snapshot() {
    let vectors = load_vectors();
    assert!(
        !vectors.is_empty(),
        "the fleet contract vectors file is empty — the anti-drift check would pass vacuously"
    );

    for v in &vectors {
        let name = v["name"].as_str().expect("vector has no name");
        let input = v["input"]
            .as_str()
            .unwrap_or_else(|| panic!("vector {name:?} has no string `input`"));
        let expected = &v["normalized"];
        assert!(
            expected.is_object(),
            "vector {name:?} has no `normalized` object"
        );

        let snapshot = parse_fleet(input)
            .unwrap_or_else(|e| panic!("vector {name:?} did not parse as a fleet: {e}"));
        let got = serde_json::to_value(&snapshot).expect("snapshot must serialize");
        assert_eq!(
            &got, expected,
            "vector {name:?}: the normalizer's output disagrees with the pinned contract"
        );
    }
    eprintln!("checked {} fleet contract vectors", vectors.len());
}

#[test]
fn the_vectors_pin_the_online_default_to_false() {
    // Belt and braces: the check above would still pass if someone edited
    // BOTH the fixture and the code to say `true`. This asserts that the
    // fixture contains at least one device with `online` silent on the wire
    // and `false` in the normalized form — the contract's one load-bearing
    // default, stated once in DISCOVERY.md and pinned here.
    let vectors = load_vectors();
    let mut pinned = 0usize;
    for v in &vectors {
        let input: Value = serde_json::from_str(v["input"].as_str().unwrap())
            .expect("vector input must itself be JSON");
        let wire_devices: Vec<Value> = match &input {
            Value::Array(devices) => devices.clone(),
            Value::Object(_) => input["devices"].as_array().cloned().unwrap_or_default(),
            _ => Vec::new(),
        };
        let normalized_devices = v["normalized"]["devices"]
            .as_array()
            .expect("normalized has no devices");
        assert_eq!(
            wire_devices.len(),
            normalized_devices.len(),
            "vector {:?}: a device that answered at all is reported, never dropped",
            v["name"]
        );
        for (wire, norm) in wire_devices.iter().zip(normalized_devices) {
            if wire.get("online").is_none() {
                assert_eq!(
                    norm["online"],
                    Value::Bool(false),
                    "vector {:?}: a silent `online` must normalize to false — \
                     a silent field is never a presence claim",
                    v["name"]
                );
                pinned += 1;
            }
        }
    }
    assert!(
        pinned > 0,
        "no vector has a device with `online` silent — the default is not pinned"
    );
}

#[test]
fn the_kernel_vector_lists_a_peer_beside_the_kernel_itself() {
    // The repo-root kernel serves this exact `input` for one planted peer
    // (its `fleet_document_matches_the_shared_two_row_vector` test pins the
    // bytes). This side pins what the Wall makes of them: a two-row fleet,
    // the kernel first and a Canary the hub's bridge heard second, with the
    // peer's presence proven (`online: true`) and its room words intact.
    const NAME: &str =
        "the kernel's aggregated document (fleet_document, verbatim): itself plus one Canary the bridge heard";
    let vectors = load_vectors();
    let vector = vectors
        .iter()
        .find(|v| v["name"] == NAME)
        .unwrap_or_else(|| panic!("the kernel's two-row vector {NAME:?} is missing"));
    let input = vector["input"].as_str().expect("input is a string");
    let snapshot = parse_fleet(input).expect("the kernel's document parses as a fleet");
    assert_eq!(snapshot.kernel.as_deref(), Some("witness-kernel"));
    assert_eq!(
        snapshot.devices.len(),
        2,
        "two rows: the kernel, then the peer"
    );
    assert_eq!(
        snapshot.devices[0].product.as_deref(),
        Some("witness-kernel")
    );
    assert!(snapshot.devices[0].online);
    let peer = &snapshot.devices[1];
    assert_eq!(peer.name, "Bedroom");
    assert!(
        peer.online,
        "a signed chain publish inside the window proves presence"
    );
    assert_eq!(peer.chain.as_deref(), Some("ok"));
    assert_eq!(peer.presence.as_deref(), Some("present"));
    assert_eq!(peer.occupants.as_deref(), Some("1"));
    assert_eq!(peer.breathing, Some(true));
    assert!(!snapshot.has_chain_trouble());
    assert_eq!(snapshot.summary(), "2 Canaries, all online");

    // The bridge's private bookkeeping never reaches the wire: the kernel
    // projects its summary file through an allowlist, and these are the
    // fields that file carries which the contract does not.
    for private in [
        "pinned_key",
        "public_key",
        "device_id",
        "last_seen",
        "last_signed",
        "wellbeing_epoch",
        "\"fp\"",
    ] {
        assert!(
            !input.contains(private),
            "{private} is bridge bookkeeping and must not be served: {input}"
        );
    }
}

/// Days since the Unix epoch to a proleptic Gregorian (year, month, day), the
/// civil-from-days algorithm. Local to this test on purpose: the crate renders
/// no dates, and the point here is an independent reading of the day the
/// vectors' `born[].utc_date` names.
fn civil_from_days(days: i64) -> (i64, u32, u32) {
    let z = days + 719_468;
    let era = (if z >= 0 { z } else { z - 146_096 }) / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let month = (if mp < 10 { mp + 3 } else { mp - 9 }) as u32;
    let year = yoe + era * 400 + i64::from(month <= 2);
    (year, month, day)
}

#[test]
fn every_born_line_follows_the_normalized_pair() {
    // `born` is what every reader of these vectors must SAY for each row (the
    // Wall's DeviceDetailView, the website's tv/app.js and emulator): "Born"
    // only when the device said its day is exact, "First dated" for a day
    // without that, and nothing at all without a day. This holds the line to
    // the normalized pair it is drawn from, so the label and the UTC day
    // cannot drift from the snapshot that the readers decode.
    let vectors = load_vectors();
    let (mut born, mut first_dated, mut silent, mut dropped) = (0usize, 0usize, 0usize, 0usize);
    for v in &vectors {
        let name = v["name"].as_str().unwrap_or("?");
        let rows = v["normalized"]["devices"]
            .as_array()
            .expect("normalized has devices");
        let lines: Vec<Value> = match v.get("born") {
            Some(Value::Array(lines)) => lines.clone(),
            Some(other) => panic!("vector {name:?}: `born` must be an array, got {other}"),
            None => vec![Value::Null; rows.len()],
        };
        assert_eq!(
            lines.len(),
            rows.len(),
            "vector {name:?}: one `born` entry per normalized row"
        );
        let input: Value = serde_json::from_str(v["input"].as_str().unwrap()).unwrap();
        let wire_rows: Vec<Value> = match &input {
            Value::Array(devices) => devices.clone(),
            _ => input["devices"].as_array().cloned().unwrap_or_default(),
        };
        for (i, (row, line)) in rows.iter().zip(&lines).enumerate() {
            match row.get("born_day").and_then(Value::as_u64) {
                Some(day) => {
                    let exact = row.get("born_exact") == Some(&Value::Bool(true));
                    let want_label = if exact { "Born" } else { "First dated" };
                    assert_eq!(
                        line["label"],
                        want_label,
                        "vector {name:?} row {i}: born_exact {:?} must read {want_label:?}",
                        row.get("born_exact")
                    );
                    let (y, m, d) = civil_from_days(day as i64);
                    assert_eq!(
                        line["utc_date"].as_str(),
                        Some(format!("{y:04}-{m:02}-{d:02}").as_str()),
                        "vector {name:?} row {i}: born_day {day} is that UTC day"
                    );
                    if exact {
                        born += 1
                    } else {
                        first_dated += 1
                    }
                }
                None => {
                    assert!(
                        line.is_null(),
                        "vector {name:?} row {i}: no day, so no line — absence is \"cannot say\""
                    );
                    silent += 1;
                    if wire_rows.get(i).and_then(|r| r.get("born_day")).is_some() {
                        dropped += 1;
                    }
                }
            }
        }
    }
    // Each case pinned at least once, so trimming the fixture cannot quietly
    // leave a reader's branch unexercised.
    assert!(born >= 2, "no vector pins an exact birth day ({born})");
    assert!(
        first_dated >= 2,
        "no vector pins a first-dated day ({first_dated})"
    );
    assert!(silent > 0, "no vector pins a row without a birth day");
    assert!(
        dropped >= 6,
        "too few malformed or out-of-range birth days are pinned ({dropped})"
    );
}

#[test]
fn the_born_day_window_is_the_firmwares() {
    // BORN_DAY_MIN and BORN_DAY_MAX are not this crate's to choose: they are
    // the firmware's clock floor and its 32-bit clock, read here from the
    // header that decides a Canary's birth day.
    use securacv_witness_core::fleet::{BORN_DAY_MAX, BORN_DAY_MIN};
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../firmware/common/identity/birth_day.h");
    let header = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("could not read {}: {e}", path.display()));
    let constant = |name: &str| -> u64 {
        let at = header
            .find(&format!("{name} = "))
            .unwrap_or_else(|| panic!("{name} is gone from birth_day.h"));
        header[at + name.len() + 3..]
            .chars()
            .take_while(char::is_ascii_digit)
            .collect::<String>()
            .parse()
            .unwrap_or_else(|e| panic!("{name} is not a plain number in birth_day.h: {e}"))
    };
    let per_day = constant("kSecondsPerDay");
    let floor = constant("kClockFloor");
    assert_eq!(per_day, 86_400);
    assert_eq!(
        u64::from(BORN_DAY_MIN),
        floor / per_day,
        "the clock floor moved"
    );
    assert_eq!(u64::from(BORN_DAY_MAX), u64::from(u32::MAX) / per_day);
    // And the day the constant names is the date its comment says it is.
    assert_eq!(civil_from_days(i64::from(BORN_DAY_MIN)), (2025, 1, 1));
    assert_eq!(civil_from_days(i64::from(BORN_DAY_MAX)), (2106, 2, 7));
}
