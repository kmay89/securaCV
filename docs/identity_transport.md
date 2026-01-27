# Device Identity and Transport Independence

## Purpose

This note captures how device identity should work independently of transport protocols, and
highlights where future transport adapters can hook into the current API and bridge binaries.
The goal is to keep identity anchored locally and stable even as MQTT/HTTP endpoints change.

## Device identity (Ed25519 keys, transport-independent)

* **Identity is a keypair, not an endpoint.** Each device owns an Ed25519 keypair used as the
  stable identity. Network addresses, MQTT client IDs, or HTTP endpoints are transport-level
  routing details and must not be treated as identity.
* **Public key = device identity.** The Ed25519 public key (or its canonical fingerprint) is
  the globally unique identifier for a device.
* **Key storage is local-only.** Private keys are generated and stored locally on the device.
  No external identity service is consulted for key issuance or verification.

## Peer identity lookup and verification when transports change

When a device connects via a different transport (e.g., switching from HTTP polling to MQTT
publish/subscribe), the peer verification flow should remain the same:

1. **Retrieve the presented identity.** The transport adapter obtains the peer's presented
   Ed25519 public key (or a signed assertion containing it).
2. **Resolve local trust anchor.** The adapter looks up the public key in the local trust store
   (e.g., a local file or database of accepted device keys). No remote lookup is allowed.
3. **Verify signatures.** The adapter verifies that messages/events are signed by the private
   key corresponding to the presented public key. Signed payloads MUST include replay
   protection (e.g., nonce, sequence number, or sufficiently granular timestamp) and adapters
   MUST reject replays. The transport endpoint (host/port/client ID) is treated as untrusted
   metadata.
4. **Bind session metadata.** The adapter may record a local-only mapping of the current
   transport endpoint to the verified identity for audit/debugging, but the mapping must not
   become an identity source of truth.

If a presented public key is not found in the local trust store, the connection or message
MUST be rejected and MUST NOT be processed or forwarded.

This flow ensures that changing MQTT brokers, HTTP endpoints, or other transports does not
break identity: the key remains the root of trust, and the trust anchor stays local.

## Constraints

* **No external identity services.** No remote key registries, directory services, or
  cloud-based identity providers are allowed.
* **Local-only trust anchors.** The only source of trust is local configuration (files or
  embedded policy) and locally stored approved public keys.
* **Identity keys are long-lived.** The device's primary Ed25519 public key is considered
  stable. Rotation of this key is a heavyweight operation requiring explicit re-provisioning
  (e.g., in response to a suspected key compromise). Lighter-weight rotation applies only to
  protocol-level aliases or gossip identifiers and MUST NOT change the underlying device
  public key.
* **Capability tokens are not identity.** Tokens grant permission only; possession of a valid
  token MUST NOT be treated as proof of device identity.

## Candidate integration points for adapter hooks

The following locations are natural seams to introduce transport-agnostic identity adapters:

### `src/api/mod.rs`

* **Connection handling (`handle_connection`).** This function already validates loopback
  access and capability tokens. A future hook can attach a transport-level identity adapter
  that extracts a peer Ed25519 identity and verifies signatures before servicing `/events`.
* **Token validation (`CapabilityTokenManager::validate`).** If capability tokens are signed
  or wrapped in identity assertions, verification can be performed here or in a pre-validate
  hook that associates the request with a verified device key.

### MQTT bridge binaries

* **`src/bin/event_mqtt_bridge.rs` (publish path).** The bridge constructs MQTT clients and
  publishes events. A future adapter hook can:
  * sign outgoing event payloads with the local device key,
  * attach a public-key fingerprint in MQTT properties/headers,
  * verify broker/server identity using locally pinned trust anchors.
* **`src/bin/frigate_bridge.rs` (subscribe path).** The bridge consumes MQTT events. A future
  adapter hook can:
  * verify signatures on incoming messages using the local trust store,
  * map broker endpoint changes to the same verified device identity,
  * reject events that do not present a trusted public key.

These hooks keep transport-specific concerns isolated while preserving the key-based identity
model across HTTP and MQTT flows.
