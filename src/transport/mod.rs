//! Transport layer abstractions for MQTT bridges.
//!
//! This module provides TLS backend selection for MQTT connections,
//! supporting both classic TLS (rustls) and hybrid post-quantum TLS.
//! It also includes Frigate MQTT event parsing utilities.

pub mod frigate;
mod tls;

pub use frigate::{
    map_label_to_event_type, parse_frigate_event, parse_review_event, sanitize_zone_name,
    FrigateEventData, FrigateEventWrapper, FrigateReview, FrigateReviewData, ParsedFrigateEvent,
};
pub use tls::{
    parse_mqtt_endpoint, validate_loopback_addr, MqttEndpoint, TlsBackend, TlsConfig, TlsMaterials,
};
