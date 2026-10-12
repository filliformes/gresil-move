FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    g++ \
    make \
    dos2unix \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY . .

RUN find . -name '*.sh' -exec dos2unix {} + && \
    chmod +x scripts/*.sh && \
    bash scripts/test.sh && \
    CROSS_PREFIX=aarch64-linux-gnu- bash scripts/build.sh
