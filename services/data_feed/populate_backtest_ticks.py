"""
Transform QuestDB candles → backtest_ticks via ILP.
Derives tick fields from 1-minute OHLCV bars:
  last_price = close
  vwap       = (open + high + low + close) / 4
  bid/ask    = close ± 10% of bar spread (floor: 2bps)
  volume     = bar volume (cumulative)

Runs inside Docker network: questdb:9009 (ILP), questdb:8812 (PG wire),
postgres-db:5432.
"""
import os
import socket
import sys
import psycopg2

QUESTDB_HOST     = os.getenv("QUESTDB_HOST",     "questdb")
QUESTDB_ILP_PORT = int(os.getenv("QUESTDB_ILP_PORT", "9009"))
QUESTDB_PG_PORT  = 8812
POSTGRES_HOST    = os.getenv("POSTGRES_HOST",    "postgres-db")

# ── 1. Get authoritative token map from PostgreSQL ────────────────────────────
pg_conn = psycopg2.connect(
    host=POSTGRES_HOST, port=5432, dbname="alpha_db",
    user="alpha_user", password="alpha_password",
)
pg_cur = pg_conn.cursor()
pg_cur.execute(
    "SELECT trading_symbol, id FROM instrument_universe "
    "WHERE date = (SELECT MAX(date) FROM instrument_universe)"
)
token_map = {row[0]: row[1] for row in pg_cur.fetchall()}
pg_conn.close()
print(f"[populate] Token map from PostgreSQL: {token_map}", flush=True)

# ── 2. Read candles from QuestDB via Postgres wire (port 8812) ────────────────
print(f"[populate] Connecting to QuestDB PG wire at {QUESTDB_HOST}:{QUESTDB_PG_PORT}", flush=True)
qdb_conn = psycopg2.connect(
    host=QUESTDB_HOST, port=QUESTDB_PG_PORT,
    dbname="qdb", user="admin", password="quest",
)
qdb_cur = qdb_conn.cursor()
symbols_placeholder = ",".join(f"'{s}'" for s in token_map.keys())
qdb_cur.execute(f"""
    SELECT timestamp, symbol, open, high, low, close, volume
    FROM candles
    WHERE interval = '1minute'
      AND symbol IN ({symbols_placeholder})
    ORDER BY timestamp
""")
rows = qdb_cur.fetchall()
qdb_conn.close()
print(f"[populate] Fetched {len(rows)} candle rows from QuestDB", flush=True)

if not rows:
    print("[populate] No rows found — did the historical backfill run successfully?", flush=True)
    sys.exit(1)

# ── 3. Write backtest_ticks via ILP ──────────────────────────────────────────
ilp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
ilp_sock.connect((QUESTDB_HOST, QUESTDB_ILP_PORT))
print(f"[populate] Connected to QuestDB ILP at {QUESTDB_HOST}:{QUESTDB_ILP_PORT}", flush=True)

written  = 0
skipped  = 0
per_sym  = {}

for ts_raw, symbol, open_, high, low, close, volume in rows:
    token = token_map.get(symbol, 0)
    if token == 0:
        skipped += 1
        continue

    # Convert QuestDB timestamp → nanoseconds
    if hasattr(ts_raw, "timestamp"):
        ts_ns = int(ts_raw.timestamp() * 1e9)
    else:
        ts_ns = int(ts_raw) * 1000  # QuestDB returns microseconds as int

    o, h, l, c = float(open_), float(high), float(low), float(close)
    vol = int(volume) if volume else 0

    last_price = c
    vwap       = (o + h + l + c) / 4.0
    spread     = max(h - l, c * 0.0002)   # floor at 2 bps
    bid_price  = c - spread * 0.1
    ask_price  = c + spread * 0.1

    # ILP line — no tags; all data as fields so integer filtering works in SQL
    line = (
        f"backtest_ticks "
        f"instrument_token={token}i,"
        f"last_price={last_price},"
        f"volume={vol}i,"
        f"bid_price={bid_price},"
        f"ask_price={ask_price},"
        f"vwap={vwap} "
        f"{ts_ns}\n"
    )
    ilp_sock.sendall(line.encode())
    written += 1
    per_sym[symbol] = per_sym.get(symbol, 0) + 1

ilp_sock.close()
print(f"[populate] Done — {written} rows written, {skipped} skipped", flush=True)
for sym, cnt in sorted(per_sym.items()):
    print(f"  {sym:<12} {cnt:>5} ticks", flush=True)
