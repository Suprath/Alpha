# --- Build Stage ---
FROM debian:bookworm AS builder

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libboost-system-dev \
    libboost-dev \
    libgtest-dev \
    libpqxx-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the entire Alpha folder structure for shared headers
COPY . .

# Build the Signal Engine
WORKDIR /app/services/signal_engine
RUN mkdir -p build
WORKDIR /app/services/signal_engine/build
RUN cmake .. && make -j$(nproc)
RUN ./alpha-signal-engine-test

# --- Runtime Stage ---
FROM debian:bookworm-slim AS runtime

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libboost-system1.81.0 \
    libpqxx-6.4 \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the binary
COPY --from=builder /app/services/signal_engine/build/alpha-signal-engine /app/alpha-signal-engine
COPY --from=builder /app/shared /app/shared

# Entrypoint
ENTRYPOINT ["./alpha-signal-engine"]
