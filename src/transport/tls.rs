//! TLS backend selection for MQTT bridges.
//!
//! Provides `TlsBackend` enum for selecting between classic and hybrid PQ TLS.
//! Compatible with Home Assistant MQTT integration patterns:
//! - CA verification: custom CA path or system roots
//! - Client certificates: mutual TLS support
//! - Insecure mode: disabled by default (HA supports it, we don't)
//!
//! # Example
//!
//! ```ignore
//! let config = TlsConfig {
//!     backend: TlsBackend::Classic,
//!     materials: TlsMaterials::default(),
//! };
//! let transport = config.build_transport(&endpoint)?;
//! ```

use anyhow::{anyhow, Context, Result};
use rumqttc::Transport;
use std::path::PathBuf;
use std::str::FromStr;

/// TLS backend selection for MQTT connections.
///
/// Home Assistant uses classic TLS with rustls/OpenSSL. Our bridges maintain
/// compatibility while optionally supporting hybrid post-quantum key exchange.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TlsBackend {
    /// Classic TLS using rustls with X25519 key exchange.
    /// Compatible with all standard MQTT brokers including Home Assistant's Mosquitto.
    #[default]
    Classic,

    /// Hybrid post-quantum TLS using X25519Kyber768Draft00.
    /// Requires `pqc-tls` feature and a PQ-capable MQTT broker.
    /// Falls back to classic if the broker doesn't support PQ.
    HybridPq,
}

impl FromStr for TlsBackend {
    type Err = anyhow::Error;

    fn from_str(s: &str) -> Result<Self> {
        match s.to_lowercase().as_str() {
            "classic" | "rustls" | "default" => Ok(Self::Classic),
            "hybrid_pq" | "hybridpq" | "pq" | "hybrid" => Ok(Self::HybridPq),
            other => Err(anyhow!(
                "unknown TLS backend '{}': expected 'classic' or 'hybrid_pq'",
                other
            )),
        }
    }
}

impl TlsBackend {
    /// Check if this backend requires the pqc-tls feature.
    pub fn requires_pqc_feature(&self) -> bool {
        matches!(self, Self::HybridPq)
    }

    /// Validate that required features are enabled at runtime.
    /// Returns an error if hybrid_pq is requested but pqc-tls feature is not compiled in.
    pub fn validate_feature_support(&self) -> Result<()> {
        if self.requires_pqc_feature() && !cfg!(feature = "pqc-tls") {
            return Err(anyhow!(
                "TLS backend 'hybrid_pq' requires the 'pqc-tls' feature.\n\
                 Recompile with: cargo build --features pqc-tls\n\
                 Or use '--mqtt-tls-backend classic' for standard TLS."
            ));
        }
        Ok(())
    }
}

impl std::fmt::Display for TlsBackend {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Classic => write!(f, "classic"),
            Self::HybridPq => write!(f, "hybrid_pq"),
        }
    }
}

/// TLS certificate materials for MQTT connections.
///
/// Mirrors Home Assistant's MQTT TLS configuration:
/// - `certificate`: CA cert path (like HA's CONF_CERTIFICATE)
/// - `client_cert`/`client_key`: Client auth (like HA's CONF_CLIENT_CERT/KEY)
#[derive(Clone, Debug, Default)]
pub struct TlsMaterials {
    /// PEM-encoded CA certificate bytes.
    /// When None, uses system/webpki root certificates (like HA's "auto" mode).
    pub ca: Option<Vec<u8>>,

    /// Client certificate and key for mutual TLS.
    /// Both must be provided together (cert, key).
    pub client_auth: Option<(Vec<u8>, Vec<u8>)>,
}

impl TlsMaterials {
    /// Load TLS materials from file paths.
    ///
    /// Follows Home Assistant's pattern:
    /// - CA path is optional (None = use system roots)
    /// - Client cert and key must be provided together
    pub fn load(
        ca_path: Option<&PathBuf>,
        client_cert_path: Option<&PathBuf>,
        client_key_path: Option<&PathBuf>,
    ) -> Result<Self> {
        let ca = match ca_path {
            Some(path) => Some(
                std::fs::read(path)
                    .with_context(|| format!("failed to read MQTT TLS CA '{}'", path.display()))?,
            ),
            None => None,
        };

        let client_auth = match (client_cert_path, client_key_path) {
            (Some(cert_path), Some(key_path)) => {
                let cert = std::fs::read(cert_path).with_context(|| {
                    format!(
                        "failed to read MQTT TLS client cert '{}'",
                        cert_path.display()
                    )
                })?;
                let key = std::fs::read(key_path).with_context(|| {
                    format!(
                        "failed to read MQTT TLS client key '{}'",
                        key_path.display()
                    )
                })?;
                Some((cert, key))
            }
            (None, None) => None,
            (Some(_), None) => {
                return Err(anyhow!("MQTT TLS client certificate provided without key"))
            }
            (None, Some(_)) => {
                return Err(anyhow!("MQTT TLS client key provided without certificate"))
            }
        };

        Ok(Self { ca, client_auth })
    }

    /// Check if any TLS materials are configured.
    pub fn is_configured(&self) -> bool {
        self.ca.is_some() || self.client_auth.is_some()
    }
}

/// MQTT endpoint with TLS configuration.
#[derive(Clone, Debug)]
pub struct MqttEndpoint {
    pub host: String,
    pub port: u16,
    pub use_tls: bool,
}

/// Complete TLS configuration for MQTT connections.
#[derive(Clone, Debug)]
pub struct TlsConfig {
    pub backend: TlsBackend,
    pub materials: TlsMaterials,
}

impl TlsConfig {
    /// Build a rumqttc Transport from this configuration.
    ///
    /// # Arguments
    /// * `endpoint` - MQTT endpoint with host, port, and TLS flag
    ///
    /// # Errors
    /// - If TLS materials provided but TLS is disabled
    /// - If hybrid_pq requested without pqc-tls feature
    /// - If CA is required but not provided with client certs
    pub fn build_transport(&self, endpoint: &MqttEndpoint) -> Result<Transport> {
        // Validate feature support first
        self.backend.validate_feature_support()?;

        if !endpoint.use_tls {
            if self.materials.is_configured() {
                return Err(anyhow!(
                    "MQTT TLS materials provided but TLS is disabled.\n\
                     Use --mqtt-use-tls or mqtts:// scheme to enable TLS."
                ));
            }
            return Ok(Transport::tcp());
        }

        match self.backend {
            TlsBackend::Classic => self.build_classic_transport(),
            TlsBackend::HybridPq => self.build_hybrid_pq_transport(),
        }
    }

    /// Build classic TLS transport using standard rustls.
    fn build_classic_transport(&self) -> Result<Transport> {
        if !self.materials.is_configured() {
            // Use system/webpki roots (like HA's "auto" mode)
            return Ok(Transport::tls_with_default_config());
        }

        // Custom CA and/or client auth
        let ca = self.materials.ca.clone().ok_or_else(|| {
            anyhow!(
                "MQTT TLS CA certificate is required when providing client certificates.\n\
                 Specify --mqtt-tls-ca-path or remove client cert configuration."
            )
        })?;

        Ok(Transport::tls(ca, self.materials.client_auth.clone(), None))
    }

    /// Build hybrid PQ TLS transport.
    #[cfg(feature = "pqc-tls")]
    fn build_hybrid_pq_transport(&self) -> Result<Transport> {
        use rustls::crypto::{aws_lc_rs, CryptoProvider};
        use rustls::ClientConfig;
        use std::sync::Arc;

        // Create rustls config with hybrid PQ key exchange
        let mut root_store = rustls::RootCertStore::empty();

        if let Some(ca_bytes) = &self.materials.ca {
            // Parse PEM certificates - fail loudly on any parse error
            let certs = rustls_pemfile::certs(&mut ca_bytes.as_slice())
                .collect::<Result<Vec<_>, _>>()
                .map_err(|e| anyhow!("failed to parse CA certificate from PEM: {}", e))?;
            for cert in certs {
                root_store
                    .add(cert)
                    .map_err(|e| anyhow!("failed to add CA certificate: {}", e))?;
            }
        } else {
            // Use webpki roots
            root_store.extend(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
        }

        // Configure with hybrid PQ key exchange
        // aws-lc-rs provider includes X25519Kyber768Draft00 by default
        let provider = CryptoProvider {
            kx_groups: aws_lc_rs::ALL_KX_GROUPS.to_vec(),
            ..aws_lc_rs::default_provider()
        };

        let builder = ClientConfig::builder_with_provider(Arc::new(provider))
            .with_safe_default_protocol_versions()
            .map_err(|e| anyhow!("failed to configure TLS versions: {}", e))?
            .with_root_certificates(root_store);

        // Configure client authentication (mTLS) if provided
        let config = if let Some((cert_bytes, key_bytes)) = &self.materials.client_auth {
            let certs = rustls_pemfile::certs(&mut cert_bytes.as_slice())
                .collect::<Result<Vec<_>, _>>()
                .map_err(|e| anyhow!("failed to parse client certificate from PEM: {}", e))?;
            let key = rustls_pemfile::private_key(&mut key_bytes.as_slice())
                .map_err(|e| anyhow!("failed to parse private key from PEM: {}", e))?
                .ok_or_else(|| anyhow!("no private key found in PEM file"))?;
            builder
                .with_client_auth_cert(certs, key)
                .map_err(|e| anyhow!("failed to configure TLS client auth: {}", e))?
        } else {
            builder.with_no_client_auth()
        };

        ::log::info!("TLS configured with hybrid PQ key exchange (X25519Kyber768Draft00)");

        // rumqttc accepts a custom rustls ClientConfig
        Ok(Transport::tls_with_config(
            rumqttc::TlsConfiguration::Rustls(Arc::new(config)),
        ))
    }

    /// Build hybrid PQ TLS transport (stub when feature disabled).
    #[cfg(not(feature = "pqc-tls"))]
    fn build_hybrid_pq_transport(&self) -> Result<Transport> {
        // This should be unreachable due to validate_feature_support(),
        // but provide a clear error just in case.
        Err(anyhow!(
            "Hybrid PQ TLS is not available: recompile with --features pqc-tls"
        ))
    }
}

/// Parse MQTT endpoint from address string.
///
/// Supports formats:
/// - `host:port` (plain TCP or TLS based on tls_override)
/// - `mqtt://host:port` (plain TCP)
/// - `mqtts://host:port` (TLS)
/// - `tcp://host:port` (plain TCP)
/// - `ssl://host:port` (TLS)
/// - `[ipv6]:port` (IPv6 with brackets)
pub fn parse_mqtt_endpoint(addr: &str, tls_override: bool) -> Result<MqttEndpoint> {
    let mut use_tls = tls_override;
    let mut remainder = addr.trim();

    if let Some((scheme, rest)) = remainder.split_once("://") {
        match scheme {
            "mqtt" | "tcp" => {}
            "mqtts" | "ssl" => use_tls = true,
            other => return Err(anyhow!("unsupported MQTT scheme: {}", other)),
        }
        remainder = rest;
    }

    let (host, port) = split_host_port(remainder)?;
    Ok(MqttEndpoint {
        host,
        port,
        use_tls,
    })
}

fn split_host_port(addr: &str) -> Result<(String, u16)> {
    // Handle IPv6 addresses in brackets: [::1]:1883
    if let Some(rest) = addr.strip_prefix('[') {
        let (host, rest) = rest
            .split_once(']')
            .ok_or_else(|| anyhow!("invalid MQTT address: {}", addr))?;
        let port = rest
            .strip_prefix(':')
            .ok_or_else(|| anyhow!("missing MQTT port in {}", addr))?;
        let port: u16 = port
            .parse()
            .with_context(|| format!("invalid MQTT port in {}", addr))?;
        return Ok((host.to_string(), port));
    }

    // Standard host:port
    let (host, port) = addr
        .rsplit_once(':')
        .ok_or_else(|| anyhow!("missing MQTT port in {}", addr))?;
    let port: u16 = port
        .parse()
        .with_context(|| format!("invalid MQTT port in {}", addr))?;
    Ok((host.to_string(), port))
}

/// Validate that endpoint is loopback (for security).
pub fn validate_loopback_addr(endpoint: &MqttEndpoint, original: &str) -> Result<()> {
    let host = endpoint.host.as_str();
    if host == "localhost" || host == "127.0.0.1" || host == "::1" {
        return Ok(());
    }
    if let Ok(ip) = host.parse::<std::net::IpAddr>() {
        if ip.is_loopback() {
            return Ok(());
        }
    }
    Err(anyhow!(
        "MQTT broker must be loopback for security: {} (use --allow-remote-mqtt to override)",
        original
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn backend_from_str_classic() {
        assert_eq!(
            "classic".parse::<TlsBackend>().unwrap(),
            TlsBackend::Classic
        );
        assert_eq!("rustls".parse::<TlsBackend>().unwrap(), TlsBackend::Classic);
        assert_eq!(
            "default".parse::<TlsBackend>().unwrap(),
            TlsBackend::Classic
        );
        assert_eq!(
            "CLASSIC".parse::<TlsBackend>().unwrap(),
            TlsBackend::Classic
        );
    }

    #[test]
    fn backend_from_str_hybrid_pq() {
        assert_eq!(
            "hybrid_pq".parse::<TlsBackend>().unwrap(),
            TlsBackend::HybridPq
        );
        assert_eq!(
            "hybridpq".parse::<TlsBackend>().unwrap(),
            TlsBackend::HybridPq
        );
        assert_eq!("pq".parse::<TlsBackend>().unwrap(), TlsBackend::HybridPq);
        assert_eq!(
            "hybrid".parse::<TlsBackend>().unwrap(),
            TlsBackend::HybridPq
        );
        assert_eq!(
            "HYBRID_PQ".parse::<TlsBackend>().unwrap(),
            TlsBackend::HybridPq
        );
    }

    #[test]
    fn backend_from_str_invalid() {
        assert!("unknown".parse::<TlsBackend>().is_err());
        assert!("".parse::<TlsBackend>().is_err());
    }

    #[test]
    fn backend_requires_pqc_feature() {
        assert!(!TlsBackend::Classic.requires_pqc_feature());
        assert!(TlsBackend::HybridPq.requires_pqc_feature());
    }

    #[test]
    fn backend_display() {
        assert_eq!(TlsBackend::Classic.to_string(), "classic");
        assert_eq!(TlsBackend::HybridPq.to_string(), "hybrid_pq");
    }

    #[test]
    fn parse_endpoint_plain() {
        let ep = parse_mqtt_endpoint("127.0.0.1:1883", false).unwrap();
        assert_eq!(ep.host, "127.0.0.1");
        assert_eq!(ep.port, 1883);
        assert!(!ep.use_tls);
    }

    #[test]
    fn parse_endpoint_mqtts_scheme() {
        let ep = parse_mqtt_endpoint("mqtts://broker.example.com:8883", false).unwrap();
        assert_eq!(ep.host, "broker.example.com");
        assert_eq!(ep.port, 8883);
        assert!(ep.use_tls);
    }

    #[test]
    fn parse_endpoint_tls_override() {
        let ep = parse_mqtt_endpoint("127.0.0.1:8883", true).unwrap();
        assert!(ep.use_tls);
    }

    #[test]
    fn parse_endpoint_ipv6() {
        let ep = parse_mqtt_endpoint("[::1]:1883", false).unwrap();
        assert_eq!(ep.host, "::1");
        assert_eq!(ep.port, 1883);
    }

    #[test]
    fn validate_loopback_accepts_localhost() {
        let ep = MqttEndpoint {
            host: "localhost".to_string(),
            port: 1883,
            use_tls: false,
        };
        assert!(validate_loopback_addr(&ep, "localhost:1883").is_ok());
    }

    #[test]
    fn validate_loopback_rejects_remote() {
        let ep = MqttEndpoint {
            host: "192.168.1.10".to_string(),
            port: 1883,
            use_tls: false,
        };
        assert!(validate_loopback_addr(&ep, "192.168.1.10:1883").is_err());
    }

    #[test]
    fn tls_materials_requires_both_cert_and_key() {
        use std::path::PathBuf;
        let cert_only =
            TlsMaterials::load(None, Some(&PathBuf::from("/nonexistent/cert.pem")), None);
        assert!(cert_only.is_err());
        assert!(cert_only.unwrap_err().to_string().contains("without key"));
    }

    #[test]
    fn classic_backend_validates_ok() {
        assert!(TlsBackend::Classic.validate_feature_support().is_ok());
    }

    #[test]
    #[cfg(not(feature = "pqc-tls"))]
    fn hybrid_pq_backend_requires_feature() {
        let result = TlsBackend::HybridPq.validate_feature_support();
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("pqc-tls"));
    }
}
