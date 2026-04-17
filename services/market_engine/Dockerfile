# --- Build Stage ---
FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libboost-system-dev \
    libboost-dev \
    libhiredis-dev \
    libprotobuf-dev \
    protobuf-compiler \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the entire Alpha folder structure for shared headers
COPY . .

# Build the Market Engine
WORKDIR /app/services/market_engine
RUN mkdir -p build
WORKDIR /app/services/market_engine/build
RUN cmake .. && make -j$(nproc)

# --- Runtime Stage ---
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y \
    libboost-system1.81.0 \
    libhiredis0.14 \
    libprotobuf32 \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the binary
COPY --from=builder /app/services/market_engine/build/alpha-market-engine /app/alpha-market-engine

# Default starting capital: ₹10,00,000
ENV STARTING_CAPITAL=1000000

ENTRYPOINT ["./alpha-market-engine"]
