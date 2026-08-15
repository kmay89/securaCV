# Privacy Witness Kernel - Home Assistant Add-on
#
# Multi-stage build. Both stages use the SAME Home Assistant add-on base
# (Alpine + s6-overlay + bashio, published multi-arch), so the build-time and
# runtime musl / GStreamer ABIs match exactly and run.sh gets the
# `with-contenv` / `bashio` it relies on. The base is selected via BUILD_FROM
# and is the only external image this add-on depends on.
ARG BUILD_FROM=ghcr.io/hassio-addons/base:14.1.3

# Builder: pinned to the SAME Alpine release as the runtime add-on base
# (hassio-addons/base 14.1.3 == Alpine 3.18, GStreamer 1.22, SQLite 3.41), so the
# compiled binaries link against the exact C-library sonames present at runtime.
# Alpine 3.18's own Rust is too old (1.71) for this crate's dependencies, so we
# install a current stable toolchain via rustup (which targets musl on Alpine).
#
# cargo-chef splits dependency compilation from the application build: the expensive
# "cook" step is keyed only on Cargo.toml/Cargo.lock, so a source-only change reuses
# the cached dependency layer (and, with the buildx GHA cache wired in
# .github/workflows/addon-image.yml, across CI runs too) — only the crate itself is
# then recompiled.
FROM docker.io/library/alpine:3.18 AS chef

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
# checksum-verified, and the two base images below (BUILD_FROM and alpine:3.18)
# are pinned by tag, not digest. The root Dockerfile pins its bases by
# `@sha256:` — apply the same to BUILD_FROM/alpine here and add a sha256 check
# of rustup-init to close this fully (needs registry/artifact digests).
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

# Runtime stage
FROM ${BUILD_FROM}

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

# Copy add-on scripts and wizard
COPY privacy_witness_kernel/run.sh /run.sh
COPY privacy_witness_kernel/run_lib.sh /usr/local/bin/run_lib.sh
COPY privacy_witness_kernel/discover_cameras.sh /usr/local/bin/
COPY privacy_witness_kernel/serve_wizard.py /usr/local/bin/
COPY privacy_witness_kernel/wizard/ /usr/local/share/securacv-wizard/
RUN chmod +x /run.sh /usr/local/bin/discover_cameras.sh /usr/local/bin/serve_wizard.py

# Data directories
RUN mkdir -p /data/vault /config

WORKDIR /config
CMD ["/run.sh"]
