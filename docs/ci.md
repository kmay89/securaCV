# CI validation

This repository favors invariant-preserving checks that validate buildability
without expanding data access paths.

## Container build validation

Container images are built and smoke-tested in CI — a green build is required
before changes to an image or the crate it packages can merge:

- **`witnessd` image** (root `Dockerfile`):
  `.github/workflows/container-images.yml` builds the image on every PR/push
  touching it and asserts every shipped binary loads with no missing shared
  libraries and that the healthcheck's `curl` is present.
- **Frigate sidecar** (`docker/sidecar/Dockerfile`):
  `.github/workflows/docker-sidecar.yml` lints the scripts, validates the
  compose files, runs a full end-to-end test against a live broker, and
  publishes to GHCR on main/tags.
- **Home Assistant add-on** (`privacy_witness_kernel/Dockerfile`):
  `.github/workflows/addon-image.yml` builds amd64 (+ aarch64 on main/tags),
  publishes to GHCR, and verifies every declared arch is anonymously pullable.

To reproduce the witnessd build locally:

```bash
docker build --build-arg CARGO_FEATURES=rtsp-gstreamer -t witnessd:ci .
```

This confirms the deployable artifact can be built with RTSP support while
keeping the runtime surface limited to the Event API.
