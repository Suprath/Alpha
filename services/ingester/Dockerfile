# ── Build stage ───────────────────────────────────────────────────────────────
FROM debian:bookworm AS builder

LABEL stage=builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libboost-system-dev \
    libboost-dev \
    libgtest-dev \
    libhiredis-dev \
    nlohmann-json3-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the full repo (shared headers + tests live outside the service dir)
COPY . .

WORKDIR /app/services/ingester
RUN mkdir -p build

WORKDIR /app/services/ingester/build
RUN cmake .. && make -j$(nproc)

# Run unit tests; image build fails if any test fails
RUN ./alpha-ingester-test


# ── Runtime stage ─────────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y \
    libboost-system1.81.0 \
    libhiredis0.14 \
    ca-certificates \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /app/services/ingester/build/alpha-ingester      /app/alpha-ingester
COPY --from=builder /app/services/ingester/build/alpha-ingester-test /app/alpha-ingester-test

EXPOSE 9000

ENTRYPOINT ["./alpha-ingester"]
