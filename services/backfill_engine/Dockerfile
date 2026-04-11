# ── Build Stage ──────────────────────────────────────────────────────────
FROM debian:bookworm AS builder

LABEL stage=builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libboost-system-dev \
    libboost-dev \
    libssl-dev \
    libpqxx-dev \
    nlohmann-json3-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

WORKDIR /app/services/backfill_engine
RUN mkdir -p build
WORKDIR /app/services/backfill_engine/build
RUN cmake .. && make -j$(nproc)

# ── Runtime Stage ─────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y \
    libboost-system1.81.0 \
    libssl3 \
    libpqxx-dev \
    ca-certificates \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/services/backfill_engine/build/alpha-backfill /app/alpha-backfill

ENTRYPOINT ["./alpha-backfill"]
