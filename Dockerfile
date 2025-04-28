# ─── Stage 1: build with g++ ────────────────────────────────────────────────────
FROM ubuntu:22.04 AS builder

# install compiler & tools
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# copy your source tree
COPY src/ ./src/

# compile all .cpp files in src/ into one binary called 'custom-redis'
# note: -pthread is required for <thread>
RUN g++ -std=c++17 -O2 src/*.cpp -o custom-redis -pthread

# ─── Stage 2: runtime ───────────────────────────────────────────────────────────
FROM ubuntu:22.04

# only need the C++ runtime
RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 \
 && rm -rf /var/lib/apt/lists/*

# pull in the compiled binary
COPY --from=builder /app/custom-redis /usr/local/bin/custom-redis

EXPOSE 6379
ENTRYPOINT ["/usr/local/bin/custom-redis"]
