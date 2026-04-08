---
name: Signal Engine Tests Location
description: Where to put signal_engine unit tests
type: feedback
---

All unit tests for signal_engine signals go into /Users/suprathps/code/Alpha/tests/unit/ (the shared tests/ folder at repo root), not into services/signal_engine/tests/.

**Why:** Consistent with the ingester tests which also live in tests/unit/ (referenced from services/ingester/CMakeLists.txt).

**How to apply:** When writing tests for any service's C++ calculators, create files under /Users/suprathps/code/Alpha/tests/unit/ and wire them up in the relevant service's CMakeLists.txt.
