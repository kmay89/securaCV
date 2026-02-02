//! Transport layer abstractions for MQTT bridges.
//!
//! This module provides TLS backend selection for MQTT connections,
//! supporting both classic TLS (rustls) and hybrid post-quantum TLS.

mod tls;

pub use tls::{
    parse_mqtt_endpoint, validate_loopback_addr, MqttEndpoint, TlsBackend, TlsConfig, TlsMaterials,
};
