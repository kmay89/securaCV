# CI validation

This repository favors invariant-preserving checks that validate buildability
without expanding data access paths.

## Minimal container build validation

```bash
docker build --build-arg CARGO_FEATURES=rtsp-gstreamer -t witnessd:ci .
```

This step confirms the deployable artifact can be built with RTSP support while
keeping the runtime surface limited to the Event API.
