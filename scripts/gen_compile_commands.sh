#!/usr/bin/env bash
# Generate compile_commands.json for all C++ services by running CMake inside
# their Docker builder stage. Output lands in each service directory so that
# CLion / clangd picks it up automatically.
#
# Usage:
#   ./scripts/gen_compile_commands.sh                  # all services
#   ./scripts/gen_compile_commands.sh market_engine    # one service

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SERVICES=(
    market_engine
    backtest_engine
    signal_engine
    backfill_engine
    ingester
    strategy_engine
)

# If a service name is passed, restrict to that one
if [[ $# -gt 0 ]]; then
    SERVICES=("$1")
fi

for SVC in "${SERVICES[@]}"; do
    SVC_DIR="$REPO_ROOT/services/$SVC"
    DOCKERFILE="$SVC_DIR/Dockerfile"

    if [[ ! -f "$DOCKERFILE" ]]; then
        echo "  [skip] $SVC — no Dockerfile found"
        continue
    fi

    IMAGE="alpha-${SVC//_/-}-builder:compile-db"
    echo ""
    echo "==> $SVC"

    # Build only the 'builder' stage (uses cache, fast on subsequent runs)
    echo "    building image..."
    docker build \
        --target builder \
        --tag "$IMAGE" \
        --file "$DOCKERFILE" \
        "$REPO_ROOT" \
        --quiet

    # Run cmake configure-only (no make) to produce compile_commands.json,
    # then copy it back to the host service directory via stdout.
    echo "    extracting compile_commands.json..."
    docker run --rm "$IMAGE" \
        bash -c "cmake /app/services/${SVC} -B /tmp/ccdb -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null 2>&1 && cat /tmp/ccdb/compile_commands.json" \
        | sed "s|/app|${REPO_ROOT}|g" \
        > "$SVC_DIR/compile_commands.json"

    echo "    written → services/$SVC/compile_commands.json"
done

echo ""
echo "Done. Reload clangd in your IDE to pick up the changes:"
echo "  CLion  : File → Invalidate Caches / Restart"
echo "  VS Code: Ctrl+Shift+P → 'clangd: Restart Language Server'"