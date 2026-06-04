# Privacy Witness Kernel - Home Assistant Add-on
# Multi-stage build for minimal image size

ARG BUILD_FROM=ghcr.io/home-assistant/amd64-base-rust:latest
FROM ${BUILD_FROM} AS builder

# Install build dependencies
RUN apk add --no-cache \
    musl-dev \
    pkgconf \
    gstreamer-dev \
    gst-plugins-base-dev \
    libseccomp-dev \
    sqlite-dev

WORKDIR /build
COPY . .

# Build with GStreamer support for real RTSP streams
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
