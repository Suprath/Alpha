"""
Upstox v2 Historical Market Data Feed
─────────────────────────────────────────────────────────────────────────────
1. Weekend skip   — Sat/Sun never touch the API (no need for a calendar)
2. Holiday learn  — when Upstox returns empty candles for a weekday, that
                    date is recorded in QuestDB's `nse_holidays` table and
                    skipped on every future run (self-building calendar)
3. Deduplication  — queries existing bar count before each chunk; only
                    fetches data that is actually missing
4. Graceful retry — per-chunk exponential back-off; 429 → 62 s cooldown;
                    5xx → up to 3 retries; permanent 4xx → skip + continue
5. Rate limit     — 66 req/min (safe: 66×30 = 1,980 < Upstox 2,000/30-min)
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
QUESTDB_HOST     = os.getenv("QUESTDB_HOST",       "questdb")
QUESTDB_ILP_PORT = int(os.getenv("QUESTDB_ILP_PORT", "9009"))
QUESTDB_PG_PORT  = 8812   # PostgreSQL wire — used for existence / holiday queries

MAX_REQUESTS_PER_MIN = 55    # Upstox ~66/min hard cap; 55 gives 17% safety margin
MAX_RETRIES          = 3
RETRY_BASE_DELAY     = 2.0   # seconds; doubles each attempt
EXISTENCE_TOLERANCE  = 0.90  # accept chunk if ≥90% of expected bars present


# ── Expected bars per trading day ─────────────────────────────────────────────
# NSE equity session: 09:15 – 15:30 = 375 minutes

_BARS_PER_DAY: dict = {
    "1minute":  375.0,
    "5minute":   75.0,
    "15minute":  25.0,
    "30minute":  13.0,
    "60minute":   7.0,
    "1hour":      7.0,
    "day":        1.0,
    "week":       1.0,
    "month":      1.0,
}


def _expected_bars(interval: str, n_trading_days: int) -> int:
    bpd = _BARS_PER_DAY.get(interval, 375.0)
    return max(1, int(bpd * n_trading_days * EXISTENCE_TOLERANCE))


# ── Weekend / weekday helpers ──────────────────────────────────────────────────

def _weekdays_in_range(from_str: str, to_str: str) -> list:
    """Return all Mon–Fri dates in [from_str, to_str]. Ignores holidays."""
    fmt    = "%Y-%m-%d"
    cursor = datetime.strptime(from_str, fmt).date()
    end    = datetime.strptime(to_str,   fmt).date()
    days   = []
    while cursor <= end:
        if cursor.weekday() < 5:
            days.append(cursor)
        cursor += timedelta(days=1)
    return days


def _dates_from_candle_rows(rows: list) -> set:
    """
    Extract the unique calendar dates present in an Upstox candle response.
    Each row's first element is an ISO-8601 string like "2024-03-15T09:15:00+05:30".
    """
    dates = set()
    for row in rows:
        try:
            dates.add(datetime.strptime(row[0][:10], "%Y-%m-%d").date())
        except Exception:
            pass
    return dates


# ── NSE holiday persistence (QuestDB nse_holidays table) ──────────────────────

def _load_known_holidays() -> set:
    """
    Read all previously discovered NSE holidays from QuestDB into memory.
    Returns an empty set if the table doesn't exist yet (first run).
    """
    query = "SELECT DISTINCT timestamp FROM nse_holidays ORDER BY timestamp"
    try:
        conn = psycopg2.connect(
            host=QUESTDB_HOST, port=QUESTDB_PG_PORT,
            dbname="qdb", user="admin", password="quest",
            connect_timeout=5,
        )
        cur = conn.cursor()
        cur.execute(query)
        holidays: set = set()
        for (ts,) in cur.fetchall():
            if hasattr(ts, "date"):
                holidays.add(ts.date())
            else:
                # QuestDB may return microseconds as int
                holidays.add(
                    datetime.utcfromtimestamp(int(ts) / 1_000_000).date()
                )
        conn.close()
        logger.info("[HistFeed] Loaded %d known NSE holidays from QuestDB", len(holidays))
        return holidays
    except Exception as exc:
        logger.debug("[HistFeed] nse_holidays table not yet available: %s", exc)
        return set()


# ── Rate limiter ───────────────────────────────────────────────────────────────

class SlidingWindowThrottler:
    """
    Two-layer throttle that prevents burst-then-stall cycles:

    Layer 1 — Uniform pacing: enforces a minimum gap of 60/max_per_minute
              seconds between every request (~1.09 s at 55 req/min). This
              spreads requests evenly across the minute so the sliding window
              never fills up in a burst.

    Layer 2 — Sliding window: a while-loop that keeps sleeping until the
              60-second window actually has capacity before appending. The
              original single-if check could exit the sleep and append even
              when still at the limit, silently growing the queue past max.

    reset_window(): call after a server-side 429 to flush the timestamp
              history so the next acquire() doesn't re-burst immediately
              after the cooldown sleep.
    """

    def __init__(self, max_per_minute: int = MAX_REQUESTS_PER_MIN) -> None:
        self._max      = max_per_minute
        self._min_gap  = 60.0 / max_per_minute   # ~1.09 s at 55 req/min
        self._timestamps: deque = deque()
        self._last_req = 0.0                      # monotonic time of last request

    async def acquire(self) -> None:
        # ── Layer 1: uniform pacing ───────────────────────────────────────────
        now      = time.monotonic()
        gap_wait = self._last_req + self._min_gap - now
        if gap_wait > 0.001:
            await asyncio.sleep(gap_wait)

        # ── Layer 2: sliding-window guard (while loop, not if) ────────────────
        while True:
            now = time.monotonic()
            while self._timestamps and now - self._timestamps[0] > 60.0:
                self._timestamps.popleft()

            if len(self._timestamps) < self._max:
                break   # capacity available — proceed

            # Window still full: wait until the oldest slot would expire,
            # then loop back and re-check (don't just append blindly).
            wait = 60.0 - (now - self._timestamps[0]) + 0.1
            logger.info("[HistFeed] Rate throttle — waiting %.2fs for window capacity",
                        wait)
            await asyncio.sleep(max(0.05, wait))

        now = time.monotonic()
        self._last_req = now
        self._timestamps.append(now)

    def reset_window(self) -> None:
        """
        Flush timestamp history after a server-side 429.
        Forces a full min_gap pause before the next request so we don't
        immediately re-burst after a cooldown sleep.
        """
        self._timestamps.clear()
        self._last_req = time.monotonic()   # next acquire() will still wait min_gap


# ── Date-chunking helpers ──────────────────────────────────────────────────────

def _chunk_days_for_interval(interval: str) -> int:
    return {
        "1minute":  4,
        "5minute":  24,
        "15minute": 75,
        "30minute": 150,
        "60minute": 300,
        "1hour":    300,
    }.get(interval, 0)


def _date_chunks(from_date: str, to_date: str, chunk_days: int):
    fmt    = "%Y-%m-%d"
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


# ── QuestDB existence check ────────────────────────────────────────────────────

def _count_existing_candles(symbol: str, interval: str,
                             chunk_from: str, chunk_to: str) -> int:
    """Count candles already in QuestDB for (symbol, interval) over the chunk range."""
    exclusive_end = (
        datetime.strptime(chunk_to, "%Y-%m-%d") + timedelta(days=1)
    ).strftime("%Y-%m-%dT00:00:00Z")

    query = (
        f"SELECT COUNT(*) FROM candles "
        f"WHERE symbol = '{symbol}' "
        f"AND interval = '{interval}' "
        f"AND timestamp >= '{chunk_from}T00:00:00Z' "
        f"AND timestamp < '{exclusive_end}'"
    )
    try:
        conn = psycopg2.connect(
            host=QUESTDB_HOST, port=QUESTDB_PG_PORT,
            dbname="qdb", user="admin", password="quest",
            connect_timeout=5,
        )
        cur   = conn.cursor()
        cur.execute(query)
        count = cur.fetchone()[0] or 0
        conn.close()
        return int(count)
    except Exception as exc:
        logger.debug("[HistFeed] Existence check failed (%s %s→%s): %s",
                     symbol, chunk_from, chunk_to, exc)
        return 0


# ── Main class ─────────────────────────────────────────────────────────────────

class UpstoxHistoricalFeed:
    def __init__(
        self,
        access_token: str,
        interval: str,
        from_date: str,
        to_date: Optional[str] = None,
    ) -> None:
        self.access_token  = access_token
        self.interval      = interval
        self.from_date     = from_date
        self.to_date       = to_date or date.today().strftime("%Y-%m-%d")
        self._throttler    = SlidingWindowThrottler(MAX_REQUESTS_PER_MIN)
        self._qdb_sock: Optional[socket.socket] = None

        # In-memory holiday set — seeded from QuestDB at startup
        self._known_holidays: set = _load_known_holidays()

    # ── QuestDB ILP write ──────────────────────────────────────────────────────

    def _connect_questdb(self) -> None:
        self._qdb_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._qdb_sock.connect((QUESTDB_HOST, QUESTDB_ILP_PORT))
        logger.info("[HistFeed] QuestDB ILP connected at %s:%d",
                    QUESTDB_HOST, QUESTDB_ILP_PORT)

    def _write_candle(self, candle: dict, symbol: str) -> None:
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

    def _record_new_holidays(self, discovered: list) -> None:
        """
        Persist newly discovered NSE holiday dates to QuestDB and update
        the in-memory set. Only writes dates not already known.
        Each date is stored as midnight UTC in the nse_holidays table.
        """
        new = [d for d in discovered if d not in self._known_holidays]
        if not new:
            return

        for d in new:
            ts_ns = int(
                datetime(d.year, d.month, d.day, tzinfo=timezone.utc).timestamp() * 1e9
            )
            # ILP: nse_holidays table — confirmed=1 is a placeholder field;
            # the designated timestamp column carries the holiday date.
            line = f"nse_holidays confirmed=1i {ts_ns}\n"
            self._qdb_sock.sendall(line.encode())
            self._known_holidays.add(d)

        logger.info(
            "[HistFeed] Recorded %d new NSE holiday(s): %s",
            len(new), [d.isoformat() for d in sorted(new)],
        )

    # ── Instrument lookup ──────────────────────────────────────────────────────

    @staticmethod
    def _load_instruments() -> list:
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

    # ── HTTP fetch ─────────────────────────────────────────────────────────────

    async def _fetch_chunk(
        self,
        session: aiohttp.ClientSession,
        instrument_key: str,
        chunk_from: str,
        chunk_to: str,
    ) -> list:
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
            if resp.status == 429:
                raise aiohttp.ClientResponseError(
                    resp.request_info, resp.history,
                    status=429, message="rate limited",
                )
            if resp.status != 200:
                raise aiohttp.ClientResponseError(
                    resp.request_info, resp.history,
                    status=resp.status, message=str(body),
                )
            if body.get("status") != "success":
                raise RuntimeError(f"API non-success: {body}")
            return body["data"]["candles"]

    async def _fetch_with_retry(
        self,
        session: aiohttp.ClientSession,
        instrument_key: str,
        symbol: str,
        chunk_from: str,
        chunk_to: str,
    ) -> list:
        for attempt in range(1, MAX_RETRIES + 1):
            try:
                return await self._fetch_chunk(
                    session, instrument_key, chunk_from, chunk_to
                )

            except aiohttp.ClientResponseError as exc:
                if exc.status == 429:
                    # Server rejected us despite our local throttle.
                    # Reset the sliding window so the post-cooldown resume
                    # doesn't immediately re-burst into another 429.
                    self._throttler.reset_window()
                    cooldown = 65.0
                    logger.warning(
                        "[HistFeed] %s %s→%s — 429 from server. "
                        "Window reset + cooling down %.0fs (attempt %d/%d).",
                        symbol, chunk_from, chunk_to, cooldown, attempt, MAX_RETRIES,
                    )
                    await asyncio.sleep(cooldown)
                    continue

                elif exc.status >= 500:
                    if attempt == MAX_RETRIES:
                        logger.error(
                            "[HistFeed] %s %s→%s — HTTP %d after %d attempts. Skipping.",
                            symbol, chunk_from, chunk_to, exc.status, MAX_RETRIES,
                        )
                        return []
                    wait = RETRY_BASE_DELAY * (2 ** (attempt - 1))
                    logger.warning(
                        "[HistFeed] %s %s→%s — HTTP %d attempt %d/%d. Retry in %.1fs.",
                        symbol, chunk_from, chunk_to, exc.status, attempt, MAX_RETRIES, wait,
                    )
                    await asyncio.sleep(wait)

                else:
                    logger.error(
                        "[HistFeed] %s %s→%s — HTTP %d (permanent). Skipping.",
                        symbol, chunk_from, chunk_to, exc.status,
                    )
                    return []

            except (aiohttp.ClientError, asyncio.TimeoutError) as exc:
                if attempt == MAX_RETRIES:
                    logger.error(
                        "[HistFeed] %s %s→%s — network error after %d attempts: %s. Skipping.",
                        symbol, chunk_from, chunk_to, MAX_RETRIES, exc,
                    )
                    return []
                wait = RETRY_BASE_DELAY * (2 ** (attempt - 1))
                logger.warning(
                    "[HistFeed] %s %s→%s — network error attempt %d/%d, retry in %.1fs: %s",
                    symbol, chunk_from, chunk_to, attempt, MAX_RETRIES, wait, exc,
                )
                await asyncio.sleep(wait)

        return []

    @staticmethod
    def _parse_candle(row: list) -> dict:
        ts_str = row[0]
        try:
            dt_naive = datetime.strptime(ts_str[:19], "%Y-%m-%dT%H:%M:%S")
            tz_part  = ts_str[19:].strip()
            sign = 1
            if tz_part.startswith("-"):
                sign, tz_part = -1, tz_part[1:]
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

    # ── Main run ───────────────────────────────────────────────────────────────

    async def run(self) -> None:
        self._connect_questdb()

        instruments = self._load_instruments()
        logger.info(
            "[HistFeed] %d instruments | interval=%s | %s → %s | "
            "%d holidays already known",
            len(instruments), self.interval, self.from_date, self.to_date,
            len(self._known_holidays),
        )

        chunk_days    = _chunk_days_for_interval(self.interval)
        total_new     = 0
        total_skipped = 0

        async with aiohttp.ClientSession() as session:
            for instr in instruments:
                key        = instr["instrument_key"]
                symbol     = instr["trading_symbol"]
                instr_new  = 0
                instr_skip = 0

                for chunk_from, chunk_to in _date_chunks(
                    self.from_date, self.to_date, chunk_days
                ):
                    # ── 1. Skip pure-weekend chunks ───────────────────────────
                    weekdays = _weekdays_in_range(chunk_from, chunk_to)
                    if not weekdays:
                        continue   # Sat/Sun only — no API call needed

                    # ── 2. Subtract known holidays ────────────────────────────
                    expected_trading = [
                        d for d in weekdays if d not in self._known_holidays
                    ]

                    if not expected_trading:
                        logger.debug(
                            "[HistFeed] %s %s→%s — all weekdays are known holidays. Skip.",
                            symbol, chunk_from, chunk_to,
                        )
                        instr_skip += len(weekdays)
                        continue

                    # ── 3. Skip if sufficient candles already in QuestDB ──────
                    existing = _count_existing_candles(
                        symbol, self.interval, chunk_from, chunk_to
                    )
                    needed = _expected_bars(self.interval, len(expected_trading))

                    if existing >= needed:
                        logger.info(
                            "[HistFeed] %s %s→%s — %d bars present "
                            "(%d trading days, %d known holidays). Skip.",
                            symbol, chunk_from, chunk_to, existing,
                            len(expected_trading), len(weekdays) - len(expected_trading),
                        )
                        instr_skip += existing
                        continue

                    logger.info(
                        "[HistFeed] %s %s→%s — %d/%d bars, %d trading days. Fetching...",
                        symbol, chunk_from, chunk_to, existing, needed,
                        len(expected_trading),
                    )

                    # ── 4. Fetch with retry ───────────────────────────────────
                    rows = await self._fetch_with_retry(
                        session, key, symbol, chunk_from, chunk_to
                    )

                    # ── 5. Discover and record holidays ───────────────────────
                    # Only learn holidays from chunks that returned some data —
                    # if the whole chunk is empty it's likely a pre-listing period
                    # (stock didn't exist yet) rather than a market holiday.
                    if rows:
                        covered_dates = _dates_from_candle_rows(rows)
                        new_holidays  = [
                            d for d in weekdays
                            if d not in covered_dates
                            and d not in self._known_holidays
                        ]
                        if new_holidays:
                            self._record_new_holidays(new_holidays)
                    else:
                        continue   # pre-listing or all days in chunk were holidays

                    # ── 6. Write candles to QuestDB ───────────────────────────
                    for row in reversed(rows):   # Upstox returns newest-first
                        candle = self._parse_candle(row)
                        self._write_candle(candle, symbol)
                        instr_new += 1

                total_new     += instr_new
                total_skipped += instr_skip
                logger.info(
                    "[HistFeed] %-20s  +%d new candles  %d skipped",
                    symbol, instr_new, instr_skip,
                )

        if self._qdb_sock:
            self._qdb_sock.close()

        logger.info(
            "[HistFeed] Done — %d new candles, %d skipped. "
            "Total known holidays: %d",
            total_new, total_skipped, len(self._known_holidays),
        )
