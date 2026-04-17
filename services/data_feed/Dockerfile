# ── Stage 1: compile .proto files → Python stubs ─────────────────────────────
FROM python:3.12-slim AS proto-builder

# grpc_tools.protoc is the most reliable way to generate Python protobuf stubs
RUN pip install --no-cache-dir grpcio-tools protobuf

COPY shared/proto/market_data_v3.proto /proto/market_data_v3.proto
COPY shared/proto/alpha_tick.proto     /proto/alpha_tick.proto

RUN mkdir -p /app && python -m grpc_tools.protoc \
        --proto_path=/proto \
        --python_out=/app \
        /proto/market_data_v3.proto \
        /proto/alpha_tick.proto


# ── Stage 2: runtime image ────────────────────────────────────────────────────
FROM python:3.12-slim AS runtime

WORKDIR /app

# Install Python dependencies
COPY services/data_feed/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy generated proto stubs from builder stage
COPY --from=proto-builder /app/market_data_v3_pb2.py .
COPY --from=proto-builder /app/alpha_tick_pb2.py      .

# Copy feed modules
COPY services/data_feed/main.py .
COPY services/data_feed/live_feed.py .
COPY services/data_feed/historical_feed.py .
COPY services/data_feed/populate_backtest_ticks.py .

# MODE=live (default) | historical
ENV MODE=live

CMD ["python", "main.py"]
