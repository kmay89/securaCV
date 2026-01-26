# Codex Prompt — CV Backend Abstraction

Implement pluggable CV backend abstraction for witness-kernel (Rust privacy camera system).

## Context
- `RawFrame.data` is private (no Clone, no AsRef)
- `InferenceView` can run detection but cannot export bytes
- `DetectionResult` must have NO identity data fields

## Create `src/detect/` module:

### Files to create:
````

src/detect/mod.rs           - exports
src/detect/backend.rs       - DetectorBackend trait, BackendConfig, PixelFormat
src/detect/result.rs        - DetectionResult, Detection, BoundingBox, ObjectClass, SizeClass
src/detect/registry.rs      - BackendRegistry (thread-safe using Mutex around backends because detect is &mut self)
src/detect/backends/mod.rs  - backend module exports
src/detect/backends/stub.rs - StubBackend (pixel hash motion detection)
src/detect/backends/tract.rs - TractBackend (#[cfg(feature = "backend-tract")])

````

### Key types:

```rust
// Only 3 capabilities for v1
pub enum DetectionCapability { Motion, ObjectDetection, Classification }

// Coarse classes only - NO Face, LicensePlate, PersonID
pub enum ObjectClass { Unknown, Person, Vehicle, Animal, Package }

// Normalized [0,1] coordinates
pub struct BoundingBox { x: f32, y: f32, width: f32, height: f32 }

pub struct Detection { bbox: BoundingBox, class: ObjectClass, confidence: f32, size_class: SizeClass }

// NO identity fields allowed
pub struct DetectionResult {
    motion_detected: bool,
    detections: Vec<Detection>,
    max_confidence: f32,
    size_class: SizeClass,
    latency_ms: u32,
}

// Audit boundary, NOT security boundary
pub trait DetectorBackend: Send {
    fn name(&self) -> &'static str;
    fn supports(&self, cap: DetectionCapability) -> bool; // NOT capabilities() -> &[...]
    fn detect(&mut self, pixels: &[u8], width: u32, height: u32, format: PixelFormat) -> Result<DetectionResult>;
}
````

### BackendRegistry:

* `HashMap<String, Arc<Mutex<dyn DetectorBackend>>>`
* Methods: `register()`, `get()`, `set_default()`, `default_backend()`, `list()`
* First registered becomes default

### StubBackend:

* SHA256 hash comparison for motion detection
* Document as TEST-ONLY, not production
* No feature gate (pure Rust)

### TractBackend (feature-gated):

* `#[cfg(feature = "backend-tract")]`
* Load ONNX model from config path
* Preprocess: resize, normalize [0,1], NCHW
* Postprocess: parse output, apply thresholds (model-specific; placeholder acceptable for Phase 2)

### Update frame.rs InferenceView:

```rust
pub fn run_backend(&self, backend: &mut dyn DetectorBackend) -> Result<DetectionResult>
pub fn run_detection(&self, registry: &BackendRegistry) -> Result<DetectionResult>
```

### Cargo.toml:

```toml
[features]
default = []
backend-tract = ["dep:tract-onnx"]

[dependencies]
tract-onnx = { version = "0.21", optional = true }
```

## Tests required:

* Stub detects motion on pixel change
* Stub no motion on identical frames
* Registry register/get/default
* BoundingBox::area()
* SizeClass::from_area()
* DetectionResult::from_detections()

## Documentation must state:

* DetectorBackend is an AUDIT BOUNDARY, not security boundary
* Backends run with full process privileges
* Each backend must be manually audited
* True isolation requires WASM sandboxing (not implemented)

## FORBIDDEN - do not implement:

* Face recognition/embedding
* License plate OCR
* Person re-identification
* Demographic estimation
* Any identity-linked output fields
* Network I/O in backends
* Disk I/O in backends

