"""
commands.py — Docker and Redis command wrappers for the Alpha TUI.
All blocking operations run via run_in_executor from async panel workers.
"""
from __future__ import annotations

import os
import time
from datetime import datetime, timezone

import redis as syncredis
import docker as dockersdk

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
    return syncredis.Redis(host=REDIS_HOST, port=REDIS_PORT,
                           decode_responses=True, socket_timeout=1.0)


def get_portfolio_state() -> dict[str, str]:
    """HGETALL alpha:portfolio → dict of all portfolio fields."""
    try:
        r = _redis()
        data = r.hgetall("alpha:portfolio")
        r.close()
        return data
    except Exception:
        return {}


def get_recent_trades(count: int = 20) -> list[dict[str, str]]:
    """XREVRANGE alpha:trades → list of trade field dicts, newest first."""
    try:
        r = _redis()
        entries = r.xrevrange("alpha:trades", count=count)
        r.close()
        return [fields for _, fields in entries]
    except Exception:
        return []


def get_latest_prices() -> dict[str, dict]:
    """
    Read last 300 entries of alpha:ticks, return the most recent price
    per token as {token_str: {symbol, price, bid, ask, volume}}.
    """
    try:
        r = _redis()
        entries = r.xrevrange("alpha:ticks", count=300)
        r.close()
        prices: dict[str, dict] = {}
        for _, f in entries:
            tok = f.get("token", "")
            if tok and tok not in prices:
                sym = f.get("symbol", TOKEN_SYMBOLS.get(tok, f"T_{tok}"))
                prices[tok] = {
                    "symbol":  sym,
                    "price":   float(f.get("price",     0) or 0),
                    "bid":     float(f.get("bid_price", 0) or 0),
                    "ask":     float(f.get("ask_price", 0) or 0),
                    "volume":  int(f.get("volume",      0) or 0),
                }
        return prices
    except Exception:
        return {}


def get_raw_ticks(count: int = 80) -> list[dict]:
    """
    XREVRANGE alpha:ticks → list of tick field dicts, newest-first.
    Each entry has _id plus all stream fields (token, symbol, price, bid_price,
    ask_price, bid_size, volume, ts_ns).
    """
    try:
        r = _redis()
        entries = r.xrevrange("alpha:ticks", count=count)
        r.close()
        return [{"_id": eid, **fields} for eid, fields in entries]
    except Exception:
        return []


def get_stream_lengths() -> dict[str, int]:
    """Return xlen for the main streams."""
    try:
        r = _redis()
        result = {}
        for s in ["alpha:ticks", "alpha:trades", "alpha:portfolio"]:
            try:
                result[s] = r.xlen(s)
            except Exception:
                result[s] = 0
        r.close()
        return result
    except Exception:
        return {}


def inject_test_tick(
    token: int, price: float,
    bid_price: float, ask_price: float,
    bid_size: int, volume: int, symbol: str
) -> bool:
    """XADD a synthetic tick to alpha:ticks."""
    try:
        r = _redis()
        ts_ns = int(time.time() * 1e9)
        fields = {
            "ts_ns":     str(ts_ns),
            "token":     str(token),
            "price":     str(price),
            "bid_price": str(bid_price),
            "ask_price": str(ask_price),
            "bid_size":  str(bid_size),
            "ask_size":  str(bid_size),
            "volume":    str(volume),
            "oi":        "0",
            "symbol":    symbol,
            "bid_p0":    str(bid_price),
            "bid_q0":    str(bid_size),
            "ask_p0":    str(ask_price),
            "ask_q0":    str(bid_size),
        }
        r.xadd("alpha:ticks", fields, maxlen=50_000, approximate=True)
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
