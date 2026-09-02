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
