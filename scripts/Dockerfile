FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    make \
    dos2unix \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
