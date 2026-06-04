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
FROM docker.io/library/alpine:3.18 AS builder

RUN apk add --no-cache \
    build-base cmake perl pkgconf linux-headers musl-dev \
    curl ca-certificates \
    gstreamer-dev gst-plugins-base-dev \
    libseccomp-dev \
    sqlite-dev \
    openssl-dev
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
      | sh -s -- -y --profile minimal --default-toolchain stable
ENV PATH="/root/.cargo/bin:${PATH}"

WORKDIR /build
COPY . .

# Build with GStreamer support for real RTSP streams.
RUN cargo build --release --features rtsp-gstreamer

# Runtime stage
FROM ${BUILD_FROM}

# Install runtime dependencies
RUN apk add --no-cache \
    gstreamer \
    gst-plugins-base \
    gst-plugins-good \
    gst-plugins-bad \
    gst-plugins-ugly \
    libseccomp \
    sqlite-libs \
    openssl \
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
COPY privacy_witness_kernel/discover_cameras.sh /usr/local/bin/
COPY privacy_witness_kernel/serve_wizard.py /usr/local/bin/
COPY privacy_witness_kernel/wizard/ /usr/local/share/securacv-wizard/
RUN chmod +x /run.sh /usr/local/bin/discover_cameras.sh /usr/local/bin/serve_wizard.py

# Data directories
RUN mkdir -p /data/vault /config

WORKDIR /config
CMD ["/run.sh"]
