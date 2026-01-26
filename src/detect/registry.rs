use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use anyhow::{anyhow, Result};

use crate::detect::result::DetectionResult;

use super::backend::{DetectionCapability, DetectorBackend};

/// Thread-safe registry of detector backends.
///
/// Backends are wrapped in `Mutex` because `DetectorBackend::detect` takes `&mut self`.
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

    /// Register a backend. The first registered backend becomes the default.
    pub fn register<B: DetectorBackend + 'static>(&mut self, backend: B) {
        let name = backend.name().to_string();
        if self.default_name.is_none() {
            self.default_name = Some(name.clone());
        }
        self.backends.insert(name, Arc::new(Mutex::new(backend)));
    }

    /// Set default backend by name.
    pub fn set_default(&mut self, name: &str) -> Result<()> {
        if !self.backends.contains_key(name) {
            return Err(anyhow!("backend '{}' not registered", name));
        }
        self.default_name = Some(name.to_string());
        Ok(())
    }

    /// Get backend by name.
    pub fn get(&self, name: &str) -> Option<Arc<Mutex<dyn DetectorBackend>>> {
        self.backends.get(name).cloned()
    }

    /// Get default backend.
    pub fn default_backend(&self) -> Option<Arc<Mutex<dyn DetectorBackend>>> {
        self.default_name.as_ref().and_then(|name| self.get(name))
    }

    /// List registered backends.
    pub fn list(&self) -> Vec<String> {
        self.backends.keys().cloned().collect()
    }

    /// Select a backend that supports the requested capability.
    ///
    /// Prefers the default backend when it supports the capability.
    pub fn backend_for_capability(
        &self,
        capability: DetectionCapability,
    ) -> Result<Arc<Mutex<dyn DetectorBackend>>> {
        if let Some(default_backend) = self.default_backend() {
            let supports = {
                let guard = default_backend
                    .lock()
                    .map_err(|_| anyhow!("default backend lock poisoned"))?;
                guard.supports(capability)
            };
            if supports {
                return Ok(default_backend);
            }
        }

        for backend in self.backends.values() {
            let supports = {
                let guard = backend
                    .lock()
                    .map_err(|_| anyhow!("backend lock poisoned"))?;
                guard.supports(capability)
            };
            if supports {
                return Ok(backend.clone());
            }
        }

        Err(anyhow!(
            "no registered backend supports capability {:?}",
            capability
        ))
    }

    /// Run detection using a backend that supports the requested capability.
    pub fn detect_with_capability(
        &self,
        capability: DetectionCapability,
        pixels: &[u8],
        width: u32,
        height: u32,
    ) -> Result<DetectionResult> {
        let backend = self.backend_for_capability(capability)?;
        let mut guard = backend
            .lock()
            .map_err(|_| anyhow!("backend lock poisoned"))?;
        guard.detect(pixels, width, height)
    }
}

impl Default for BackendRegistry {
    fn default() -> Self {
        Self::new()
    }
}
