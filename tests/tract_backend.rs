#![cfg(feature = "backend-tract")]

use std::path::PathBuf;

use witness_kernel::detect::{
    BackendRegistry, DetectionCapability, DetectorBackend, ObjectClass, SizeClass, TractBackend,
};

fn fixture_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/test_detector.onnx")
}

fn make_rgb_frame(width: u32, height: u32) -> Vec<u8> {
    vec![128u8; (width as usize) * (height as usize) * 3]
}

// ── TractBackend unit tests ─────────────────────────────────────────────

#[test]
fn loads_model_and_reports_name() {
    let backend = TractBackend::new(fixture_path(), 4, 4).expect("model should load");
    assert_eq!(backend.name(), "tract");
}

#[test]
fn supports_object_detection_and_classification() {
    let backend = TractBackend::new(fixture_path(), 4, 4).unwrap();
    assert!(backend.supports(DetectionCapability::ObjectDetection));
    assert!(backend.supports(DetectionCapability::Classification));
    assert!(!backend.supports(DetectionCapability::Motion));
}

#[test]
fn detects_person_above_threshold() {
    let mut backend = TractBackend::new(fixture_path(), 4, 4).unwrap();
    let pixels = make_rgb_frame(4, 4);

    let result = backend
        .detect(&pixels, 4, 4)
        .expect("inference should succeed");

    assert!(result.motion_detected);
    assert_eq!(
        result.detections.len(),
        1,
        "only person (conf 0.9) should pass threshold 0.5"
    );

    let det = &result.detections[0];
    assert_eq!(det.class, ObjectClass::Person);
    assert!((det.confidence - 0.9).abs() < 1e-4);
    assert!((det.x - 0.1).abs() < 1e-4);
    assert!((det.y - 0.2).abs() < 1e-4);
    assert!((det.w - 0.4).abs() < 1e-4);
    assert!((det.h - 0.4).abs() < 1e-4);
}

#[test]
fn low_threshold_returns_both_detections() {
    let mut backend = TractBackend::new(fixture_path(), 4, 4)
        .unwrap()
        .with_threshold(0.1);
    let pixels = make_rgb_frame(4, 4);

    let result = backend.detect(&pixels, 4, 4).unwrap();

    assert_eq!(result.detections.len(), 2);
    assert_eq!(result.detections[0].class, ObjectClass::Person);
    assert_eq!(result.detections[1].class, ObjectClass::Vehicle);
}

#[test]
fn high_threshold_returns_no_detections() {
    let mut backend = TractBackend::new(fixture_path(), 4, 4)
        .unwrap()
        .with_threshold(0.95);
    let pixels = make_rgb_frame(4, 4);

    let result = backend.detect(&pixels, 4, 4).unwrap();

    assert!(!result.motion_detected);
    assert!(result.detections.is_empty());
}

#[test]
fn rejects_wrong_frame_size() {
    let mut backend = TractBackend::new(fixture_path(), 4, 4).unwrap();
    let pixels = make_rgb_frame(8, 8);

    let err = backend.detect(&pixels, 8, 8);
    assert!(err.is_err());
    assert!(err.unwrap_err().to_string().contains("does not match"));
}

#[test]
fn rejects_wrong_pixel_count() {
    let mut backend = TractBackend::new(fixture_path(), 4, 4).unwrap();
    let pixels = vec![0u8; 10];

    let err = backend.detect(&pixels, 4, 4);
    assert!(err.is_err());
    assert!(err.unwrap_err().to_string().contains("RGB bytes"));
}

#[test]
fn size_class_is_large_for_big_box() {
    let mut backend = TractBackend::new(fixture_path(), 4, 4).unwrap();
    let pixels = make_rgb_frame(4, 4);

    let result = backend.detect(&pixels, 4, 4).unwrap();

    // Person box: 0.4 * 0.4 = 0.16, below LARGE_AREA_THRESHOLD (0.2)
    assert_eq!(result.size_class, SizeClass::Small);
}

#[test]
fn model_file_not_found() {
    let result = TractBackend::new("nonexistent.onnx", 4, 4);
    assert!(result.is_err());
    let msg = format!("{}", result.err().unwrap());
    assert!(msg.contains("failed to load"), "got: {msg}");
}

// ── Registry integration tests ──────────────────────────────────────────

#[test]
fn registry_routes_to_tract_for_object_detection() {
    let mut registry = BackendRegistry::new();
    let backend = TractBackend::new(fixture_path(), 4, 4).unwrap();
    registry.register(backend);

    let pixels = make_rgb_frame(4, 4);
    let result = registry
        .detect_with_capability(DetectionCapability::ObjectDetection, &pixels, 4, 4)
        .expect("registry should route to tract backend");

    assert_eq!(result.detections.len(), 1);
    assert_eq!(result.detections[0].class, ObjectClass::Person);
}

#[test]
fn registry_with_stub_and_tract_selects_by_capability() {
    use witness_kernel::detect::StubBackend;

    let mut registry = BackendRegistry::new();
    registry.register(StubBackend::new());
    registry.register(TractBackend::new(fixture_path(), 4, 4).unwrap());

    let pixels = make_rgb_frame(4, 4);

    let motion_result = registry
        .detect_with_capability(DetectionCapability::Motion, &pixels, 4, 4)
        .expect("stub should handle motion");
    assert!(motion_result.detections.is_empty());

    let obj_result = registry
        .detect_with_capability(DetectionCapability::ObjectDetection, &pixels, 4, 4)
        .expect("tract should handle object detection");
    assert_eq!(obj_result.detections[0].class, ObjectClass::Person);
}
