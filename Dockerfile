# slim-bookworm (not bare -slim) so the build-stage glibc matches the
# debian:bookworm-slim runtime stage. The locked gstreamer/glib crates
# declare rust-version 1.92, so keep this at or above that (pinned in
# lockstep with docker/sidecar/Dockerfile).
FROM rust:1.97-slim-bookworm@sha256:96c0af8cf054fd006435089f0076729716784ec9be485bd655de59c55df105ce AS build

# libssl-dev: the bundled SQLCipher (rusqlite bundled-sqlcipher) compiles
# against OpenSSL headers and links libcrypto dynamically.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
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

ARG CARGO_FEATURES=rtsp-gstreamer
RUN cargo build --release --features "${CARGO_FEATURES}"

FROM debian:bookworm-slim@sha256:abd67ffcfa541b485a3dff59865ab629aa048a6c613e639d36e7456b0b229241

# libssl3 provides the libcrypto.so.3 the SQLCipher-linked binary loads at
# runtime (listed explicitly rather than relying on transitive dependencies).
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    gstreamer1.0-libav \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    libgstreamer1.0-0 \
    libseccomp2 \
    libssl3 \
  && rm -rf /var/lib/apt/lists/*

RUN groupadd --system --gid 1001 witness && \
    useradd --system --uid 1001 --gid witness --home-dir /data --no-create-home witness && \
    mkdir -p /data && chown witness:witness /data

WORKDIR /data

COPY --from=build /app/target/release/witnessd /usr/local/bin/witnessd
# Operator tooling: verify, export, and manage break-glass on the sealed
# log/vault this container owns under /data, without copying the encrypted
# database out of the volume. Already compiled by the build above (none of
# these are feature-gated); adds binaries only, no extra runtime deps and
# no new listening ports.
COPY --from=build /app/target/release/log_verify /usr/local/bin/log_verify
COPY --from=build /app/target/release/export_events /usr/local/bin/export_events
COPY --from=build /app/target/release/export_verify /usr/local/bin/export_verify
COPY --from=build /app/target/release/envelope_verify /usr/local/bin/envelope_verify
COPY --from=build /app/target/release/break_glass /usr/local/bin/break_glass

ENV WITNESS_API_ADDR=0.0.0.0:8799
ENV RUST_LOG=info

# SECURITY: Expose only the API port. No other services should bind.
EXPOSE 8799
VOLUME ["/data"]

# SECURITY: Run as non-root with explicit UID.
USER 1001:1001

# SECURITY: Health check for orchestrators to detect stuck processes.
# Probes the Event API /health endpoint (witnessd serves it on WITNESS_API_ADDR).
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8799/health || exit 1

ENTRYPOINT ["witnessd"]
