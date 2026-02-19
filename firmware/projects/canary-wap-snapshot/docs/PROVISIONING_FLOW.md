# Provisioning Flow

## Overview

When a SecuraCV Canary boots for the first time (or after factory reset), it generates
a unique cryptographic identity and derives all credentials from it. This document
describes the end-to-end provisioning process and how to obtain the API token.

## First Boot Sequence

```
1. NVS check: keypair exists?
   ├── YES → Load existing identity, derive token, resume normal operation
   └── NO  → First boot / factory reset path:
       a. Generate Ed25519 keypair
       b. Store keypair in NVS
       c. Compute public key fingerprint (SHA-256, first 8 bytes hex)
       d. Derive API token via two-step HKDF (see API_AUTH.md)
       e. Derive device-unique AP password from fingerprint
       f. Store API token in NVS
       g. Generate TLS self-signed certificate (RSA-2048)
       h. Store TLS cert and key in NVS
       i. Print provisioning receipt to Serial
       j. Mark identity as first_boot = true
```

## Serial Provisioning Receipt

On first boot, the device prints a complete provisioning receipt to Serial (115200 baud):

```
════════════════════════════════════════
  SecuraCV Canary — Provisioning Receipt
════════════════════════════════════════

  Device ID    : SCV-A1B2
  MAC Address  : AA:BB:CC:DD:EE:FF
  Fingerprint  : a1b2c3d4e5f67890
  WiFi SSID    : SecuraCV-EEFF
  WiFi Password: cv-a1b2c
  API Token    : cv_7kX9mN2pQrS5tU8vW0xY1zA3bC4dE6f

  ⚠ SAVE THIS TOKEN — it will not be shown in full again.
  Use it in the SAP "Add Canary" form or in the dashboard.

════════════════════════════════════════
```

On subsequent boots, the token is redacted:

```
  API Token    : cv_7kX9****  (use BOOT button or Serial 'w' to retrieve)
```

## Obtaining the Token

There are three ways to obtain the API token:

### 1. First Boot Serial Output (Recommended)

Connect via USB Serial at 115200 baud before first power-on. The full token
is printed once and never shown in full via Serial again.

### 2. Physical BOOT Button (Any Time)

The device exposes a `/api/provisioning-receipt` endpoint that returns the full
token, but **only when the physical BOOT button is pressed**:

1. Connect to the device's WiFi AP
2. Open the dashboard at `https://192.168.4.1/` (or `http://` if TLS unavailable)
3. Click "Request from Device" in the auth modal
4. Press the BOOT button on the device within 60 seconds
5. The dashboard polls the endpoint every 2 seconds
6. When the button is pressed, the provisioning gate opens for 30 seconds
7. The receipt (including full token) is returned

This ensures physical access to the device is required to obtain the token.

### 3. Serial Command (Any Time)

Send `w` over Serial to display WiFi credentials and the redacted token.
The full token is shown only on first boot.

## Provisioning Receipt JSON

The `/api/provisioning-receipt` endpoint returns SAP-compatible JSON:

```json
{
  "device_id": "SCV-A1B2",
  "mac": "AA:BB:CC:DD:EE:FF",
  "fingerprint": "a1b2c3d4e5f67890",
  "public_key_hex": "302a300506032b6570032100...",
  "token": "cv_7kX9mN2pQrS5tU8vW0xY1zA3bC4dE6f",
  "wifi_ssid": "SecuraCV-EEFF",
  "wifi_password": "cv-a1b2c",
  "firmware": "2.1.0-wap",
  "tls_enabled": true,
  "tls_fingerprint": "AB:CD:EF:01:23:45:..."
}
```

This JSON can be pasted directly into the SAP "Add Canary" form.

### Access Control

The provisioning receipt endpoint has dual access control:

- **Physical gate**: Returns receipt when BOOT button is pressed (no auth needed)
- **Bearer token**: Returns receipt when valid token is provided (no button needed)

If neither condition is met, returns `403 Forbidden`:

```json
{
  "error": "forbidden",
  "hint": "Press physical BOOT button on device to authorize"
}
```

## Device-Unique AP Password

The WiFi AP password is derived from the public key fingerprint:

```
Format: cv-XXXXX
Where XXXXX = first 5 characters of the hex fingerprint
```

This replaces the default `witness2026` password with a device-unique credential.
The password is printed during provisioning and shown via the Serial `w` command.

## Factory Reset

Long-pressing the BOOT button for 3+ seconds triggers a factory reset:

1. Clears keypair from NVS
2. Clears API token from NVS
3. Clears TLS certificate and key from NVS
4. Reboots the device
5. On next boot, a fresh identity is generated (new token, new password)

## SAP Integration

The provisioning receipt is formatted for direct use with the SecuraCV Administrative
Platform (SAP) "Add Canary" workflow:

1. Provision the device (first boot or BOOT button)
2. Copy the receipt JSON
3. In SAP, navigate to Fleet > Add Canary
4. Paste the receipt JSON
5. SAP validates the fingerprint and registers the device
