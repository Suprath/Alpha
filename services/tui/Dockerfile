# ── Stage 1: compile .proto files → Python stubs ─────────────────────────────
FROM python:3.12-slim AS proto-builder

RUN pip install --no-cache-dir grpcio-tools protobuf

COPY shared/proto/alpha_tick.proto      /proto/alpha_tick.proto
COPY shared/proto/alpha_portfolio.proto /proto/alpha_portfolio.proto
COPY shared/proto/alpha_backtest.proto  /proto/alpha_backtest.proto

RUN mkdir -p /app && python -m grpc_tools.protoc \
        --proto_path=/proto \
        --python_out=/app \
        /proto/alpha_tick.proto \
        /proto/alpha_portfolio.proto \
        /proto/alpha_backtest.proto


# ── Stage 2: runtime image ────────────────────────────────────────────────────
FROM python:3.12-slim

RUN apt-get update && apt-get install -y \
    gcc \
    libpq-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY services/tui/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy generated proto stubs from builder stage
COPY --from=proto-builder /app/alpha_tick_pb2.py      .
COPY --from=proto-builder /app/alpha_portfolio_pb2.py .
COPY --from=proto-builder /app/alpha_backtest_pb2.py  .

COPY services/tui/ .

# Textual needs a real terminal
ENV TERM=xterm-256color
ENV COLORTERM=truecolor
ENV PYTHONUNBUFFERED=1

CMD ["python", "main.py"]
