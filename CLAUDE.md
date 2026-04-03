# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Alpha** is an institutional-grade, ultra-low-latency HFT (High-Frequency Trading) and backtesting engine targeting Indian equity markets (NSE via Upstox). It uses C++20 microservices communicating over POSIX shared memory for sub-microsecond IPC, with Python for data preparation.

## Critical: Everything Runs in Docker

**Nothing runs on the local machine.** All services, tests, and scripts must be built and executed inside Docker containers. Always use `docker-compose` workflows — never suggest running binaries or scripts directly on the host.

## Build Commands

All services are built with CMake (3.20+) requiring C++20:

```bash
# Build ingester (run from repo root or service dir)
cd services/ingester && mkdir -p build && cd build
cmake .. && make -j$(nproc)

# Build signal engine
cd services/signal_engine && mkdir -p build && cd build
cmake .. && make -j$(nproc)

# Docker build (preferred - ensures environment parity)
docker-compose build
docker-compose build ingester       # Build specific service
docker-compose up -d                # Start all services
```

## Running Tests

**Tests must run inside Docker for environment parity** — PRs without Docker-passing tests are rejected.

```bash
# C++ unit tests (GTest) - built as part of CMake
cd services/ingester/build
./alpha-ingester-test               # Run all ingester unit tests

# Run a single test by name
./alpha-ingester-test --gtest_filter=RingBufferTest.OverflowBehavior

# Python tests (instrument_worker)
cd services/instrument_worker
pytest tests/

# Docker-based testing (canonical)
docker-compose build ingester       # Tests run during image build
```

Test files live in `tests/unit/` (GTest), `tests/integration/`, `tests/performance/`.

## Architecture

### Data Flow

```
Upstox WebSocket (protobuf)
        ↓
   Ingester (C++)           — deserializes protobuf, normalizes to Tick structs
        ↓
  Shared Memory Ring Buffer — zero-copy, lock-free SPSC, 64-byte cache-line aligned
        ↓
  Signal Engine (C++)       — consumes ticks, computes signals, spins on dedicated core
        ↓
  Redis / PostgreSQL / QuestDB
```

### Services

| Service | Language | Role |
|---|---|---|
| `ingester` | C++ | Upstox WebSocket → shared memory ring buffer |
| `signal-engine` | C++ | Shared memory consumer → signal processing |
| `instrument-worker` | Python | Daily job: NSE instrument universe → PostgreSQL |

### Shared Memory IPC

`ipc: host` in docker-compose gives ingester and signal-engine access to the same `/dev/shm`. Ingester writes `Tick` structs (defined in `shared/cpp/include/alpha/MarketModels.hpp`) directly to the ring buffer; signal-engine attaches via `ShmManager` (Boost.Interprocess wrapper in `shared/cpp/include/alpha/ShmManager.hpp`). No serialization in the hot path.

### Key Shared Headers (`shared/cpp/include/alpha/`)

- **RingBuffer.hpp** — Lock-free SPSC ring buffer. Power-of-2 capacity, drop-oldest on overflow, memory-ordered atomics, no heap allocation.
- **MarketModels.hpp** — POD structs (`Tick`, `Candle`) used for zero-copy shared memory transfers.
- **ShmManager.hpp** — Lifecycle management for the shared memory segment.
- **Config.hpp** — Thread-safe environment-variable-based config.
- **RateLimiter.hpp** — Token-bucket rate limiter for Upstox API calls.
- **Timestamp.hpp** — IST-aware high-resolution timestamps.
- **SpinLock.hpp** — Busy-wait spinlock for the lowest-latency critical sections.

### Protobuf

`shared/proto/` contains `.proto` definitions (e.g., `market_data_v3.proto`). Protobuf is used **only** for deserializing incoming Upstox wire messages — not for internal IPC.

## Performance Constraints

The hot path (ingester → ring buffer → signal engine) has strict latency requirements:

- **No heap allocations** in the tick processing path — use pre-allocated pools or stack allocation.
- **O(1) operations only** in critical sections.
- Signal engine runs a **busy-spin loop** at 100% CPU on a dedicated core — do not add blocking calls or sleeps.
- All hot-path data structures must be **cache-line aligned** (64 bytes) to prevent false sharing.

## Environment Setup

Copy `.env.example` to `.env` and fill in Upstox API credentials. The `TEST_MODE=1` flag on `instrument-worker` limits the instrument universe to 3 mock instruments (NIFTY, RELIANCE, BANKNIFTY) for local dev.

## Infrastructure

- **QuestDB** — time-series OLAP for tick/OHLCV storage (port 9000 UI, 9009 ILP ingest)
- **PostgreSQL** — relational store for instrument metadata (port 5432)
- **Redis** — custom build in `infra/redis/`, used for pub-sub and caching (localhost:6379 only)
- **pgAdmin** — PostgreSQL UI at port 5050

Kubernetes manifests for production deployment are in `k8s/`.
