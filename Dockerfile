# Build environment for Magic Lantern and the Soup module. The sources are
# bind-mounted from the host, so this image only holds the toolchain.
FROM debian:bookworm-slim

RUN apt-get update && \
    apt-get install -qy --no-install-recommends \
        build-essential python3 python3-docutils git ca-certificates zip \
        gcc-arm-none-eabi libnewlib-arm-none-eabi \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
