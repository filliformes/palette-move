# Native (x86_64) toolchain for the offline DSP audit harness (tests/fx_audit.c).
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y gcc g++ dos2unix && rm -rf /var/lib/apt/lists/*
WORKDIR /build
