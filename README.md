# Alpha: Ultra-Low Latency HFT & Backtesting Engine

This repository contains an institutional-grade, microservices-based high-frequency trading (HFT) and backtesting system.

## Project Structure

- **`infra/`**: Infrastructure-related Dockerfiles and configurations (e.g., Redis, DBs, Monitoring).
- **`services/`**: Core microservices (C++ for execution, Python for analysis/backtesting).
- **`shared/`**: Shared libraries, zero-copy IPC protocols, and common math utilities.
  - `cpp/`: Low-latency C++ headers and "Zero-Deque" core.
  - `python/`: Python wrappers and shared data models.
  - `proto/`: IPC serialization definitions (e.g., SBE or Protobuf).
- **`k8s/`**: Kubernetes manifests for production deployment.
- **`scripts/`**: Utility scripts for build, setup, and deployment.
- **`docker-compose.yml`**: Local development orchestration.

## Key Design Principles
1. **Ultra-Low Latency**: O(1) time complexity for rolling metrics and data ingestion.
2. **Zero-Copy**: Shared memory IPC across Docker containers using `ipc: host`.
3. **Containerized**: Consistent environments across development and production.
4. **Test-Driven (TDD)**: Mandatory robust test cases for every module, executed within Docker.

## Testing Architecture
- **Location**: All tests reside in the `tests/` folder.
- **Docker-Centric**: A dedicated `docker-compose.test.yml` (to be created) will handle unified testing across all C++ and Python services.
- **Standards**: GTest for C++ services and PyTest for Python components.
