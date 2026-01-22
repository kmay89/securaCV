# Changelog

## [0.3.1] - 2026-01-21
### Fixed
- `log_verify` now verifies break-glass receipt chain (called from `main`)
- Receipt verification uses `[u8; 32]` device key signature input and supports `--verbose`


All notable changes to the Privacy Witness Kernel will be documented in this file.

## [0.2.0] - 2026-01-21

### Added
- **Frame isolation layer** (`src/frame.rs`):
  - `RawFrame`: Opaque container with private bytes (no Clone, no AsRef<[u8]>)
  - `InferenceView`: Restricted interface for modules (cannot export bytes)
  - `FrameBuffer`: Bounded ring buffer with build-time caps (30s, 300 frames)
  - `Detector` trait: Modules run inference without capturing pixel data
  - `StubDetector`: MVP motion detection via pixel hash comparison
  - `BreakGlassToken`: Placeholder for quorum-gated vault access

- **Ingestion layer** (`src/ingest/`):
  - `RtspSource`: Stub RTSP source with synthetic frames
  - `RtspConfig`: Configuration for RTSP streams
  - Timestamp coarsening at capture time
  - Non-invertible feature hash computation at capture time

- **Runtime improvements**:
  - `env_logger` for structured logging
  - Frame buffer stats logging
  - Verbose mode for `log_verify`
  - Conformance alarm checking in `log_verify`

### Changed
- `Module` trait now receives `InferenceView` instead of `Frame`
- `ZoneCrossingModule` uses `StubDetector` for motion detection
- `witnessd` uses `RtspSource` and `FrameBuffer` for frame handling

### Security
- Raw bytes are now physically inaccessible to modules (type-level enforcement)
- Frame buffer auto-zeroizes on drop and eviction
- Only path to raw bytes is `RawFrame::export_for_vault()` requiring `BreakGlassToken`

## [0.1.2] - 2026-01-21

### Fixed
- `validate_zone_id()` regex now compiled once via OnceLock
- Added negative test for module event-type allowlist rejection

## [0.1.1] - 2026-01-21

### Added
- `ReprocessGuard` wired into `read_events_ruleset_bound()`
- `conformance_alarms` actively written on contract/module violations
- `RawMediaBoundary` choke point scaffold
- Runtime module event-type authorization via `ModuleDescriptor`

### Changed
- Zone ID validation: blocklist → strict allowlist regex

## [0.1.0] - 2026-01-20

### Added
- Initial kernel: sealed log, contract enforcer, bucket key manager
- `witnessd` daemon and `log_verify` tool
- Spec documents: invariants, event contract, threat model, architecture