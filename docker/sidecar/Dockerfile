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

# Needs a toolchain that satisfies the workspace MSRV (Cargo.lock v4 needs
# 1.78+, the rumqttc-next family needs 1.89+); pinned to a recent stable.
# The -bookworm variant matters: the runtime stage below is bookworm, and
# the binaries must link against the same glibc/libssl generation.
FROM rust:1.97-slim-bookworm@sha256:96c0af8cf054fd006435089f0076729716784ec9be485bd655de59c55df105ce AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    libseccomp-dev \
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
COPY README.md LICENSE CHANGELOG.md CONTRIBUTING.md SECURITY.md ./

RUN cargo build --release \
    --bin witness_api \
    --bin frigate_bridge \
    --bin event_mqtt_bridge \
    --bin log_verify

FROM debian:bookworm-slim@sha256:abd67ffcfa541b485a3dff59865ab629aa048a6c613e639d36e7456b0b229241

# mosquitto-clients powers the `doctor` subcommand's broker checks.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libseccomp2 \
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
