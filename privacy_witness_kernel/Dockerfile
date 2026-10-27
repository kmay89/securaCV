# SecuraCV — Home Assistant app (add-on)
#
# Multi-stage build. The runtime is the OFFICIAL Home Assistant base image
# (ghcr.io/home-assistant/<arch>-base: Alpine + s6-overlay v3 + bashio),
# pinned to the Alpine release the builder stage uses, so the compiled
# binaries link against the exact musl / GStreamer / SQLite sonames present
# at runtime. The arch is selected by BuildKit's TARGETARCH through a pair of
# named stages (there is no build.yaml; config.yaml `image:` names the
# published per-arch image). The image runs s6-overlay as PID 1
# (config.yaml `init: false`): the services under rootfs/etc/services.d
# supervise the kernel, the bridges and the panel, and run.sh is the one
# setup step that runs before them (cont-init).
ARG TARGETARCH
ARG ALPINE_VERSION=3.18

# The runtime base, one stage per arch. Alpine 3.18 == GStreamer 1.22,
# SQLite 3.41, Python 3.11 — move ALPINE_VERSION and these two tags together.
FROM ghcr.io/home-assistant/amd64-base:3.18 AS runtime-amd64
FROM ghcr.io/home-assistant/aarch64-base:3.18 AS runtime-arm64

# Builder: pinned to the SAME Alpine release as the runtime base.
# Alpine 3.18's own Rust is too old (1.71) for this crate's dependencies, so we
# install a current stable toolchain via rustup (which targets musl on Alpine).
#
# cargo-chef splits dependency compilation from the application build: the expensive
# "cook" step is keyed only on Cargo.toml/Cargo.lock, so a source-only change reuses
# the cached dependency layer (and, with the buildx GHA cache wired in
# .github/workflows/addon-image.yml, across CI runs too) — only the crate itself is
# then recompiled.
FROM docker.io/library/alpine:${ALPINE_VERSION} AS chef

RUN apk add --no-cache \
    build-base cmake perl pkgconf linux-headers musl-dev \
    curl ca-certificates xz \
    gstreamer-dev gst-plugins-base-dev \
    libseccomp-dev \
    sqlite-dev \
    openssl-dev
# Pin an EXACT toolchain (not the moving `stable`) so the build is
# reproducible — this matches the Rust version the root Dockerfile builds the
# same crate with. `set -eux` fails the layer on any error in the pipe.
# NOTE (supply chain, tracked): the rustup installer is fetched over TLS but not
# checksum-verified, and the base images are pinned by tag, not digest. The
# root Dockerfile pins its bases by `@sha256:` — apply the same here and add a
# sha256 check of rustup-init to close this fully (needs registry/artifact
# digests).
ARG RUST_TOOLCHAIN=1.97.0
RUN set -eux; \
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
      | sh -s -- -y --profile minimal --default-toolchain "${RUST_TOOLCHAIN}"
ENV PATH="/root/.cargo/bin:${PATH}"
# Disable crt-static for the whole build: the musl target links fully static by
# default, but Alpine ships only shared (.so) GStreamer/GLib/seccomp libraries (no
# static archives), so link dynamically instead. The runtime stage is the same Alpine
# release and provides all of these .so libs. Set as ENV (not only on the final build)
# so the dependency "cook" and the crate build use identical flags — required for the
# cached dependency artifacts to be reused by the final build.
ENV RUSTFLAGS="-C target-feature=-crt-static"
# Install cargo-chef from its pinned prebuilt musl release rather than compiling it
# from source — much faster on a cold build, and avoids compiling it under arm64 QEMU.
# TARGETARCH is provided automatically by buildx (amd64 / arm64).
ARG CARGO_CHEF_VERSION=0.1.77
ARG TARGETARCH
RUN set -eu; \
    case "$TARGETARCH" in \
      amd64) chef_arch=x86_64 ;; \
      arm64) chef_arch=aarch64 ;; \
      *) echo "unsupported TARGETARCH='$TARGETARCH' for cargo-chef download" >&2; exit 1 ;; \
    esac; \
    url="https://github.com/LukeMathwalker/cargo-chef/releases/download/v${CARGO_CHEF_VERSION}/cargo-chef-${chef_arch}-unknown-linux-musl.tar.xz"; \
    curl --proto '=https' --tlsv1.2 -fsSL "$url" -o /tmp/cargo-chef.tar.xz; \
    mkdir -p /tmp/cargo-chef; \
    xz -dc /tmp/cargo-chef.tar.xz | tar -x -C /tmp/cargo-chef; \
    chef_bin="$(find /tmp/cargo-chef -type f -name cargo-chef | head -n1)"; \
    test -n "$chef_bin"; \
    install -m 0755 "$chef_bin" /usr/local/bin/cargo-chef; \
    rm -rf /tmp/cargo-chef /tmp/cargo-chef.tar.xz; \
    test -x /usr/local/bin/cargo-chef
WORKDIR /build

# Planner: distil the dependency graph into a recipe from the manifests only.
FROM chef AS planner
COPY . .
RUN cargo chef prepare --recipe-path recipe.json

# Builder: cook dependencies (cached unless the recipe — i.e. Cargo.toml/lock —
# changes), then build the crate against the already-compiled deps.
FROM chef AS builder
COPY --from=planner /build/recipe.json recipe.json
RUN cargo chef cook --release --features rtsp-gstreamer --recipe-path recipe.json
COPY . .
RUN cargo build --release --features rtsp-gstreamer

# Carry: the SecuraCV integration and the blueprints the app places into
# /config (option G, D15). The integration tree is pruned here — no tests/,
# no www/*.test.js, no caches — so the copy a user ends up with is the
# integration without its tests and caches (Docker COPY has no exclude, so an
# intermediate stage prunes). HACS serves the mirror's carried copy of this
# same tree, tests included; the two match in every file kept here once a
# carry has run, and not before it. The carried integration's version must
# equal the app's (config.yaml): this layer refuses a mismatch, as
# scripts/lint_version_sync.sh and the pwk-wizard-tests job do before any
# image is built.
FROM docker.io/library/alpine:${ALPINE_VERSION} AS carry
COPY custom_components/securacv /carry/integration/securacv
COPY docs/blueprints/ /carry/blueprints/
COPY privacy_witness_kernel/config.yaml /carry/config.yaml
RUN set -eu; \
    rm -rf /carry/integration/securacv/tests; \
    rm -f /carry/integration/securacv/www/*.test.js; \
    find /carry -type d -name __pycache__ -prune -exec rm -rf {} +; \
    find /carry -type f -name '*.pyc' -delete; \
    find /carry/blueprints -type f ! -name '*.yaml' -delete; \
    carried=$(sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p' /carry/integration/securacv/manifest.json | head -n1); \
    app=$(sed -n 's/^version:[[:space:]]*"\([^"]*\)".*/\1/p' /carry/config.yaml | head -n1); \
    test -n "$carried" && test -n "$app"; \
    if [ "$carried" != "$app" ]; then \
      echo "carried integration version $carried != app version $app" >&2; exit 1; \
    fi; \
    rm -f /carry/config.yaml

# Runtime stage
FROM runtime-${TARGETARCH}
ARG TARGETARCH
RUN if [ -z "${TARGETARCH}" ]; then \
      echo "TARGETARCH is not set, please use Docker BuildKit for the build." && exit 1; \
    fi

# Install runtime dependencies. Note: libcrypto.so.3 (needed by the SQLCipher-linked
# binaries) is already provided by the base image's pinned libcrypto3/libssl3, so we must
# NOT pull the `openssl` metapackage here — it wants a newer revision and breaks that pin.
RUN apk add --no-cache \
    gstreamer \
    gst-plugins-base \
    gst-plugins-good \
    gst-plugins-bad \
    gst-plugins-ugly \
    libseccomp \
    sqlite-libs \
    jq \
    curl \
    python3

# Copy binaries
COPY --from=builder /build/target/release/witnessd /usr/local/bin/
COPY --from=builder /build/target/release/witness_api /usr/local/bin/
COPY --from=builder /build/target/release/frigate_bridge /usr/local/bin/
COPY --from=builder /build/target/release/event_mqtt_bridge /usr/local/bin/
COPY --from=builder /build/target/release/break_glass /usr/local/bin/
COPY --from=builder /build/target/release/log_verify /usr/local/bin/
COPY --from=builder /build/target/release/export_events /usr/local/bin/
COPY --from=builder /build/target/release/export_verify /usr/local/bin/
COPY --from=builder /build/target/release/log_anchor /usr/local/bin/
COPY --from=builder /build/target/release/court_export /usr/local/bin/

# The carried integration and blueprints (read by run.sh / serve_wizard.py)
COPY --from=carry /carry/integration /usr/share/securacv/integration
COPY --from=carry /carry/blueprints /usr/share/securacv/blueprints

# App scripts, the panel and the s6 service tree. run.sh is the one-time
# setup step: s6-overlay runs /etc/cont-init.d before any service starts.
COPY privacy_witness_kernel/run.sh /etc/cont-init.d/10-securacv-setup
COPY privacy_witness_kernel/run_lib.sh /usr/local/bin/run_lib.sh
COPY privacy_witness_kernel/discover_cameras.sh /usr/local/bin/
COPY privacy_witness_kernel/serve_wizard.py /usr/local/bin/
COPY privacy_witness_kernel/integration_carry.py /usr/local/bin/
COPY privacy_witness_kernel/wizard/ /usr/local/share/securacv-wizard/
COPY privacy_witness_kernel/rootfs/ /
RUN chmod +x /etc/cont-init.d/10-securacv-setup \
      /usr/local/bin/discover_cameras.sh \
      /usr/local/bin/serve_wizard.py \
      /usr/local/bin/integration_carry.py \
      /etc/services.d/*/run /etc/services.d/*/finish

# Data directories
RUN mkdir -p /data/vault

LABEL \
    org.opencontainers.image.title="SecuraCV" \
    org.opencontainers.image.description="The verified witness log for Frigate — signed events, no footage, no identity" \
    org.opencontainers.image.source="https://github.com/kmay89/securaCV" \
    org.opencontainers.image.documentation="https://github.com/kmay89/securaCV/blob/main/privacy_witness_kernel/DOCS.md" \
    org.opencontainers.image.licenses="See repository LICENSE"
