"""
data_feed entry point
─────────────────────────────────────────────────────────────────────────────
Reads MODE env var and runs the appropriate feed module:

  MODE=live        → UpstoxLiveFeed   (WebSocket → Redis Stream alpha:ticks)
  MODE=historical  → UpstoxHistoricalFeed (REST API → QuestDB ILP)

Required env vars:
  UPSTOX_ACCESS_TOKEN   — Upstox OAuth2 access token
  INGEST_SYMBOLS        — Comma-separated instrument keys (live mode)
  BACKFILL_INTERVAL     — Candle interval, e.g. "1minute" (historical mode)
  BACKFILL_FROM_DATE    — Start date, e.g. "2024-01-01"  (historical mode)
  BACKFILL_TO_DATE      — End date (default: today)       (historical mode)
"""
import asyncio
import logging
import os
import sys
from datetime import date

import psycopg2

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s %(message)s",
    stream=sys.stdout,
)
logger = logging.getLogger("data_feed")


def _require(name: str) -> str:
    val = os.getenv(name, "").strip()
    if not val:
        logger.critical("Required env var %s is not set — cannot start", name)
        sys.exit(1)
    return val


def _load_token_map() -> dict[str, int]:
    """Load instrument_key → DB id mapping from PostgreSQL (latest date)."""
    pg_host = os.getenv("POSTGRES_HOST", "postgres-db")
    pg_port = int(os.getenv("POSTGRES_PORT", "5432"))
    pg_db   = os.getenv("POSTGRES_DB",   "alpha_db")
    pg_user = os.getenv("POSTGRES_USER",  "alpha_user")
    pg_pass = os.getenv("POSTGRES_PASSWORD", "alpha_password")

    conn = psycopg2.connect(
        host=pg_host, port=pg_port, dbname=pg_db,
        user=pg_user, password=pg_pass,
    )
    try:
        cur = conn.cursor()
        cur.execute(
            "SELECT instrument_key, id FROM instrument_universe "
            "WHERE date = (SELECT MAX(date) FROM instrument_universe)"
        )
        token_map = {row[0]: row[1] for row in cur.fetchall()}
        logger.info("[main] token_map: %d instruments loaded from PostgreSQL", len(token_map))
        return token_map
    finally:
        conn.close()


async def run_live() -> None:
    from live_feed import UpstoxLiveFeed

    access_token    = _require("UPSTOX_ACCESS_TOKEN")
    instrument_keys = [
        k.strip()
        for k in _require("INGEST_SYMBOLS").split(",")
        if k.strip()
    ]
    token_map = _load_token_map()

    logger.info("[main] Starting live feed for %d instruments", len(instrument_keys))
    feed = UpstoxLiveFeed(access_token, instrument_keys, token_map)
    await feed.run()


async def run_historical() -> None:
    from historical_feed import UpstoxHistoricalFeed

    access_token = _require("UPSTOX_ACCESS_TOKEN")
    interval     = os.getenv("BACKFILL_INTERVAL",  "1minute")
    from_date    = os.getenv("BACKFILL_FROM_DATE", "2024-01-01")
    to_date      = os.getenv("BACKFILL_TO_DATE",   date.today().strftime("%Y-%m-%d"))

    logger.info(
        "[main] Starting historical feed: interval=%s from=%s to=%s",
        interval, from_date, to_date,
    )
    feed = UpstoxHistoricalFeed(access_token, interval, from_date, to_date)
    await feed.run()


if __name__ == "__main__":
    mode = os.getenv("MODE", "live").lower()
    logger.info("=== Alpha data_feed starting in MODE=%s ===", mode)

    if mode == "live":
        asyncio.run(run_live())
    elif mode == "historical":
        asyncio.run(run_historical())
    else:
        logger.critical("Unknown MODE=%s — expected: live | historical", mode)
        sys.exit(1)
