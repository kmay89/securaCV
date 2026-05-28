//! A thread-safe registry of sensor adapters.
//!
//! Modeled on `detect::registry::BackendRegistry`. Adapters are wrapped in `Mutex` because
//! [`SensorAdapter::poll`](crate::adapter::SensorAdapter::poll) takes `&mut self`. Registration
//! order is preserved so the host polls adapters deterministically.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use crate::adapter::SensorAdapter;

/// Registry of named sensor adapters.
#[derive(Default)]
pub struct AdapterRegistry {
    adapters: HashMap<String, Arc<Mutex<dyn SensorAdapter>>>,
    order: Vec<String>,
}

impl AdapterRegistry {
    pub fn new() -> Self {
        Self {
            adapters: HashMap::new(),
            order: Vec::new(),
        }
    }

    /// Register an adapter by its [`name`](crate::adapter::SensorAdapter::name).
    /// Re-registering the same name replaces the adapter but keeps its position in poll order.
    pub fn register<A: SensorAdapter + 'static>(&mut self, adapter: A) {
        let name = adapter.name().to_string();
        if !self.adapters.contains_key(&name) {
            self.order.push(name.clone());
        }
        self.adapters.insert(name, Arc::new(Mutex::new(adapter)));
    }

    /// Get an adapter handle by name.
    pub fn get(&self, name: &str) -> Option<Arc<Mutex<dyn SensorAdapter>>> {
        self.adapters.get(name).cloned()
    }

    /// List adapter names in registration order.
    pub fn list(&self) -> Vec<String> {
        self.order.clone()
    }

    pub fn len(&self) -> usize {
        self.adapters.len()
    }

    pub fn is_empty(&self) -> bool {
        self.adapters.is_empty()
    }
}
