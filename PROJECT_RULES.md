# Project Rules for Alpha

This document defines the core engineering standards and development practices for the **Alpha** project. All contributions must adhere to these rules.

## 1. Test-Driven Development (TDD)
- **Mandatory Coverage**: Every new module or critical function MUST have a robust test case.
- **Location**: All test cases must be placed in the `tests/` directory.
- **Execution**: Tests MUST run exclusively inside Docker containers to ensure environment parity.
- **Failure**: Any PR lacking appropriate test coverage or failing in Docker will be rejected.

## 2. Ultra-Low Latency Standards
- **Complexity**: Aim for **O(1)** or constant-time complexity in all market-data ingestion and execution paths.
- **Memory Management**: 
  - Zero heap allocations in the critical hot path. 
  - Prefer stack allocations, pre-allocated memory pools, or the "Zero-Deque" core architecture.
- **Cache Locality**: Design data structures to be cache-line friendly to minimize cache misses.

## 3. Language & Tools
- **C++**: Target **C++20** for performance features (coroutines, concepts) and better type safety.
- **Python**: Use for non-critical analysis and backtesting. Use performance-optimized libraries (NumPy, Cython) where necessary.
- **Messaging**: IPC Must use zero-copy mechanisms (shared memory) for inter-service communication.

## 4. Documentation
- Keep the `README.md` and architecture diagrams up to date.
- Document any hardware-specific optimizations (e.g., AVX-512, SIMD).
