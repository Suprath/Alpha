# --- Build Stage ---
FROM debian:bookworm AS builder

LABEL stage=builder

# Install dependencies (C++ compiler, Cmake, Boost, GTest, Protobuf, Postgres Libs)
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libboost-system-dev \
    libboost-dev \
    libgtest-dev \
    libprotobuf-dev \
    protobuf-compiler \
    nlohmann-json3-dev \
    libpqxx-dev \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the entire Alpha folder structure for shared headers (Mono-repo approach)
COPY . .

# Build the ingester
WORKDIR /app/services/ingester
RUN mkdir -p build
WORKDIR /app/services/ingester/build
RUN cmake .. && make -j$(nproc)
RUN ./alpha-ingester-test

# --- Runtime Stage ---
FROM debian:bookworm-slim AS runtime

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libboost-system1.81.0 \
    libprotobuf32 \
    libpqxx-dev \
    libssl3 \
    ca-certificates \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the binaries from the builder stage
COPY --from=builder /app/services/ingester/build/alpha-ingester /app/alpha-ingester
COPY --from=builder /app/services/ingester/build/alpha-ingester-test /app/alpha-ingester-test

# Expose TCP ingestion port
EXPOSE 9000

# Entrypoint
ENTRYPOINT ["./alpha-ingester"]
