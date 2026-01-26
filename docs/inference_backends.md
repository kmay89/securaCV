# Inference Backends and Device Capabilities

This kernel selects inference backends **only** from device-local capability flags.
Backend selection is an internal runtime concern and **does not** add any new
metadata to events or logs. Selection failures are treated as conformance
failures and **fail closed**.

## Selection rules

- Backends are chosen from the intersection of:
  - The module's supported backends.
  - The device's declared capabilities.
- `BackendSelection::Require(X)` MUST fail closed if `X` is not supported.
- `BackendSelection::Auto` selects the highest available backend in this order:
  1. Accelerator
  2. CPU
  3. Stub
- If no backend is available, selection fails closed.

## Capability matrix

| Backend | Capability flag | Status | Notes |
| --- | --- | --- | --- |
| Stub | `DeviceCapabilities.stub` | Available | Minimal non-extractive baseline backend. |
| CPU | `DeviceCapabilities.cpu` | Available | CPU path using the same non-extractive primitives as the stub backend. |
| Accelerator | `DeviceCapabilities.accelerator` | **Unavailable** | Requests fail closed until a conforming accelerator backend exists. |

## Conformance guardrails

- Backend selection never alters event schema or precision.
- No backend may emit identity-like metadata.
- Unsupported backends must never fall back silently.
