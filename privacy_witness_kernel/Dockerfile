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
# Every base is pinned by digest, the way the root Dockerfile pins its own:
# a tag is a pointer a registry can move, a digest is the image that was
# reviewed. Digests resolved 2026-11-06 from the registries' manifest HEAD
# (`docker buildx imagetools inspect <image>` prints the same ones); move
# them together with ALPINE_VERSION.
ARG ALPINE_DIGEST=sha256:de0eb0b3f2a47ba1eb89389859a9bd88b28e82f5826b6969ad604979713c2d4f

# The runtime base, one stage per arch. Alpine 3.18 == GStreamer 1.22,
# SQLite 3.41, Python 3.11 — move ALPINE_VERSION and these two tags together.
FROM ghcr.io/home-assistant/amd64-base:3.18@sha256:5bca760fc27a3cd3356bffe9de74ae075c6e798bdd63815c1c10eae9aa1d1023 AS runtime-amd64
FROM ghcr.io/home-assistant/aarch64-base:3.18@sha256:e605a9a2979f7251a4cdd757f427940662cbb3e32cb9f935b1f38c8f3724fb76 AS runtime-arm64

# Builder: pinned to the SAME Alpine release as the runtime base.
# Alpine 3.18's own Rust is too old (1.71) for this crate's dependencies, so we
# install a current stable toolchain via rustup (which targets musl on Alpine).
#
# cargo-chef splits dependency compilation from the application build: the expensive
# "cook" step is keyed only on Cargo.toml/Cargo.lock, so a source-only change reuses
# the cached dependency layer (and, with the buildx GHA cache wired in
# .github/workflows/addon-image.yml, across CI runs too) — only the crate itself is
# then recompiled.
FROM docker.io/library/alpine:${ALPINE_VERSION}@${ALPINE_DIGEST} AS chef
# TARGETARCH is provided automatically by buildx (amd64 / arm64); a stage
# re-declares it to read it.
ARG TARGETARCH

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
# The installer is not the `sh.rustup.rs` script (which fetches whatever
# rustup-init is current) but one pinned rustup-init per target, held to the
# sha256 Rust publishes beside it:
#   https://static.rust-lang.org/rustup/archive/<RUSTUP_VERSION>/<target>/rustup-init.sha256
# (read 2026-11-06). A mismatch fails the layer before anything runs.
ARG RUST_TOOLCHAIN=1.97.0
ARG RUSTUP_VERSION=1.29.3
ARG RUSTUP_INIT_SHA256_X86_64=1552169aba2836777136ccff7a381e95c17233b5d8250cd5e10ae3cb971bfcfc
ARG RUSTUP_INIT_SHA256_AARCH64=956deacdc27ab82ae7ac821d5d3e9e95e4795d476d17efe99743bb8531ca1031
RUN set -eux; \
    case "$TARGETARCH" in \
      amd64) rust_target=x86_64-unknown-linux-musl; want="$RUSTUP_INIT_SHA256_X86_64" ;; \
      arm64) rust_target=aarch64-unknown-linux-musl; want="$RUSTUP_INIT_SHA256_AARCH64" ;; \
      *) echo "unsupported TARGETARCH='$TARGETARCH' for rustup-init download" >&2; exit 1 ;; \
    esac; \
    curl --proto '=https' --tlsv1.2 -fsSL \
      "https://static.rust-lang.org/rustup/archive/${RUSTUP_VERSION}/${rust_target}/rustup-init" \
      -o /tmp/rustup-init; \
    echo "${want}  /tmp/rustup-init" | sha256sum -c -; \
    chmod 0755 /tmp/rustup-init; \
    /tmp/rustup-init -y --profile minimal --default-toolchain "${RUST_TOOLCHAIN}"; \
    rm -f /tmp/rustup-init
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
# The release publishes no checksum file, so the sha256 of each tarball was
# taken from the asset itself as downloaded from github.com on 2026-11-06
# (decompressed and listed to prove it was the archive, not an error page);
# a release re-cut under the same tag fails the layer here.
ARG CARGO_CHEF_VERSION=0.1.77
ARG CARGO_CHEF_SHA256_X86_64=a3733ab416c3ffddd37914cd13919ca05fee1a1cf654f3016dcfe7f399d89cd1
ARG CARGO_CHEF_SHA256_AARCH64=3bff2e7ff979db837c7e820dfd8d51d2d97b9c94283506c6e596ae4887ef178b
RUN set -eu; \
    case "$TARGETARCH" in \
      amd64) chef_arch=x86_64; want="$CARGO_CHEF_SHA256_X86_64" ;; \
      arm64) chef_arch=aarch64; want="$CARGO_CHEF_SHA256_AARCH64" ;; \
      *) echo "unsupported TARGETARCH='$TARGETARCH' for cargo-chef download" >&2; exit 1 ;; \
    esac; \
    url="https://github.com/LukeMathwalker/cargo-chef/releases/download/v${CARGO_CHEF_VERSION}/cargo-chef-${chef_arch}-unknown-linux-musl.tar.xz"; \
    curl --proto '=https' --tlsv1.2 -fsSL "$url" -o /tmp/cargo-chef.tar.xz; \
    echo "${want}  /tmp/cargo-chef.tar.xz" | sha256sum -c -; \
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
FROM docker.io/library/alpine:${ALPINE_VERSION}@${ALPINE_DIGEST} AS carry
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

# No `USER`: the image runs the Home Assistant base's s6-overlay as PID 1
# (config.yaml `init: false`), whose cont-init step (run.sh, `with-contenv
# bashio`) writes /run/securacv and the services' argv as root, and the
# Supervisor expects the app's processes to run as root under the AppArmor
# profile in apparmor.txt. Dropping privileges per service is a change to
# the s6 tree, not to this file.

LABEL \
    org.opencontainers.image.title="SecuraCV" \
    org.opencontainers.image.description="The verified witness log for Frigate — signed events, no footage, no identity" \
    org.opencontainers.image.source="https://github.com/kmay89/securaCV" \
    org.opencontainers.image.documentation="https://github.com/kmay89/securaCV/blob/main/privacy_witness_kernel/DOCS.md" \
    org.opencontainers.image.licenses="See repository LICENSE"
