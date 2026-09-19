# armv7 (hard-float) toolchain for driving the site package's ARM binaries.
FROM --platform=linux/arm/v7 debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        gcc g++ libc6-dev python3 libstdc++6 file nlohmann-json3-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /work
