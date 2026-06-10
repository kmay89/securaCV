# SecuraCV Frigate sidecar
#
# A single container for standalone (non-Home-Assistant) Frigate users:
# witness_api + frigate_bridge + event_mqtt_bridge supervised by
# entrypoint.sh, plus log_verify and a `doctor` diagnostic subcommand.
# None of these binaries ingest video, so no GStreamer is needed —
# this image is much smaller than the witnessd one.
#
# Build (repo root):  docker build -f docker/sidecar/Dockerfile -t securacv-sidecar .
# Run:                see docker/sidecar/quickstart.compose.yml

FROM rust:1.77-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    libssl-dev \
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

# mosquitto-clients powers the `doctor` subcommand's broker checks.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3 \
    mosquitto-clients \
  && rm -rf /var/lib/apt/lists/*

RUN groupadd --system --gid 1001 witness && \
    useradd --system --uid 1001 --gid witness --home-dir /data --no-create-home witness && \
    mkdir -p /data && chown witness:witness /data

COPY --from=build /app/target/release/witness_api /usr/local/bin/
COPY --from=build /app/target/release/frigate_bridge /usr/local/bin/
COPY --from=build /app/target/release/event_mqtt_bridge /usr/local/bin/
COPY --from=build /app/target/release/log_verify /usr/local/bin/
COPY docker/sidecar/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENV RUST_LOG=info

WORKDIR /data
VOLUME ["/data"]

USER 1001:1001

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
