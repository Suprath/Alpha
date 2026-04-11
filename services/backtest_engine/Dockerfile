# ── Build Stage ──────────────────────────────────────────────────────────
FROM debian:bookworm AS builder

LABEL stage=builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libpqxx-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

WORKDIR /app/services/backtest_engine
RUN mkdir -p build
WORKDIR /app/services/backtest_engine/build
RUN cmake .. && make -j$(nproc)

# ── Runtime Stage ─────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y \
    libpqxx-dev \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/services/backtest_engine/build/alpha-backtest /app/alpha-backtest

# Data directory for .alpha flat files (mmap source)
VOLUME ["/data/backtest"]

# Default: run backtest. Override CMD to "export" for data export mode.
ENTRYPOINT ["./alpha-backtest"]
CMD ["run"]
