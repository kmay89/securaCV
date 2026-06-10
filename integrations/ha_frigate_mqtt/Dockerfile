# SecuraCV image for the local Frigate + MQTT + Home Assistant compose stack.
#
# Ships only the MQTT-pipeline binaries (witness_api, frigate_bridge,
# event_mqtt_bridge) plus log_verify for operator integrity checks. There is
# no RTSP ingestion in this stack — Frigate owns the cameras — so the build
# skips the GStreamer feature and the runtime image carries no media stack.
#
# Build from the repo root (compose does this via context: ../..):
#   docker build -f integrations/ha_frigate_mqtt/Dockerfile -t securacv-ha-frigate:local .

# slim-bookworm (not bare -slim) so the build-stage glibc matches the
# debian:bookworm-slim runtime stage. Cargo >= 1.78 is required by this
# repo's lock file (version 4).
FROM rust:1.90-slim-bookworm AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    libseccomp-dev \
    pkg-config \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY Cargo.toml Cargo.lock ./
COPY src ./src
COPY kernel ./kernel
COPY spec ./spec
COPY docs ./docs
COPY examples ./examples
COPY scripts ./scripts
COPY tests ./tests
COPY spec.md log_verify_README.md README.md LICENSE CHANGELOG.md CONTRIBUTING.md SECURITY.md why_this_matters.md ./

RUN cargo build --release \
    --bin witness_api \
    --bin frigate_bridge \
    --bin event_mqtt_bridge \
    --bin log_verify

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    libseccomp2 \
  && rm -rf /var/lib/apt/lists/*

RUN groupadd --system --gid 1001 witness && \
    useradd --system --uid 1001 --gid witness --home-dir /data --no-create-home witness && \
    mkdir -p /data && chown witness:witness /data

WORKDIR /data

COPY --from=build /app/target/release/witness_api /usr/local/bin/witness_api
COPY --from=build /app/target/release/frigate_bridge /usr/local/bin/frigate_bridge
COPY --from=build /app/target/release/event_mqtt_bridge /usr/local/bin/event_mqtt_bridge
COPY --from=build /app/target/release/log_verify /usr/local/bin/log_verify

ENV WITNESS_API_ADDR=127.0.0.1:8799
ENV RUST_LOG=info

VOLUME ["/data"]

# SECURITY: Run as non-root with explicit UID.
USER 1001:1001

# Probes the Event API /health endpoint served by witness_api.
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8799/health || exit 1

# The compose file supplies the actual command (witness_api + bridges).
CMD ["witness_api"]
