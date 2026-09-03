# slim-bookworm (not bare -slim) so the build-stage glibc matches the
# debian:bookworm-slim runtime stage. The locked gstreamer/glib crates
# declare rust-version 1.92, so keep this at or above that (pinned in
# lockstep with docker/sidecar/Dockerfile).
FROM rust:1.98-slim-bookworm@sha256:1469a27c125cb5a3aebfa4f4e4665d935b02fb72cc093b2c974b3d740e43f157 AS build

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
# The default build of this image terminates no TLS in-process, so witnessd
# fails closed on a non-loopback bind unless plaintext exposure is explicitly
# acknowledged. This image binds 0.0.0.0 by design (so the published port is
# reachable) and relies on Docker network isolation / the operator's
# published-port and firewall controls for confidentiality — put a TLS
# terminator in front for untrusted networks, or rebuild with
# CARGO_FEATURES including api-tls and mount cert/key named by
# WITNESS_API_TLS_CERT / WITNESS_API_TLS_KEY to terminate in-process (the
# health check below follows that configuration). The opt-in here only takes
# effect when TLS is not active, so it is safe to leave set in TLS deployments.
ENV WITNESS_API_ALLOW_INSECURE=1
ENV RUST_LOG=info

# SECURITY: Expose only the API port. No other services should bind.
EXPOSE 8799
VOLUME ["/data"]

# SECURITY: Run as non-root with explicit UID.
USER 1001:1001

# SECURITY: Health check for orchestrators to detect stuck processes.
# Probes the Event API /health endpoint (witnessd serves it on WITNESS_API_ADDR).
# The scheme follows the TLS configuration: with WITNESS_API_TLS_CERT set (an
# api-tls build terminating in-process), port 8799 speaks only TLS, so the
# probe must too. -k because the operator's cert is typically self-signed or
# not minted for 127.0.0.1 — this is a same-container liveness probe, not a
# trust decision.
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
  CMD if [ -n "$WITNESS_API_TLS_CERT" ]; then \
        curl -fsSk https://127.0.0.1:8799/health || exit 1; \
      else \
        curl -fsS http://127.0.0.1:8799/health || exit 1; \
      fi

ENTRYPOINT ["witnessd"]
