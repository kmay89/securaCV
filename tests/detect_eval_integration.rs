//! Known-answer end-to-end test for the perception eval harness.
//!
//! Runs the real eval runner against the deterministic `tests/fixtures/test_detector.onnx`
//! (which always emits Person@conf0.9 at (0.1,0.2,0.4,0.4) and Vehicle@conf0.3) over a
//! self-generated single-frame dataset, and asserts the exact resulting metrics. Hermetic and
//! fast: no network, no model download, no committed image fixtures.

#![cfg(feature = "detect-eval")]

use std::path::PathBuf;

use witness_kernel::config::TractFormat;
use witness_kernel::eval::{parse_sweep_spec, run_eval, EvalConfig};

fn test_model() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/test_detector.onnx")
}

#[test]
fn known_answer_eval_on_deterministic_model() {
    let dir = tempfile::tempdir().expect("tempdir");

    // A trivial 4x4 frame; the test model ignores pixel content.
    let img = image::RgbImage::from_pixel(4, 4, image::Rgb([128, 128, 128]));
    img.save(dir.path().join("frame.png")).expect("write png");

    // Ground truth: a single person exactly where the model reports one.
    let labels = r#"{
        "frames": [
            { "image": "frame.png",
              "objects": [ { "class": "person", "x": 0.1, "y": 0.2, "w": 0.4, "h": 0.4 } ],
              "expected_size_class": "small" }
        ]
    }"#;
    std::fs::write(dir.path().join("labels.json"), labels).expect("write labels");

    let cfg = EvalConfig {
        model_path: test_model(),
        dataset_dir: dir.path().to_path_buf(),
        width: 4,
        height: 4,
        format: TractFormat::PostNms,
        iou_threshold: 0.5,
        operating_threshold: 0.5,
        sweep: parse_sweep_spec("0.1:0.9:0.2").unwrap(),
    };

    let report = run_eval(&cfg).expect("eval runs");

    assert_eq!(report.frames, 1);
    // Person is matched perfectly; the 0.3-confidence vehicle is below the 0.5 operating
    // threshold and there is no vehicle ground truth, so it is neither TP nor FP.
    assert_eq!(report.overall.precision, 1.0, "precision");
    assert_eq!(report.overall.recall, 1.0, "recall");
    assert_eq!(report.overall.witness_miss_rate, 0.0, "miss rate");
    assert!(
        (report.mean_average_precision - 1.0).abs() < 1e-6,
        "mAP should be 1.0, got {}",
        report.mean_average_precision
    );

    // Size class: person box area 0.4*0.4 = 0.16 < 0.2 → Small; expected Small → correct.
    assert_eq!(report.size_class_confusion.total, 1);
    assert_eq!(report.size_class_confusion.correct, 1);

    // Latency is recorded per frame.
    assert_eq!(report.latency.count, 1);

    // The report must serialize cleanly (no NaN/Infinity).
    serde_json::to_string(&report).expect("report serializes");
}

#[test]
fn higher_threshold_drops_recall_to_zero() {
    let dir = tempfile::tempdir().expect("tempdir");
    let img = image::RgbImage::from_pixel(4, 4, image::Rgb([10, 20, 30]));
    img.save(dir.path().join("frame.png")).expect("write png");
    let labels = r#"{ "frames": [ { "image": "frame.png",
        "objects": [ { "class": "person", "x": 0.1, "y": 0.2, "w": 0.4, "h": 0.4 } ] } ] }"#;
    std::fs::write(dir.path().join("labels.json"), labels).expect("write labels");

    // Operating threshold above the model's 0.9 person confidence → the witness misses it.
    let cfg = EvalConfig {
        model_path: test_model(),
        dataset_dir: dir.path().to_path_buf(),
        width: 4,
        height: 4,
        format: TractFormat::PostNms,
        iou_threshold: 0.5,
        operating_threshold: 0.95,
        sweep: parse_sweep_spec("0.1:0.9:0.4").unwrap(),
    };
    let report = run_eval(&cfg).expect("eval runs");
    assert_eq!(report.overall.recall, 0.0);
    assert_eq!(report.overall.witness_miss_rate, 1.0);
}
