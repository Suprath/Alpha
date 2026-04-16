"""
Upstox v2 Historical Market Data Feed
─────────────────────────────────────────────────────────────────────────────
1. Loads instrument universe from PostgreSQL (latest date)
2. Fetches OHLCV candles from Upstox REST API with date chunking per interval
3. Applies a sliding-window rate limiter (80 req/min) to stay under API limits
4. Writes candles to QuestDB via ILP (Line Protocol) over TCP

Interval → max days per request (≤ ~1900 bars, matching C++ backfill engine):
  1minute  → 4 days     (375 bars/day × 4 = 1500)
  5minute  → 24 days    ( 75 bars/day × 24 = 1800)
  15minute → 75 days    ( 25 bars/day × 75 = 1875)
  30minute → 150 days   ( 12 bars/day × 150 = 1800)
  60minute → 300 days   (  6 bars/day × 300 = 1800)
  day/week/month → no chunking (single call)
"""
import asyncio
import logging
import os
import socket
import time
from collections import deque
from datetime import date, datetime, timedelta, timezone
from typing import Optional

import aiohttp
import psycopg2

logger = logging.getLogger(__name__)

UPSTOX_HISTORY_URL = (
    "https://api.upstox.com/v2/historical-candle"
    "/{instrument_key}/{interval}/{to_date}/{from_date}"
)
QUESTDB_HOST = os.getenv("QUESTDB_HOST", "questdb")
QUESTDB_PORT = int(os.getenv("QUESTDB_ILP_PORT", "9009"))
MAX_REQUESTS_PER_MIN = 66   # Upstox free tier: 500/min but 2000/30min window → safe cap is 66/min


# ── Rate limiter ──────────────────────────────────────────────────────────────

class SlidingWindowThrottler:
    """
    Rate limiter using a sliding window of request timestamps.
    Never allows more than max_per_minute calls within any 60-second window.
    """

    def __init__(self, max_per_minute: int = 80) -> None:
        self._max = max_per_minute
        self._timestamps: deque[float] = deque()

    async def acquire(self) -> None:
        now = time.monotonic()
        # Evict timestamps older than 60 seconds
        while self._timestamps and now - self._timestamps[0] > 60.0:
            self._timestamps.popleft()

        if len(self._timestamps) >= self._max:
            # Sleep until the oldest entry ages out of the window
            wait = 60.0 - (now - self._timestamps[0]) + 0.05  # 50 ms safety buffer
            if wait > 0:
                logger.info("[HistFeed] Rate limit hit — sleeping %.2fs", wait)
                await asyncio.sleep(wait)
            # Re-evict after sleep
            now = time.monotonic()
            while self._timestamps and now - self._timestamps[0] > 60.0:
                self._timestamps.popleft()

        self._timestamps.append(time.monotonic())


# ── Date-chunking helpers ─────────────────────────────────────────────────────

def _chunk_days_for_interval(interval: str) -> int:
    """Max calendar days per API request for each intraday interval."""
    return {
        "1minute":  4,
        "5minute":  24,
        "15minute": 75,
        "30minute": 150,
        "60minute": 300,
        "1hour":    300,
    }.get(interval, 0)   # 0 → no chunking (day / week / month)


def _date_chunks(from_date: str, to_date: str, chunk_days: int):
    """Yield (chunk_from, chunk_to) date-string pairs for the given range."""
    fmt = "%Y-%m-%d"
    d_from = datetime.strptime(from_date, fmt).date()
    d_to   = datetime.strptime(to_date,   fmt).date()

    if chunk_days == 0:
        yield from_date, to_date
        return

    cursor = d_from
    while cursor <= d_to:
        chunk_end = min(cursor + timedelta(days=chunk_days - 1), d_to)
        yield cursor.strftime(fmt), chunk_end.strftime(fmt)
        cursor = chunk_end + timedelta(days=1)


# ── Main class ────────────────────────────────────────────────────────────────

class UpstoxHistoricalFeed:
    def __init__(
        self,
        access_token: str,
        interval: str,
        from_date: str,
        to_date: Optional[str] = None,
    ) -> None:
        self.access_token = access_token
        self.interval     = interval
        self.from_date    = from_date
        self.to_date      = to_date or date.today().strftime("%Y-%m-%d")
        self._throttler   = SlidingWindowThrottler(MAX_REQUESTS_PER_MIN)
        self._qdb_sock: Optional[socket.socket] = None

    # ── QuestDB ILP ───────────────────────────────────────────────────────────

    def _connect_questdb(self) -> None:
        self._qdb_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._qdb_sock.connect((QUESTDB_HOST, QUESTDB_PORT))
        logger.info("[HistFeed] QuestDB connected at %s:%d", QUESTDB_HOST, QUESTDB_PORT)

    def _write_candle(self, candle: dict, symbol: str) -> None:
        """Write one candle row to QuestDB via ILP line protocol."""
        ts_ns = candle["timestamp_ns"]
        line = (
            f"candles,symbol={symbol},interval={self.interval} "
            f"open={candle['open']},"
            f"high={candle['high']},"
            f"low={candle['low']},"
            f"close={candle['close']},"
            f"volume={candle['volume']}i,"
            f"open_interest={candle['open_interest']}i "
            f"{ts_ns}\n"
        )
        self._qdb_sock.sendall(line.encode())

    # ── PostgreSQL instrument lookup ──────────────────────────────────────────

    @staticmethod
    def _load_instruments() -> list[dict]:
        """Return all rows from the latest date in instrument_universe."""
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
                "SELECT instrument_key, trading_symbol "
                "FROM instrument_universe "
                "WHERE date = (SELECT MAX(date) FROM instrument_universe)"
            )
            return [{"instrument_key": r[0], "trading_symbol": r[1]}
                    for r in cur.fetchall()]
        finally:
            conn.close()

    # ── HTTP fetch ────────────────────────────────────────────────────────────

    async def _fetch_chunk(
        self,
        session: aiohttp.ClientSession,
        instrument_key: str,
        chunk_from: str,
        chunk_to: str,
    ) -> list:
        """Fetch one date chunk from the Upstox historical-candle endpoint."""
        await self._throttler.acquire()
        url = UPSTOX_HISTORY_URL.format(
            instrument_key=instrument_key,
            interval=self.interval,
            from_date=chunk_from,
            to_date=chunk_to,
        )
        headers = {
            "Authorization": f"Bearer {self.access_token}",
            "Api-Version":   "2.0",
            "Accept":        "application/json",
        }
        async with session.get(
            url, headers=headers, timeout=aiohttp.ClientTimeout(total=15)
        ) as resp:
            body = await resp.json()
            if resp.status != 200:
                raise RuntimeError(
                    f"HTTP {resp.status} for {instrument_key} "
                    f"[{chunk_from}→{chunk_to}]: {body}"
                )
            if body.get("status") != "success":
                raise RuntimeError(
                    f"API non-success for {instrument_key}: {body}"
                )
            return body["data"]["candles"]

    @staticmethod
    def _parse_candle(row: list) -> dict:
        """
        Convert an Upstox candle row to a normalised dict with timestamp_ns.
        Row format: [iso8601_ts, open, high, low, close, volume, oi?]
        Timestamps are in IST (+05:30); we convert to UTC epoch nanoseconds.
        """
        ts_str = row[0]
        try:
            dt_naive = datetime.strptime(ts_str[:19], "%Y-%m-%dT%H:%M:%S")
            # Parse the timezone offset (e.g. "+05:30" or "+0530")
            tz_part = ts_str[19:].strip()
            sign = 1
            if tz_part.startswith("-"):
                sign = -1
                tz_part = tz_part[1:]
            elif tz_part.startswith("+"):
                tz_part = tz_part[1:]
            tz_digits = tz_part.replace(":", "").zfill(4)
            tz_h, tz_m = int(tz_digits[:2]), int(tz_digits[2:4])
            offset = sign * timedelta(hours=tz_h, minutes=tz_m)
            dt_utc = dt_naive.replace(tzinfo=timezone.utc) - offset
            ts_ns  = int(dt_utc.timestamp() * 1e9)
        except Exception:
            ts_ns = 0

        return {
            "timestamp_ns":  ts_ns,
            "open":          float(row[1]),
            "high":          float(row[2]),
            "low":           float(row[3]),
            "close":         float(row[4]),
            "volume":        int(row[5]),
            "open_interest": int(row[6]) if len(row) > 6 else 0,
        }

    # ── Main run ──────────────────────────────────────────────────────────────

    async def run(self) -> None:
        self._connect_questdb()

        instruments = self._load_instruments()
        logger.info(
            "[HistFeed] Loaded %d instruments; interval=%s from=%s to=%s",
            len(instruments), self.interval, self.from_date, self.to_date,
        )

        chunk_days   = _chunk_days_for_interval(self.interval)
        total_candles = 0

        async with aiohttp.ClientSession() as session:
            for instr in instruments:
                key    = instr["instrument_key"]
                symbol = instr["trading_symbol"]
                instr_total = 0

                for chunk_from, chunk_to in _date_chunks(
                    self.from_date, self.to_date, chunk_days
                ):
                    try:
                        rows = await self._fetch_chunk(session, key, chunk_from, chunk_to)
                        # Upstox returns newest-first; reverse to chronological order
                        for row in reversed(rows):
                            candle = self._parse_candle(row)
                            self._write_candle(candle, symbol)
                            instr_total += 1
                    except Exception as exc:
                        logger.warning(
                            "[HistFeed] %s chunk %s→%s failed: %s",
                            symbol, chunk_from, chunk_to, exc,
                        )

                total_candles += instr_total
                logger.info("[HistFeed] %-20s  %d candles written", symbol, instr_total)

        if self._qdb_sock:
            self._qdb_sock.close()
        logger.info("[HistFeed] Done — %d total candles written to QuestDB", total_candles)
