# TLS Setup

## Overview

The SecuraCV Canary uses TLS (Transport Layer Security) to encrypt all HTTP traffic
between the connected client and the device. TLS is provided by ESP-IDF's native
`esp_https_server` component, which wraps `esp_http_server` with mbedtLS.

## Certificate Generation

At first boot, the device generates a self-signed TLS certificate:

```
Algorithm:  RSA-2048
Signature:  SHA-256 with RSA
Subject:    CN=SecuraCV Canary
Validity:   10 years from generation
Storage:    NVS (DER-encoded)
```

### Why Self-Signed?

The Canary operates as an isolated WiFi AP with no internet connection. There is no
Certificate Authority (CA) infrastructure available. Self-signed certificates provide
encryption (confidentiality) without third-party trust validation. Since the user
connects directly to the device's AP, the trust model is physical proximity.

### Why RSA-2048?

ESP-IDF's `esp_https_server` has broad RSA support. RSA-2048 provides adequate
security for a local-only device and is well-supported by all browsers and HTTP
clients that will connect to the device.

## Architecture

```
Port 443 (HTTPS)              Port 80 (HTTP)
    │                              │
    ▼                              ▼
┌──────────────┐            ┌──────────────┐
│ esp_https    │            │ esp_http     │
│ _server      │            │ _server      │
│              │            │              │
│ All API      │            │ Redirect     │
│ routes       │            │ handler      │
│ registered   │            │ only         │
│ here         │            │ (301 → 443)  │
└──────────────┘            └──────────────┘
```

- **HTTPS server** (port 443): Handles all API routes with TLS encryption
- **HTTP server** (port 80): Only serves 301 redirects to HTTPS equivalent

### Fallback Mode

If TLS certificate generation fails (e.g., insufficient memory, mbedtls error):

```
Port 80 (HTTP)
    │
    ▼
┌──────────────┐
│ esp_http     │
│ _server      │
│              │
│ All API      │
│ routes       │
│ registered   │
│ here         │
│ (no TLS)     │
└──────────────┘
```

A warning is logged and the device falls back to HTTP-only operation. The
`/api/device-info` endpoint reports `tls_enabled: false` in this case.

## NVS Storage

The TLS certificate and private key are stored in NVS to persist across reboots:

| NVS Key | Content | Format |
|---------|---------|--------|
| `tls_cert` | X.509 certificate | DER-encoded |
| `tls_key` | RSA private key | DER-encoded |

On boot, the device attempts to load the certificate from NVS. If not found
(first boot), it generates a new certificate and stores it.

## Certificate Fingerprint

The SHA-256 fingerprint of the TLS certificate is:

- Available via `/api/device-info` endpoint (`tls_fingerprint` field)
- Included in the provisioning receipt JSON
- Printed to Serial on first boot

This allows SAP and other clients to pin the certificate by fingerprint.

## Browser Trust

Since the certificate is self-signed, browsers will display a security warning
on first connection. This is expected behavior:

1. Connect to the device's WiFi AP
2. Navigate to `https://192.168.4.1/`
3. Browser shows "Your connection is not private" (or similar)
4. Click "Advanced" → "Proceed to 192.168.4.1 (unsafe)"
5. The dashboard loads with TLS encryption active

The warning appears because no CA signed the certificate, not because the
connection is insecure. Traffic is still encrypted.

## Troubleshooting

### TLS Certificate Not Generated

**Symptom**: Device falls back to HTTP-only, `tls_enabled: false` in device-info.

**Possible causes**:
- Insufficient free heap memory during boot (RSA key generation needs ~20KB)
- mbedtls entropy source failure
- NVS partition full or corrupted

**Resolution**:
1. Check Serial output for `[TLS]` error messages
2. Factory reset (long press BOOT 3+ seconds) to clear NVS and regenerate
3. Verify the partition scheme is set to "Huge APP (3MB No OTA)"

### Certificate Mismatch After Factory Reset

**Symptom**: Browser shows different certificate fingerprint after reset.

**Expected**: Factory reset generates a new keypair and new TLS certificate.
The fingerprint will change. Update the SAP registration accordingly.

### Connection Refused on Port 443

**Symptom**: Browser cannot connect to HTTPS port.

**Possible causes**:
- TLS initialization failed (check Serial output)
- Device is running in HTTP-only fallback mode
- Try `http://192.168.4.1/` to verify device is responding

### ESP-IDF HTTPS Server Configuration

The HTTPS server is configured with:

```cpp
httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
config.port_secure = 443;
config.cacert_pem = cert_der;       // DER certificate
config.cacert_len = cert_der_len;
config.prvtkey_pem = key_der;       // DER private key
config.prvtkey_len = key_der_len;
config.httpd.max_uri_handlers = 40; // Accommodate all API routes
config.httpd.stack_size = 8192;     // Stack for TLS handshake
```
