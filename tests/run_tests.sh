#!/bin/bash
set -e

echo "--- Alpha Testing Pipeline Starting ---"

# 1. Run C++ Unit Tests
echo "[run_tests] Running C++ Unit Tests (Ingester)..."
/app/services/ingester/build/alpha-ingester-test

# 2. Run Python Unit Tests
echo "[run_tests] Running Python Unit Tests..."
export PYTHONPATH=$PYTHONPATH:.
pytest tests/

echo "--- All Tests Completed Successfully ---"
