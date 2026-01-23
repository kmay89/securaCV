FROM rust:1.77-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
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

ARG CARGO_FEATURES=rtsp-gstreamer
RUN cargo build --release --features "${CARGO_FEATURES}"

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    gstreamer1.0-libav \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    libgstreamer1.0-0 \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /data

COPY --from=build /app/target/release/witnessd /usr/local/bin/witnessd

ENV WITNESS_API_ADDR=0.0.0.0:8799
ENV RUST_LOG=info

EXPOSE 8799
VOLUME ["/data"]

ENTRYPOINT ["witnessd"]
