"""
commands.py — Docker and Redis command wrappers for the Alpha TUI.
All blocking operations run via run_in_executor from async panel workers.
"""
from __future__ import annotations

import os
import time
from datetime import datetime, timedelta, timezone

import redis as syncredis
import docker as dockersdk

# Proto schemas for inter-service communication
try:
    import alpha_portfolio_pb2 as portfolio_pb
    import alpha_tick_pb2 as tick_pb
    _PROTO_AVAILABLE = True
except ImportError:
    _PROTO_AVAILABLE = False

# ── Config ────────────────────────────────────────────────────────────────────

REDIS_HOST    = os.getenv("REDIS_HOST",    "redis")
REDIS_PORT    = int(os.getenv("REDIS_PORT", "6379"))
POSTGRES_HOST = os.getenv("POSTGRES_HOST", "postgres-db")
QUESTDB_HOST  = os.getenv("QUESTDB_HOST",  "questdb")
DOCKER_NETWORK = os.getenv("DOCKER_NETWORK", "alpha_default")

# Ordered list: (display_name, container_name)
SERVICES: list[tuple[str, str]] = [
    ("data-feed",    "alpha-data-feed"),
    ("ingester",     "alpha-ingester"),
    ("signal-eng",   "alpha-signal-engine"),
    ("strategy-eng", "alpha-strategy-engine"),
    ("market-eng",   "alpha-market-engine"),
    ("backfill",     "alpha-backfill-engine"),
    ("backtest",     "alpha-backtest-engine"),
    ("questdb",      "alpha-questdb"),
    ("postgres",     "alpha-postgres"),
    ("redis",        "alpha-redis"),
    ("worker-svc",   "alpha-worker-service"),
]

ENGINE_CONTAINERS = [
    "alpha-data-feed",
    "alpha-ingester",
    "alpha-signal-engine",
    "alpha-strategy-engine",
    "alpha-market-engine",
]

# Token → Symbol (fallback when Postgres is unreachable)
TOKEN_SYMBOLS: dict[str, str] = {
    "105712": "INFY",
    "105713": "NIFTY",
    "105714": "RELIANCE",
    "105715": "TCS",
    "105716": "HDFCBANK",
    "105717": "ICICIBANK",
}

# ── Docker helpers ────────────────────────────────────────────────────────────

def _docker() -> dockersdk.DockerClient:
    return dockersdk.from_env()


def _uptime(started_at: str) -> str:
    try:
        dt = datetime.fromisoformat(started_at.replace("Z", "+00:00"))
        delta = datetime.now(timezone.utc) - dt
        s = int(delta.total_seconds())
        h, rem = divmod(s, 3600)
        m, sec = divmod(rem, 60)
        if h > 0:
            return f"{h}h{m:02d}m"
        return f"{m}m{sec:02d}s"
    except Exception:
        return "?"


def get_container_statuses() -> dict[str, dict]:
    """
    Returns {container_name: {status, uptime, restarts}} for all SERVICES.
    """
    try:
        client = _docker()
        live = {c.name: c for c in client.containers.list(all=True)}
        result: dict[str, dict] = {}
        for _, cname in SERVICES:
            c = live.get(cname)
            if c is None:
                result[cname] = {"status": "absent", "uptime": "", "restarts": 0}
                continue
            state = c.attrs.get("State", {})
            status = state.get("Status", "unknown")
            uptime = _uptime(state.get("StartedAt", "")) if status == "running" else ""
            result[cname] = {
                "status":   status,
                "uptime":   uptime,
                "restarts": c.attrs.get("RestartCount", 0),
            }
        return result
    except Exception:
        return {}


def start_container(name: str) -> bool:
    try:
        _docker().containers.get(name).start()
        return True
    except Exception:
        return False


def stop_container(name: str) -> bool:
    try:
        _docker().containers.get(name).stop(timeout=10)
        return True
    except Exception:
        return False


def restart_container(name: str) -> bool:
    try:
        _docker().containers.get(name).restart(timeout=10)
        return True
    except Exception:
        return False


def start_all_engines() -> list[str]:
    return [n for n in ENGINE_CONTAINERS if start_container(n)]


def stop_all_engines() -> list[str]:
    return [n for n in reversed(ENGINE_CONTAINERS) if stop_container(n)]


def get_container_logs(name: str, tail: int = 120) -> str:
    try:
        c = _docker().containers.get(name)
        return c.logs(tail=tail, timestamps=True).decode("utf-8", errors="replace")
    except Exception as e:
        return f"[error getting logs: {e}]"


# ── Redis helpers ─────────────────────────────────────────────────────────────

def _redis() -> syncredis.Redis:
    """Binary-mode Redis client — proto payloads contain raw bytes."""
    return syncredis.Redis(host=REDIS_HOST, port=REDIS_PORT,
                           decode_responses=False, socket_timeout=1.0)


def _decode_portfolio_snapshot(raw: bytes | None) -> dict[str, str]:
    """
    Deserialize a binary PortfolioSnapshot proto into a flat dict that the
    TUI panels can consume with the same keys as the old flat HSET format.
    Returns an empty dict on any error.
    """
    if not raw or not _PROTO_AVAILABLE:
        return {}
    try:
        snap = portfolio_pb.PortfolioSnapshot()
        snap.ParseFromString(raw)
        result: dict[str, str] = {
            "cash":             f"{snap.cash:.2f}",
            "equity":           f"{snap.equity:.2f}",
            "realized_pnl":     f"{snap.realized_pnl:.2f}",
            "total_charges":    f"{snap.total_charges:.2f}",
            "open_positions":   str(snap.open_positions),
            "total_trades":     str(snap.total_trades),
            "starting_capital": f"{snap.starting_capital:.2f}",
            "ts":               str(snap.timestamp_ns),
        }
        # Reconstruct the per-position fields the panels expect
        tokens = []
        for pos in snap.positions:
            tok = str(pos.token)
            tokens.append(tok)
            result[f"pos_{tok}"] = (
                f"{pos.symbol},{pos.qty},{pos.avg_cost:.4f},"
                f"{pos.realized_pnl:.2f},{pos.unrealized_pnl:.2f}"
            )
        result["pos_tokens"] = ",".join(tokens)
        return result
    except Exception:
        return {}


def _decode_trade(raw: bytes | None) -> dict[str, str]:
    """
    Deserialize a binary Trade proto into a flat dict with the keys the
    TUI panels expect (sym, side, qty, fill, charges, cash, rpnl, ts, id).
    Returns an empty dict on any error.
    """
    if not raw or not _PROTO_AVAILABLE:
        return {}
    try:
        t = portfolio_pb.Trade()
        t.ParseFromString(raw)
        return {
            "id":      str(t.trade_id),
            "sym":     t.symbol,
            "side":    str(t.side),
            "qty":     str(t.qty),
            "fill":    f"{t.fill_price:.4f}",
            "charges": f"{t.charges:.2f}",
            "cash":    f"{t.cash_after:.2f}",
            "rpnl":    f"{t.realized_pnl:.2f}",
            "ts":      str(t.timestamp_ns),
        }
    except Exception:
        return {}


def _decode_tick(raw: bytes | None, entry_id: bytes = b"") -> dict | None:
    """
    Deserialize a binary alpha.feed.Tick proto into a flat dict.
    Returns None on any error.
    """
    if not raw or not _PROTO_AVAILABLE:
        return None
    try:
        t = tick_pb.Tick()
        t.ParseFromString(raw)
        tok = str(t.token)
        return {
            "_id":       entry_id.decode("utf-8", errors="replace") if entry_id else "",
            "token":     tok,
            "symbol":    TOKEN_SYMBOLS.get(tok, f"T_{tok}"),
            "price":     t.last_price,
            "bid_price": t.bid_price,
            "ask_price": t.ask_price,
            "bid_size":  t.bid_size,
            "ask_size":  t.ask_size,
            "volume":    t.volume,
            "oi":        t.open_interest,
            "ts_ns":     t.timestamp_ns,
        }
    except Exception:
        return None


def get_portfolio_state() -> dict[str, str]:
    """
    HGET alpha:portfolio data → deserialize PortfolioSnapshot proto.
    Returns a flat dict with the same keys as the old HSET format.
    """
    try:
        r = _redis()
        raw = r.hget("alpha:portfolio", b"data")
        r.close()
        return _decode_portfolio_snapshot(raw)
    except Exception:
        return {}


def get_recent_trades(count: int = 20) -> list[dict[str, str]]:
    """
    XREVRANGE alpha:trades → list of Trade proto dicts, newest first.
    Each dict has the same keys as the old flat XADD format.
    """
    try:
        r = _redis()
        entries = r.xrevrange("alpha:trades", count=count)
        r.close()
        result = []
        for _, fields in entries:
            decoded = _decode_trade(fields.get(b"data"))
            if decoded:
                result.append(decoded)
        return result
    except Exception:
        return []


def get_latest_prices() -> dict[str, dict]:
    """
    Read last 300 entries of alpha:ticks (binary proto), return the most
    recent price per token as {token_str: {symbol, price, bid, ask, volume}}.
    """
    try:
        r = _redis()
        entries = r.xrevrange("alpha:ticks", count=300)
        r.close()
        prices: dict[str, dict] = {}
        for _, fields in entries:
            tick = _decode_tick(fields.get(b"data"))
            if not tick:
                continue
            tok = tick["token"]
            if tok and tok not in prices:
                prices[tok] = {
                    "symbol": tick["symbol"],
                    "price":  float(tick["price"]),
                    "bid":    float(tick["bid_price"]),
                    "ask":    float(tick["ask_price"]),
                    "volume": int(tick["volume"]),
                }
        return prices
    except Exception:
        return {}


def get_raw_ticks(count: int = 80) -> list[dict]:
    """
    XREVRANGE alpha:ticks → list of tick dicts, newest-first.
    Each entry has _id plus decoded tick fields.
    """
    try:
        r = _redis()
        entries = r.xrevrange("alpha:ticks", count=count)
        r.close()
        result = []
        for eid, fields in entries:
            tick = _decode_tick(fields.get(b"data"), eid)
            if tick:
                result.append(tick)
        return result
    except Exception:
        return []


def get_stream_lengths() -> dict[str, int]:
    """Return xlen for the main streams."""
    try:
        r = _redis()
        result = {}
        for s in [b"alpha:ticks", b"alpha:trades"]:
            try:
                result[s.decode()] = r.xlen(s)
            except Exception:
                result[s.decode()] = 0
        # alpha:portfolio is a hash, not a stream — skip xlen for it
        r.close()
        return result
    except Exception:
        return {}


def inject_test_tick(
    token: int, price: float,
    bid_price: float, ask_price: float,
    bid_size: int, volume: int, symbol: str
) -> bool:
    """XADD a synthetic proto tick to alpha:ticks."""
    if not _PROTO_AVAILABLE:
        return False
    try:
        r = _redis()
        ts_ns = int(time.time() * 1e9)

        t = tick_pb.Tick()
        t.token          = token
        t.timestamp_ns   = ts_ns
        t.last_price     = price
        t.bid_price      = bid_price
        t.ask_price      = ask_price
        t.bid_size       = bid_size
        t.ask_size       = bid_size
        t.volume         = volume
        t.open_interest  = 0.0
        bid = t.bids.add(); bid.price = bid_price; bid.quantity = bid_size
        ask = t.asks.add(); ask.price = ask_price; ask.quantity = bid_size

        r.xadd(b"alpha:ticks", {b"data": t.SerializeToString()},
               maxlen=50_000, approximate=True)
        r.close()
        return True
    except Exception:
        return False


# ── One-shot container jobs ───────────────────────────────────────────────────

def run_historical_backfill(
    from_date: str, to_date: str, interval: str = "1minute"
) -> str:
    """Spin up alpha-data-feed in historical mode. Returns container ID."""
    try:
        client = _docker()
        # Remove stale container if it exists
        try:
            client.containers.get("alpha-data-feed-historical-tui").remove(force=True)
        except Exception:
            pass
        c = client.containers.run(
            "alpha-data-feed:latest",
            environment={
                "MODE":               "historical",
                "BACKFILL_FROM_DATE": from_date,
                "BACKFILL_TO_DATE":   to_date,
                "BACKFILL_INTERVAL":  interval,
                "UPSTOX_ACCESS_TOKEN": os.getenv("UPSTOX_ACCESS_TOKEN", ""),
                "REDIS_HOST":          REDIS_HOST,
                "REDIS_PORT":          str(REDIS_PORT),
                "POSTGRES_HOST":       POSTGRES_HOST,
                "POSTGRES_PORT":       "5432",
                "POSTGRES_DB":         "alpha_db",
                "POSTGRES_USER":       "alpha_user",
                "POSTGRES_PASSWORD":   "alpha_password",
                "QUESTDB_HOST":        QUESTDB_HOST,
                "QUESTDB_ILP_PORT":    "9009",
            },
            network=DOCKER_NETWORK,
            name="alpha-data-feed-historical-tui",
            detach=True,
            remove=False,
        )
        return c.short_id
    except Exception as e:
        return f"ERROR: {e}"


def get_container_info(name: str) -> dict:
    """Return {status, exit_code} for a container. status='absent' if not found."""
    try:
        client = _docker()
        c = client.containers.get(name)
        c.reload()
        state = c.attrs.get("State", {})
        return {
            "status":    state.get("Status", "unknown"),
            "exit_code": state.get("ExitCode", -1),
        }
    except Exception:
        return {"status": "absent", "exit_code": -1}


def run_populate_backtest_ticks() -> str:
    """
    Run populate_backtest_ticks.py inside the data-feed image.
    Transforms QuestDB candles → backtest_ticks table (blocking).
    Returns container stdout or an ERROR string.
    """
    try:
        client = _docker()
        cname  = "alpha-populate-bt-tui"
        try:
            client.containers.get(cname).remove(force=True)
        except Exception:
            pass
        output = client.containers.run(
            "alpha-data-feed:latest",
            command=["python", "populate_backtest_ticks.py"],
            environment={
                "POSTGRES_HOST":    POSTGRES_HOST,
                "POSTGRES_PORT":    "5432",
                "POSTGRES_DB":      "alpha_db",
                "POSTGRES_USER":    "alpha_user",
                "POSTGRES_PASSWORD": "alpha_password",
                "QUESTDB_HOST":     QUESTDB_HOST,
                "QUESTDB_ILP_PORT": "9009",
            },
            network=DOCKER_NETWORK,
            name=cname,
            remove=True,   # detach=False → blocks until done
        )
        return output.decode().strip() if output else "done"
    except Exception as e:
        return f"ERROR: {e}"


def date_to_start_ns(date_str: str) -> int:
    """'YYYY-MM-DD' → UTC midnight epoch nanoseconds (inclusive start)."""
    dt = datetime.strptime(date_str, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    return int(dt.timestamp() * 1_000_000_000)


def date_to_end_ns(date_str: str) -> int:
    """'YYYY-MM-DD' → next-day UTC midnight nanoseconds (exclusive end)."""
    dt = (
        datetime.strptime(date_str, "%Y-%m-%d") + timedelta(days=1)
    ).replace(tzinfo=timezone.utc)
    return int(dt.timestamp() * 1_000_000_000)


def export_instrument_to_alpha(token: str, start_ns: int, end_ns: int) -> str:
    """
    Run backtest-engine in export mode for one instrument token.
    Writes /data/backtest/<token>.alpha into the alpha_backtest_data volume.
    Blocks until the container exits and returns its stdout (or an error string).
    """
    try:
        client = _docker()
        cname  = f"alpha-backtest-export-{token}"
        try:
            client.containers.get(cname).remove(force=True)
        except Exception:
            pass
        output = client.containers.run(
            "alpha-backtest-engine:latest",
            command=["export", str(token), str(start_ns), str(end_ns), "/data/backtest"],
            environment={"QUESTDB_HOST": QUESTDB_HOST},
            volumes={"alpha_backtest_data": {"bind": "/data/backtest", "mode": "rw"}},
            network=DOCKER_NETWORK,
            name=cname,
            remove=True,   # detach=False (default) → blocks until done
        )
        return output.decode().strip() if output else f"exported {token}.alpha"
    except Exception as e:
        return f"ERROR: {e}"


def list_alpha_files() -> list[str]:
    """List .alpha files in the alpha_backtest_data Docker volume.
    Runs a one-off alpine container — result cached by caller.
    Returns list of filenames (e.g. ['105712.alpha', ...]).
    """
    try:
        client = _docker()
        output = client.containers.run(
            "alpine:latest",
            command="ls /data/backtest/ 2>/dev/null || true",
            volumes={"alpha_backtest_data": {"bind": "/data/backtest", "mode": "ro"}},
            network_mode="none",
            remove=True,
        )
        if not output:
            return []
        lines = output.decode().strip().splitlines()
        return [l.strip() for l in lines if l.strip().endswith(".alpha")]
    except Exception:
        return []


def run_backtest(
    capital: float = 1_000_000,
    brokerage: float = 20.0,
    slippage_bps: float = 2.0,
) -> str:
    """Spin up alpha-backtest-engine in run mode. Returns container ID."""
    try:
        client = _docker()
        try:
            client.containers.get("alpha-backtest-tui").remove(force=True)
        except Exception:
            pass
        c = client.containers.run(
            "alpha-backtest-engine:latest",
            environment={
                "STARTING_CAPITAL": str(capital),
                "BROKERAGE_FLAT":   str(brokerage),
                "SLIPPAGE_BPS":     str(slippage_bps),
                "QUESTDB_HOST":     QUESTDB_HOST,
            },
            volumes={"alpha_backtest_data": {"bind": "/data/backtest", "mode": "rw"}},
            network=DOCKER_NETWORK,
            name="alpha-backtest-tui",
            detach=True,
            remove=False,
        )
        return c.short_id
    except Exception as e:
        return f"ERROR: {e}"
