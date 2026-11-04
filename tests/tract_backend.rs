#![cfg(feature = "backend-tract")]

use std::path::{Path, PathBuf};

use witness_kernel::config::{
    DetectBackendPreference, DetectSettings, TractFormat, TractModelFile,
};
use witness_kernel::detect::{
    BackendRegistry, DetectionCapability, DetectorBackend, ObjectClass, SizeClass, TractBackend,
};

fn fixture_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/test_detector.onnx")
}

/// sha256 of the committed 236-byte fixture: the pin these tests hold a stand-in default to.
const FIXTURE_SHA256: &str = "eb5a1733bbbd0eabd14a286a8a156fd9b7596e1cb34778b64fc2e8bba70cc5dc";

/// Byte 116 is the high byte of the person row's confidence (0.9 = 0x3f666666). XOR 1 makes it
/// 0x3e666666 = 0.225: still a well-formed model that loads, now with no detection at the
/// default 0.5 threshold, so a test can tell which bytes a backend was built from.
const FLIP_AT: usize = 116;

fn fixture_bytes() -> Vec<u8> {
    std::fs::read(fixture_path()).expect("read the fixture")
}

fn flipped_bytes() -> Vec<u8> {
    let mut bytes = fixture_bytes();
    assert_eq!(
        bytes[FLIP_AT], 0x3f,
        "the fixture's layout moved; re-pick FLIP_AT"
    );
    bytes[FLIP_AT] ^= 0x01;
    bytes
}

/// witnessd's detect settings with nothing naming a model (detect.tract_model and
/// WITNESS_TRACT_MODEL both unset), or naming `tract_model`.
fn detect_settings(tract_model: Option<PathBuf>) -> DetectSettings {
    DetectSettings {
        backend: DetectBackendPreference::Tract,
        tract_model,
        tract_format: TractFormat::PostNms,
        confidence_threshold: 0.5,
    }
}

/// The fixture is a post-NMS model with a 4×4 input.
fn build(model: &TractModelFile) -> anyhow::Result<TractBackend> {
    TractBackend::from_model_file(model, TractFormat::PostNms, || Ok((4, 4)))
}

fn person_count(backend: &mut TractBackend) -> usize {
    let result = backend
        .detect(&make_rgb_frame(4, 4), 4, 4)
        .expect("inference should succeed");
    result
        .detections
        .iter()
        .filter(|d| d.class == ObjectClass::Person)
        .count()
}

fn sha256_hex(bytes: &[u8]) -> String {
    use sha2::Digest;
    hex::encode(sha2::Sha256::digest(bytes))
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

// ── Reader constructors ─────────────────────────────────────────────────

#[test]
fn reader_constructor_parses_the_bytes_it_is_given() {
    let bytes = fixture_bytes();
    let mut backend = TractBackend::new_from_reader(&mut bytes.as_slice(), None, 4, 4)
        .expect("model should load");
    assert_eq!(person_count(&mut backend), 1);
    let mut flipped =
        TractBackend::new_from_reader(&mut flipped_bytes().as_slice(), None, 4, 4).expect("loads");
    assert_eq!(person_count(&mut flipped), 0, "the flipped person is 0.225");
    let err = TractBackend::new_from_reader(&mut &b"not a model"[..], None, 4, 4)
        .err()
        .expect("garbage is refused");
    assert!(
        format!("{err:#}").contains("failed to load"),
        "got: {err:#}"
    );
}

// ── The default model is held to its pin (sweep A102) ──────────────────

#[test]
fn fixture_hash_is_the_pin_these_tests_use() {
    assert_eq!(fixture_bytes().len(), 236);
    assert_eq!(sha256_hex(&fixture_bytes()), FIXTURE_SHA256);
}

#[test]
fn the_pinned_default_loads_from_the_bytes_it_hashed() {
    let dir = tempfile::tempdir().unwrap();
    let default = dir.path().join("tinyyolov2-8.onnx");
    std::fs::write(&default, fixture_bytes()).unwrap();
    let model = detect_settings(None)
        .tract_model_file_with_default(&default, FIXTURE_SHA256)
        .expect("the pinned bytes load");
    assert!(model.is_pinned_default());
    assert_eq!(model.bytes(), fixture_bytes().as_slice());
    let mut backend = build(&model).expect("built from the pinned bytes");
    assert_eq!(person_count(&mut backend), 1);
}

#[test]
fn a_one_byte_flip_at_the_default_path_is_refused() {
    let dir = tempfile::tempdir().unwrap();
    let default = dir.path().join("tinyyolov2-8.onnx");
    std::fs::write(&default, flipped_bytes()).unwrap();
    // Loaded by path, as witnessd loaded the default before the pin, the flipped file runs:
    // it is a model, just not the pinned one.
    let mut unpinned = TractBackend::new(&default, 4, 4).expect("the flipped model loads");
    assert_eq!(person_count(&mut unpinned), 0);
    // With nothing naming a model, the default is refused before tract sees it.
    let err = detect_settings(None)
        .tract_model_file_with_default(&default, FIXTURE_SHA256)
        .expect_err("not the pinned bytes");
    let msg = err.to_string();
    assert!(msg.contains(&sha256_hex(&flipped_bytes())), "{msg}");
    assert!(msg.contains("scripts/fetch_detection_model.sh"), "{msg}");
    assert!(msg.contains("detect.tract_model"), "{msg}");
}

#[test]
fn a_file_swapped_after_the_check_does_not_reach_the_backend() {
    let dir = tempfile::tempdir().unwrap();
    let default = dir.path().join("tinyyolov2-8.onnx");
    std::fs::write(&default, fixture_bytes()).unwrap();
    let model = detect_settings(None)
        .tract_model_file_with_default(&default, FIXTURE_SHA256)
        .expect("checked");
    // Replace the file between the check and the build; loading by path would now get it.
    std::fs::write(&default, flipped_bytes()).unwrap();
    let mut reopened = TractBackend::new(&default, 4, 4).unwrap();
    assert_eq!(
        person_count(&mut reopened),
        0,
        "the file on disk is the swapped one"
    );
    // The backend is built from the bytes that were hashed: the original model answers.
    let mut backend = build(&model).expect("built from the checked bytes");
    assert_eq!(person_count(&mut backend), 1);
    // Removing the file changes nothing either: the path is never opened again.
    std::fs::remove_file(&default).unwrap();
    let mut backend = build(&model).expect("no second open");
    assert_eq!(person_count(&mut backend), 1);
}

#[cfg(unix)]
#[test]
fn a_symlink_at_the_default_path_is_held_to_its_targets_bytes() {
    let dir = tempfile::tempdir().unwrap();
    let flipped = dir.path().join("flipped.onnx");
    std::fs::write(&flipped, flipped_bytes()).unwrap();
    let default = dir.path().join("tinyyolov2-8.onnx");
    std::os::unix::fs::symlink(&flipped, &default).unwrap();
    assert!(detect_settings(None)
        .tract_model_file_with_default(&default, FIXTURE_SHA256)
        .is_err());
    std::fs::remove_file(&default).unwrap();
    std::os::unix::fs::symlink(fixture_path(), &default).unwrap();
    let model = detect_settings(None)
        .tract_model_file_with_default(&default, FIXTURE_SHA256)
        .expect("a link to the pinned bytes");
    let mut backend = build(&model).unwrap();
    assert_eq!(person_count(&mut backend), 1);
}

#[test]
fn a_configured_model_with_another_hash_loads_as_given_and_reports_it() {
    let dir = tempfile::tempdir().unwrap();
    let default = dir.path().join("tinyyolov2-8.onnx");
    let custom = dir.path().join("custom.onnx");
    std::fs::write(&custom, flipped_bytes()).unwrap();
    let model = detect_settings(Some(custom.clone()))
        .tract_model_file_with_default(&default, FIXTURE_SHA256)
        .expect("a configured model is the operator's choice");
    assert!(!model.is_pinned_default());
    assert_eq!(
        model.bytes(),
        flipped_bytes().as_slice(),
        "configured: read once, built from these bytes"
    );
    assert_eq!(model.sha256(), sha256_hex(&flipped_bytes()));
    assert_ne!(model.sha256(), FIXTURE_SHA256);
    let line = model.startup_line();
    assert!(line.contains(&custom.display().to_string()), "{line}");
    assert!(line.contains(model.sha256()), "{line}");
    assert!(line.contains("loaded as given"), "{line}");
    let mut backend = build(&model).expect("loads as given");
    assert_eq!(
        person_count(&mut backend),
        0,
        "it is the configured (flipped) model"
    );
}

#[test]
fn a_configured_model_swapped_after_the_read_does_not_reach_the_backend() {
    // The startup line logs a configured model's sha256; the backend must run the bytes that
    // hash is of, not whatever the path holds when tract would reopen it.
    let dir = tempfile::tempdir().unwrap();
    let custom = dir.path().join("custom.onnx");
    std::fs::write(&custom, fixture_bytes()).unwrap();
    let model = detect_settings(Some(custom.clone()))
        .tract_model_file_with_default(Path::new("unused"), FIXTURE_SHA256)
        .expect("a configured model is read");
    assert_eq!(model.sha256(), FIXTURE_SHA256);
    std::fs::write(&custom, flipped_bytes()).unwrap();
    let mut reopened = TractBackend::new(&custom, 4, 4).unwrap();
    assert_eq!(
        person_count(&mut reopened),
        0,
        "the file on disk is the swapped one"
    );
    let mut backend = build(&model).expect("built from the bytes hashed");
    assert_eq!(
        person_count(&mut backend),
        1,
        "the model that runs is the one whose sha256 the startup line logs"
    );
    std::fs::remove_file(&custom).unwrap();
    let mut backend = build(&model).expect("no second open of a configured model either");
    assert_eq!(person_count(&mut backend), 1);
}

// ── A model that keeps its weights beside it (ONNX external data) ───────
//
// A model loaded by path reads such weights from its own directory, and a configured model
// built from bytes must still; the pinned default must not, since the pin covers the model
// file alone. The model is the fixture's graph with its Constant's tensor moved to a file,
// encoded here by hand (ONNX is protobuf: a field is a tag varint, then a varint value or a
// length and bytes), so no second fixture is committed.

const WEIGHTS_FILE: &str = "test_detector.weights";

/// The fixture's two detections, the 1×2×6 float32 tensor its Constant node emits.
fn weights() -> Vec<u8> {
    [
        0.1f32, 0.2, 0.5, 0.6, 0.9, 0.0, // person, confidence 0.9
        0.3, 0.4, 0.7, 0.8, 0.3, 1.0, // vehicle, confidence 0.3
    ]
    .iter()
    .flat_map(|v| v.to_le_bytes())
    .collect()
}

fn varint(mut value: u64, out: &mut Vec<u8>) {
    loop {
        let byte = (value & 0x7f) as u8;
        value >>= 7;
        if value == 0 {
            out.push(byte);
            return;
        }
        out.push(byte | 0x80);
    }
}

fn uint_field(field: u64, value: u64, out: &mut Vec<u8>) {
    varint(field << 3, out);
    varint(value, out);
}

fn bytes_field(field: u64, value: &[u8], out: &mut Vec<u8>) {
    varint((field << 3) | 2, out);
    varint(value.len() as u64, out);
    out.extend_from_slice(value);
}

fn message(fill: impl FnOnce(&mut Vec<u8>)) -> Vec<u8> {
    let mut out = Vec::new();
    fill(&mut out);
    out
}

/// A float tensor `ValueInfoProto` (name = 1, type = 2; `TypeProto.tensor_type` = 1 holds
/// elem_type = 1 (FLOAT is 1) and shape = 2, a `dim` = 1 per axis with dim_value = 1).
fn value_info(name: &str, dims: &[u64]) -> Vec<u8> {
    message(|info| {
        bytes_field(1, name.as_bytes(), info);
        let tensor_type = message(|tensor| {
            uint_field(1, 1, tensor);
            let shape = message(|shape| {
                for &d in dims {
                    bytes_field(1, &message(|dim| uint_field(1, d, dim)), shape);
                }
            });
            bytes_field(2, &shape, tensor);
        });
        bytes_field(2, &message(|ty| bytes_field(1, &tensor_type, ty)), info);
    })
}

/// The fixture's model with its detections in `WEIGHTS_FILE` beside it instead of in the file.
fn external_data_model() -> Vec<u8> {
    // TensorProto: dims = 1, data_type = 2 (FLOAT is 1), name = 8, external_data = 13 (a
    // StringStringEntryProto: key = 1, value = 2), data_location = 14 (EXTERNAL is 1).
    let tensor = message(|t| {
        for d in [1, 2, 6] {
            uint_field(1, d, t);
        }
        uint_field(2, 1, t);
        bytes_field(8, b"const_detections", t);
        let location = message(|entry| {
            bytes_field(1, b"location", entry);
            bytes_field(2, WEIGHTS_FILE.as_bytes(), entry);
        });
        bytes_field(13, &location, t);
        uint_field(14, 1, t);
    });
    // NodeProto: input = 1, output = 2, op_type = 4, attribute = 5 (an AttributeProto: name =
    // 1, t = 5, type = 20, TENSOR being 4).
    let shape_node = message(|n| {
        bytes_field(1, b"input", n);
        bytes_field(2, b"input_shape", n);
        bytes_field(4, b"Shape", n);
    });
    let constant_node = message(|n| {
        bytes_field(2, b"detections", n);
        bytes_field(4, b"Constant", n);
        let value = message(|attr| {
            bytes_field(1, b"value", attr);
            bytes_field(5, &tensor, attr);
            uint_field(20, 4, attr);
        });
        bytes_field(5, &value, n);
    });
    // GraphProto: node = 1, name = 2, input = 11, output = 12. ModelProto: ir_version = 1,
    // graph = 7, opset_import = 8 (an OperatorSetIdProto: domain = 1, version = 2).
    let graph = message(|g| {
        bytes_field(1, &shape_node, g);
        bytes_field(1, &constant_node, g);
        bytes_field(2, b"test_detector_external", g);
        bytes_field(11, &value_info("input", &[1, 3, 4, 4]), g);
        bytes_field(12, &value_info("detections", &[1, 2, 6]), g);
    });
    message(|m| {
        uint_field(1, 7, m);
        bytes_field(7, &graph, m);
        let opset = message(|o| {
            bytes_field(1, b"", o);
            uint_field(2, 13, o);
        });
        bytes_field(8, &opset, m);
    })
}

/// Writes the external-data model and its weights into `dir`; returns the model's path.
fn write_external_data_model(dir: &Path) -> PathBuf {
    let model = dir.join("external.onnx");
    std::fs::write(&model, external_data_model()).unwrap();
    std::fs::write(dir.join(WEIGHTS_FILE), weights()).unwrap();
    model
}

#[test]
fn the_external_data_model_is_the_fixture_with_its_weights_beside_it() {
    // By path, tract reads the weights from the model's directory: the same detections as the
    // committed fixture. From bytes with no model directory, it refuses rather than guesses;
    // with the directory, it reads them.
    let dir = tempfile::tempdir().unwrap();
    let model_path = write_external_data_model(dir.path());
    let mut by_path = TractBackend::new(&model_path, 4, 4)
        .expect("tract reads the weights beside the model")
        .with_threshold(0.1);
    let result = by_path.detect(&make_rgb_frame(4, 4), 4, 4).unwrap();
    assert_eq!(result.detections.len(), 2);
    assert_eq!(result.detections[0].class, ObjectClass::Person);
    assert!((result.detections[0].confidence - 0.9).abs() < 1e-4);
    assert_eq!(result.detections[1].class, ObjectClass::Vehicle);
    let err = TractBackend::new_from_reader(&mut external_data_model().as_slice(), None, 4, 4)
        .err()
        .expect("no model directory, so no external data");
    assert!(format!("{err:#}").contains("external data"), "{err:#}");
    let mut with_dir = TractBackend::new_from_reader(
        &mut external_data_model().as_slice(),
        Some(dir.path()),
        4,
        4,
    )
    .expect("the weights are read from the directory given");
    assert_eq!(person_count(&mut with_dir), 1);
}

#[test]
fn a_configured_model_keeps_the_weights_beside_it() {
    let dir = tempfile::tempdir().unwrap();
    let model_path = write_external_data_model(dir.path());
    let model = detect_settings(Some(model_path.clone()))
        .tract_model_file_with_default(Path::new("unused"), FIXTURE_SHA256)
        .expect("read");
    assert_eq!(
        model.sha256(),
        sha256_hex(&external_data_model()),
        "the sha256 is of the model file alone; the weights beside it are not hashed"
    );
    let mut backend =
        build(&model).expect("a configured model reads its weights from its own directory");
    assert_eq!(person_count(&mut backend), 1);
}

#[test]
fn the_pinned_default_does_not_read_weights_beside_it() {
    let dir = tempfile::tempdir().unwrap();
    let default = write_external_data_model(dir.path());
    let pin = sha256_hex(&external_data_model());
    let model = detect_settings(None)
        .tract_model_file_with_default(&default, &pin)
        .expect("the model file matches its pin");
    assert!(model.is_pinned_default());
    let err = build(&model)
        .err()
        .expect("the pin covers the model file only: weights outside it are refused, not read");
    assert!(format!("{err:#}").contains("external data"), "{err:#}");
}

#[test]
fn a_yolov2_build_never_asks_for_frame_dimensions() {
    // tiny-YOLOv2's input is fixed, so witnessd does not need ingest dimensions for it; the
    // fixture is not a 416×416 model, so the build fails, but only after skipping `input`.
    let model = detect_settings(Some(fixture_path()))
        .tract_model_file_with_default(Path::new("unused"), FIXTURE_SHA256)
        .unwrap();
    let mut asked = false;
    let _ = TractBackend::from_model_file(&model, TractFormat::Yolov2, || {
        asked = true;
        Ok((4, 4))
    });
    assert!(!asked);
}
