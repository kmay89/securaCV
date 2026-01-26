# SecuraCV: CV Backend Abstraction — Implementation Plan

## Context

**Repository:** https://github.com/kmay89/securaCV

Privacy-preserving video event system in Rust. Outputs semantic events without exposing raw video or identity data.

### Existing Architecture
- `RawFrame` — opaque container, `data` field is private
- `InferenceView` — restricted view, cannot export bytes
- `StubDetector` — current hardcoded detector
- Hash-chained event log with `log_verify`

---

## Critical Design Notes

### The trait boundary is an AUDIT BOUNDARY, not a security boundary

**It provides:**
- Clear API contract
- Single point to audit each backend
- Type-level prevention of identity data in `DetectionResult`

**It does NOT provide:**
- Memory isolation
- Network/disk access denial
- Protection against malicious backends

**Each backend must be manually audited.** True enforcement requires WASM sandboxing (not implemented).

---

## File Structure

````

src/detect/
├── mod.rs              # Module exports
├── backend.rs          # DetectorBackend trait, config types
├── result.rs           # DetectionResult, Detection, BoundingBox
├── registry.rs         # BackendRegistry
└── backends/
├── mod.rs          # Feature-gated exports
├── stub.rs         # StubBackend (test-only, always available)
└── tract.rs        # TractBackend (feature: backend-tract)

````

---

## Type Definitions

### `src/detect/backend.rs`

```rust
use std::path::PathBuf;
use anyhow::Result;
use super::result::DetectionResult;

/// Capabilities a backend can provide. Minimal for v1.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum DetectionCapability {
    Motion,
    ObjectDetection,
    Classification,
}

/// Pixel format
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum PixelFormat {
    #[default]
    Rgb8,
    Bgr8,
    Gray8,
}

/// Backend configuration
#[derive(Debug, Clone, Default)]
pub struct BackendConfig {
    pub model_path: Option<PathBuf>,
    pub confidence_threshold: f32,
    pub nms_threshold: f32,
    pub max_detections: usize,
}

impl BackendConfig {
    pub fn new() -> Self {
        Self {
            confidence_threshold: 0.5,
            nms_threshold: 0.45,
            max_detections: 50,
            ..Default::default()
        }
    }

    pub fn with_model(mut self, path: impl Into<PathBuf>) -> Self {
        self.model_path = Some(path.into());
        self
    }
}

/// Core detector backend trait.
///
/// # Audit Requirements
///
/// This is an AUDIT BOUNDARY, not a security boundary.
/// Each implementation MUST be audited to verify:
/// - No pixel data stored beyond `detect()` call
/// - No network operations
/// - No disk writes
/// - No identity-linked computations
///
/// Backends run with full process privileges until WASM sandboxing is implemented.
pub trait DetectorBackend: Send {
    /// Backend identifier
    fn name(&self) -> &'static str;

    /// Check if this backend supports a capability
    fn supports(&self, capability: DetectionCapability) -> bool;

    /// Run detection on a frame.
    ///
    /// Takes `&mut self` because real backends need internal buffers/state.
    fn detect(
        &mut self,
        pixels: &[u8],
        width: u32,
        height: u32,
        format: PixelFormat,
    ) -> Result<DetectionResult>;

    /// Optional: warm up model
    fn warm_up(&mut self) -> Result<()> {
        Ok(())
    }
}
````

### `src/detect/registry.rs`

```rust
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use anyhow::{anyhow, Result};
use super::backend::DetectorBackend;

/// Thread-safe registry of detector backends.
/// 
/// Backends are wrapped in Mutex because DetectorBackend::detect takes &mut self.
pub struct BackendRegistry {
    backends: HashMap<String, Arc<Mutex<dyn DetectorBackend>>>,
    default_name: Option<String>,
}

impl BackendRegistry {
    pub fn new() -> Self {
        Self {
            backends: HashMap::new(),
            default_name: None,
        }
    }

    /// Register a backend. First registered becomes default.
    pub fn register<B: DetectorBackend + 'static>(&mut self, backend: B) {
        let name = backend.name().to_string();
        if self.default_name.is_none() {
            self.default_name = Some(name.clone());
        }
        self.backends.insert(name, Arc::new(Mutex::new(backend)));
    }

    /// Set default backend by name
    pub fn set_default(&mut self, name: &str) -> Result<()> {
        if !self.backends.contains_key(name) {
            return Err(anyhow!("backend '{}' not registered", name));
        }
        self.default_name = Some(name.to_string());
        Ok(())
    }

    /// Get backend by name
    pub fn get(&self, name: &str) -> Option<Arc<Mutex<dyn DetectorBackend>>> {
        self.backends.get(name).cloned()
    }

    /// Get default backend
    pub fn default_backend(&self) -> Option<Arc<Mutex<dyn DetectorBackend>>> {
        self.default_name.as_ref().and_then(|n| self.get(n))
    }

    /// List registered backends
    pub fn list(&self) -> Vec<String> {
        self.backends.keys().cloned().collect()
    }
}

impl Default for BackendRegistry {
    fn default() -> Self {
        Self::new()
    }
}
```

### InferenceView Integration

```rust
use crate::detect::{BackendRegistry, DetectorBackend, DetectionResult};

impl<'a> InferenceView<'a> {
    /// Run detection with a specific backend
    pub fn run_backend(&self, backend: &mut dyn DetectorBackend) -> Result<DetectionResult> {
        backend.detect(
            &self.frame.data,
            self.frame.width,
            self.frame.height,
            self.frame.format,
        )
    }

    /// Run detection with registry's default backend
    pub fn run_detection(&self, registry: &BackendRegistry) -> Result<DetectionResult> {
        let backend = registry.default_backend()
            .ok_or_else(|| anyhow::anyhow!("no detector backend configured"))?;
        let mut guard = backend.lock().map_err(|_| anyhow::anyhow!("backend lock poisoned"))?;
        self.run_backend(&mut *guard)
    }
}
```

---

## Cargo.toml

```toml
[features]
default = []
backend-tract = ["dep:tract-onnx"]

[dependencies]
tract-onnx = { version = "0.21", optional = true }
```

---

## Tests

* Stub detects motion on pixel change
* Stub no motion on identical frames
* stub.supports(Motion) == true
* stub.supports(ObjectDetection) == false
* Registry register/get/default
* SizeClass::from_area thresholds

````
