# ── Build Stage ──────────────────────────────────────────────────────────
FROM debian:bookworm AS builder

LABEL stage=builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libpqxx-dev \
    libhiredis-dev \
    protobuf-compiler \
    libprotobuf-dev \
    libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

WORKDIR /app/services/backtest_engine
RUN mkdir -p build
WORKDIR /app/services/backtest_engine/build
RUN cmake .. && make -j$(nproc)
RUN ./alpha-backtest-test

# ── Runtime Stage ─────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y \
    libpqxx-6.4 \
    libhiredis0.14 \
    libprotobuf32 \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/services/backtest_engine/build/alpha-backtest /app/alpha-backtest

# Data directory for .alpha flat files (mmap source)
VOLUME ["/data/backtest"]

# Default: run backtest. Override CMD to "export" for data export mode.
ENTRYPOINT ["./alpha-backtest"]
CMD ["run"]
