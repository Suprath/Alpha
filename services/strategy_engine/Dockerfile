# --- Build Stage ---
FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libboost-system-dev \
    libboost-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the entire Alpha folder structure for shared headers
COPY . .

# Build the Strategy Engine
WORKDIR /app/services/strategy_engine
RUN mkdir -p build
WORKDIR /app/services/strategy_engine/build
RUN cmake .. && make -j$(nproc)

# --- Runtime Stage ---
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y \
    libboost-system1.81.0 \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the binary
COPY --from=builder /app/services/strategy_engine/build/alpha-strategy-engine /app/alpha-strategy-engine

ENTRYPOINT ["./alpha-strategy-engine"]
