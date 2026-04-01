# --- Build Stage ---
FROM gcc:13 AS builder

LABEL stage=builder

# Install dependencies (Cmake, Boost)
RUN apt-get update && apt-get install -y \
    cmake \
    libboost-system-dev \
    libboost-dev \
    libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the entire Alpha folder structure for shared headers (Mono-repo approach)
COPY . .

# Build the ingester
WORKDIR /app/services/ingester
RUN mkdir -p build && cd build && \
    cmake .. && \
    make -j$(nproc)

# --- Runtime Stage ---
FROM debian:bookworm-slim AS runtime

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libboost-system1.81.0 \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the binary from the builder stage
COPY --from=builder /app/services/ingester/build/alpha-ingester /app/alpha-ingester

# Expose TCP ingestion port
EXPOSE 9000

# Entrypoint
ENTRYPOINT ["./alpha-ingester"]
