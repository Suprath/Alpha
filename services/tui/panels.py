"""
panels.py — All six display panels for the Alpha Terminal UI.

Each panel fetches data in a background thread via @work(thread=True) and
dispatches render back to the UI thread via call_from_thread, keeping
Textual's event loop and render loop completely unblocked.
"""
from __future__ import annotations

import time
from collections import deque
from datetime import datetime, timezone

from textual.app import ComposeResult
from textual.widget import Widget
from textual.widgets import Static, RichLog
from textual.worker import get_current_worker
from textual import work

from commands import (
    SERVICES,
    TOKEN_SYMBOLS,
    get_container_statuses,
    get_portfolio_state,
    get_recent_trades,
    get_latest_prices,
    get_raw_ticks,
    get_stream_lengths,
    get_container_logs,
)

# ── Colour palette ─────────────────────────────────────────────────────────────
G   = "#3fb950"
R   = "#f85149"
B   = "#58a6ff"
Y   = "#d29922"
GR  = "#8b949e"
W   = "#e6edf3"
DIM = "#484f58"
C   = "#79c0ff"


# ── Shared helpers ─────────────────────────────────────────────────────────────

def bar(value: float, max_val: float, width: int = 10, color: str = G) -> str:
    ratio  = min(abs(value) / max(abs(max_val), 1e-9), 1.0)
    filled = ratio * width
    full   = int(filled)
    eighth = int((filled - full) * 8)
    empty  = width - full - (1 if eighth > 0 else 0)
    chars  = " ▏▎▍▌▋▊▉█"
    s      = "█" * full
    if eighth > 0:
        s += chars[eighth]
    s += "░" * max(0, empty)
    return f"[{color}]{s}[/]"


def money(val: float, sign: bool = False) -> str:
    prefix = ("▲ +" if val > 0 else "▼ ") if sign else ""
    color  = G if val > 0 else (R if val < 0 else W)
    return f"[{color}]{prefix}₹{val:>12,.2f}[/]"


def pct(val: float) -> str:
    color = G if val > 0 else (R if val < 0 else GR)
    sign  = "+" if val > 0 else ""
    return f"[{color}]{sign}{val:.2f}%[/]"


def hdr(title: str, extra: str = "") -> str:
    right = f" [{GR}]{extra}[/]" if extra else ""
    return f"[bold {B}]{title}[/]{right}"


# ── Services Panel ─────────────────────────────────────────────────────────────

class ServicesPanel(Widget):
    """Docker container health grid. Refreshes every 2 s."""

    def compose(self) -> ComposeResult:
        yield Static("", id="svc-content")

    def on_mount(self) -> None:
        self.set_interval(2.0, self._trigger_refresh)
        self._trigger_refresh()

    def _trigger_refresh(self) -> None:
        self._fetch()

    @work(thread=True, exclusive=True)
    def _fetch(self) -> None:
        data = get_container_statuses()
        self.app.call_from_thread(self._render, data)

    def _render(self, statuses: dict) -> None:
        running = sum(1 for v in statuses.values() if v.get("status") == "running")
        total   = len(SERVICES)
        hcol    = G if running == total else (Y if running > 0 else R)
        lines   = [hdr("SERVICES", f"[{hcol}]{running}/{total} up[/]")]

        for dname, cname in SERVICES:
            info     = statuses.get(cname, {})
            status   = info.get("status", "absent")
            uptime   = info.get("uptime", "")
            restarts = info.get("restarts", 0)

            if status == "running":
                dot, col, stat_s = f"[{G}]●[/]", G, f"UP  {uptime:<8}"
            elif status == "exited":
                dot, col, stat_s = f"[{R}]✗[/]", R, "EXITED     "
            elif status == "restarting":
                dot, col, stat_s = f"[{Y}]↻[/]", Y, "RESTARTING "
            elif status == "absent":
                dot, col, stat_s = f"[{DIM}]○[/]", DIM, "ABSENT     "
            else:
                dot, col, stat_s = f"[{Y}]?[/]", Y, status.upper()[:11]

            r_badge = f" [{R}]r{restarts}[/]" if restarts > 0 else ""
            lines.append(f" {dot} [{col}]{dname:<14}[/][{GR}]{stat_s}[/]{r_badge}")

        lines.append("")
        lines.append(f" [{DIM}][F5] Start All   [F6] Stop Eng[/]")
        self.query_one("#svc-content", Static).update("\n".join(lines))


# ── Prices Panel ───────────────────────────────────────────────────────────────

class PricesPanel(Widget):
    """Live price strip from Redis alpha:ticks. Refreshes every 500 ms."""

    _prev_prices: dict[str, float]

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._prev_prices = {}

    def compose(self) -> ComposeResult:
        yield Static("", id="prices-content")

    def on_mount(self) -> None:
        self.set_interval(0.5, self._trigger_refresh)
        self._trigger_refresh()

    def _trigger_refresh(self) -> None:
        self._fetch()

    @work(thread=True, exclusive=True)
    def _fetch(self) -> None:
        prices = get_latest_prices()
        lens   = get_stream_lengths()
        self.app.call_from_thread(self._render, prices, lens)

    def _render(self, prices: dict, lens: dict) -> None:
        tick_len = lens.get("alpha:ticks", 0)
        lines    = [hdr("LIVE PRICES", f"stream depth: {tick_len:,}")]

        order = ["105713", "105714", "105715", "105716", "105712", "105717"]
        shown = {t: prices[t] for t in order if t in prices}
        for tok, p in prices.items():
            if tok not in shown:
                shown[tok] = p

        for tok, p in shown.items():
            sym   = p["symbol"]
            price = p["price"]
            bid   = p["bid"]
            ask   = p["ask"]
            vol   = p["volume"]
            prev  = self._prev_prices.get(tok, price)
            delta = price - prev

            if delta > 0.001:
                arrow, pcol = "▲", G
            elif delta < -0.001:
                arrow, pcol = "▼", R
            else:
                arrow, pcol = "─", GR

            spread_bps = ((ask - bid) / price * 10000) if price > 0 else 0
            lines.append(
                f" [{C}]{sym:<10}[/] [{pcol}]{price:>10,.2f}[/] "
                f"[{pcol}]{arrow} {delta:>+8.2f}[/]  "
                f"[{GR}]B:{bid:,.2f}/A:{ask:,.2f}  "
                f"spd:{spread_bps:.1f}bps  vol:{vol:,}[/]"
            )
            self._prev_prices[tok] = price

        if not shown:
            lines.append(f"  [{GR}]No tick data — is the pipeline running?[/]")

        depth_ratio = min(tick_len / 50_000, 1.0)
        depth_col   = G if depth_ratio < 0.5 else (Y if depth_ratio < 0.8 else R)
        lines.append("")
        lines.append(
            f" [{GR}]Redis alpha:ticks:[/] {bar(depth_ratio, 1.0, 12, depth_col)} "
            f"[{depth_col}]{depth_ratio*100:.0f}%[/]  "
            f"[{GR}]trades: {lens.get('alpha:trades', 0)}[/]"
        )
        self.query_one("#prices-content", Static).update("\n".join(lines))


# ── Portfolio Panel ────────────────────────────────────────────────────────────

class PortfolioPanel(Widget):
    """Portfolio state from Redis alpha:portfolio. Refreshes every 1 s."""

    def compose(self) -> ComposeResult:
        yield Static("", id="port-content")

    def on_mount(self) -> None:
        self.set_interval(1.0, self._trigger_refresh)
        self._trigger_refresh()

    def _trigger_refresh(self) -> None:
        self._fetch()

    @work(thread=True, exclusive=True)
    def _fetch(self) -> None:
        state = get_portfolio_state()
        self.app.call_from_thread(self._render, state)

    def _render(self, s: dict) -> None:
        if not s:
            self.query_one("#port-content", Static).update(
                hdr("PORTFOLIO") + f"\n\n  [{GR}]No data — market engine not running[/]"
            )
            return

        cash     = float(s.get("cash",            0) or 0)
        equity   = float(s.get("equity",           0) or 0)
        rpnl     = float(s.get("realized_pnl",     0) or 0)
        charges  = float(s.get("total_charges",    0) or 0)
        cap      = float(s.get("starting_capital", 1_000_000) or 1_000_000)
        n_trades = int(s.get("total_trades",        0) or 0)
        n_open   = int(s.get("open_positions",      0) or 0)
        pos_toks = s.get("pos_tokens", "").split(",")

        equity_pct = (equity - cap) / cap * 100 if cap else 0
        lines = [hdr("PORTFOLIO", f"[{GR}]cap: ₹{cap:,.0f}[/]")]
        lines.append(
            f" [{GR}]Cash  [/]{money(cash)}  "
            f"[{GR}]Equity[/]{money(equity)}"
        )
        lines.append(
            f" [{GR}]P&L   [/]{money(rpnl, sign=True)}  "
            f"[{GR}]Return[/] {pct(equity_pct)}"
        )
        lines.append(
            f" [{GR}]Chgrs [/][{R}]₹{charges:>10,.2f}[/]  "
            f"[{GR}]Trades[/] [{W}]{n_trades:>6}[/]"
        )

        pos_list = [t for t in pos_toks if t.strip()]
        if pos_list:
            lines.append(f" [{DIM}]{'─'*38}[/]")
            lines.append(f" [{GR}]POSITIONS ({n_open} open)[/]")
            for tok in pos_list:
                raw = s.get(f"pos_{tok}", "")
                if not raw:
                    continue
                parts = raw.split(",")
                if len(parts) < 5:
                    continue
                sym, qty_s, avg_s, rpnl_s, urpnl_s = parts[:5]
                qty  = int(qty_s)
                avg  = float(avg_s)
                rp   = float(rpnl_s)
                urp  = float(urpnl_s)
                side = "L" if qty > 0 else "S"
                ucol = G if urp >= 0 else R
                lines.append(
                    f"  [{C}]{sym:<10}[/] [{GR}]{side}[/][{W}]{abs(qty):>4}[/] "
                    f"[{GR}]@{avg:,.2f}[/]  [{ucol}]{urp:>+,.2f}[/]"
                )
        else:
            lines.append(f" [{DIM}]No open positions[/]")

        self.query_one("#port-content", Static).update("\n".join(lines))


# ── Signals Panel ──────────────────────────────────────────────────────────────

class SignalsPanel(Widget):
    """OFI signal events from signal/market engine logs. Refreshes every 2 s."""

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._signals: deque[str] = deque(maxlen=20)
        self._seen: set[str]      = set()

    def compose(self) -> ComposeResult:
        yield Static("", id="sig-content")

    def on_mount(self) -> None:
        self.set_interval(2.0, self._trigger_refresh)
        self._trigger_refresh()

    def _trigger_refresh(self) -> None:
        self._fetch()

    @work(thread=True, exclusive=True)
    def _fetch(self) -> None:
        raw  = get_container_logs("alpha-signal-engine", tail=80)
        raw += get_container_logs("alpha-market-engine",  tail=40)
        self.app.call_from_thread(self._render, raw)

    def _render(self, raw: str) -> None:
        for line in raw.splitlines():
            lower = line.lower()
            if ("ofi=" in lower or "ofi" in lower) and (
                "fire" in lower or "threshold" in lower or "signal" in lower
            ):
                if line not in self._seen:
                    self._signals.appendleft(line.strip()[:120])
                    self._seen.add(line)
            elif "[order]" in lower and "entry" in lower:
                if line not in self._seen:
                    self._signals.appendleft(line.strip()[:120])
                    self._seen.add(line)
            if len(self._seen) > 200:
                self._seen = set(list(self._seen)[-100:])

        lines = [hdr("SIGNALS", f"[{GR}]OFI threshold ≥ 50,000[/]")]
        if not self._signals:
            lines.append(f"  [{GR}]No signals yet — waiting for market data...[/]")
        else:
            for entry in list(self._signals)[:8]:
                if "buy" in entry.lower() or "▲" in entry or "+ofi" in entry.lower():
                    col = G
                elif "sell" in entry.lower() or "▼" in entry or "-ofi" in entry.lower():
                    col = R
                else:
                    col = Y
                display = entry[-80:] if len(entry) > 80 else entry
                lines.append(f"  [{col}]{display}[/]")

        self.query_one("#sig-content", Static).update("\n".join(lines))


# ── Trades Panel ───────────────────────────────────────────────────────────────

class TradesPanel(Widget):
    """Recent trades from Redis alpha:trades stream. Refreshes every 1 s."""

    def compose(self) -> ComposeResult:
        yield Static("", id="trades-content")

    def on_mount(self) -> None:
        self.set_interval(1.0, self._trigger_refresh)
        self._trigger_refresh()

    def _trigger_refresh(self) -> None:
        self._fetch()

    @work(thread=True, exclusive=True)
    def _fetch(self) -> None:
        trades = get_recent_trades(8)
        self.app.call_from_thread(self._render, trades)

    def _render(self, trades: list[dict]) -> None:
        lines = [hdr("RECENT TRADES", f"[{GR}]last {len(trades)}[/]")]
        if not trades:
            lines.append(f"  [{GR}]No trades — market engine running?[/]")
        else:
            lines.append(
                f" [{GR}]{'#':>4}  {'SYM':<10} {'S':>1} {'QTY':>5} "
                f"{'FILL':>10}  {'CHGS':>8}  {'CASH':>12}[/]"
            )
            for t in trades:
                side_n = int(t.get("side", 1) or 1)
                qty    = int(t.get("qty",   0) or 0)
                fill   = float(t.get("fill", 0) or 0)
                chgs   = float(t.get("charges", 0) or 0)
                cash   = float(t.get("cash",  0) or 0)
                ts_ns  = int(t.get("ts",     0) or 0)

                side_s = "B" if side_n > 0 else "S"
                scol   = G if side_n > 0 else R

                ts_str = ""
                if ts_ns:
                    try:
                        ts_str = datetime.fromtimestamp(
                            ts_ns / 1e9 + 19800, tz=timezone.utc
                        ).strftime("%H:%M")
                    except Exception:
                        pass

                lines.append(
                    f" [{GR}]{t.get('id','?'):>4}[/]  [{C}]{t.get('sym','?'):<10}[/] "
                    f"[{scol}]{side_s}[/] [{W}]{qty:>5}[/] "
                    f"[{C}]{fill:>10,.2f}[/]  [{R}]{chgs:>8,.2f}[/]  "
                    f"[{GR}]{cash:>12,.2f}[/]"
                )

        self.query_one("#trades-content", Static).update("\n".join(lines))


# ── Log Panel ──────────────────────────────────────────────────────────────────

_LOG_SOURCES = [
    ("tick-feed",      None),
    ("market-engine",  "alpha-market-engine"),
    ("signal-engine",  "alpha-signal-engine"),
    ("strategy-engine","alpha-strategy-engine"),
    ("ingester",       "alpha-ingester"),
    ("data-feed",      "alpha-data-feed"),
]

_SYM_COLORS = {
    "NIFTY":    "#79c0ff",
    "RELIANCE": "#d2a8ff",
    "TCS":      "#ffa657",
    "HDFCBANK": "#f0883e",
    "INFY":     "#56d364",
    "ICICIBANK":"#ff7b72",
}


class LogPanel(Widget):
    """Tick stream + Docker log viewer. Press L to cycle source."""

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._source_idx  = 0
        self._last_tick_id = "0-0"
        self._tick_prev:  dict[str, float] = {}

    def compose(self) -> ComposeResult:
        yield Static("", id="log-header")
        yield RichLog(id="log-view", highlight=True, markup=True, wrap=False)

    def on_mount(self) -> None:
        self.set_interval(0.5, self._trigger_refresh)
        self._trigger_refresh()

    def cycle_source(self) -> None:
        self._source_idx = (self._source_idx + 1) % len(_LOG_SOURCES)
        self.query_one("#log-view", RichLog).clear()
        if _LOG_SOURCES[self._source_idx][0] == "tick-feed":
            self._last_tick_id = "0-0"
            self._tick_prev    = {}
        self._trigger_refresh()

    def _trigger_refresh(self) -> None:
        dname, cname = _LOG_SOURCES[self._source_idx]
        if dname == "tick-feed":
            self._fetch_ticks()
        else:
            self._fetch_logs(cname)

    @work(thread=True, exclusive=True, name="log-tick-worker")
    def _fetch_ticks(self) -> None:
        ticks = get_raw_ticks(120)
        self.app.call_from_thread(self._render_tick_feed, ticks)

    @work(thread=True, exclusive=True, name="log-docker-worker")
    def _fetch_logs(self, cname: str) -> None:
        raw = get_container_logs(cname, tail=60)
        self.app.call_from_thread(self._render_logs, raw)

    # ── header ──────────────────────────────────────────────────────────────────

    def _update_header(self) -> None:
        dname = _LOG_SOURCES[self._source_idx][0]
        options = "  ".join(
            f"[bold {B}]{n}[/]" if n == dname else f"[{GR}]{n}[/]"
            for n, _ in _LOG_SOURCES
        )
        self.query_one("#log-header", Static).update(
            f"[bold {B}]LOG[/]  [{GR}][[/]{options}[{GR}]]  "
            f"[{DIM}]L=cycle  0.5s refresh[/]"
        )

    # ── tick feed ───────────────────────────────────────────────────────────────

    def _render_tick_feed(self, ticks: list[dict]) -> None:
        self._update_header()
        log_w = self.query_one("#log-view", RichLog)

        if not ticks:
            log_w.clear()
            log_w.write(
                f"[{GR}]  No tick data — start data-feed or inject a test tick (T)[/]"
            )
            return

        ticks_chron = list(reversed(ticks))
        newest_id   = ticks_chron[-1]["_id"]

        if self._last_tick_id == "0-0":
            log_w.clear()
            self._tick_prev = {}
            for t in ticks_chron:
                self._write_one_tick(log_w, t)
            self._last_tick_id = newest_id
        else:
            for t in ticks_chron:
                if t["_id"] > self._last_tick_id:
                    self._write_one_tick(log_w, t)
                    self._last_tick_id = t["_id"]

    def _write_one_tick(self, log_w: RichLog, t: dict) -> None:
        sym    = t.get("symbol", TOKEN_SYMBOLS.get(t.get("token", ""), "?"))
        price  = float(t.get("price",     0) or 0)
        bid    = float(t.get("bid_price", 0) or 0)
        ask    = float(t.get("ask_price", 0) or 0)
        vol    = int(t.get("volume",      0) or 0)
        bid_sz = int(t.get("bid_size",    0) or 0)
        ts_ns  = int(t.get("ts_ns",       0) or 0)

        if ts_ns:
            try:
                dt       = datetime.fromtimestamp(ts_ns / 1e9 + 19800, tz=timezone.utc)
                time_str = dt.strftime("%H:%M:%S.") + f"{dt.microsecond // 1000:03d}"
            except Exception:
                time_str = "--:--:--.---"
        else:
            time_str = "--:--:--.---"

        prev  = self._tick_prev.get(sym, price)
        delta = price - prev
        self._tick_prev[sym] = price

        arrow, dcol = ("▲", G) if delta > 0.001 else (("▼", R) if delta < -0.001 else ("─", GR))
        spread_bps  = (ask - bid) / price * 10000 if price > 0 else 0
        scol        = _SYM_COLORS.get(sym, C)

        log_w.write(
            f"[{GR}]{time_str}[/]  "
            f"[bold {scol}]{sym:<10}[/]  "
            f"[{dcol}]{arrow}[/] [{W}]{price:>10,.2f}[/]  "
            f"[{dcol}]{delta:>+8.2f}[/]  "
            f"[{GR}]B:[/][{G}]{bid:,.2f}[/][{GR}]/A:[/][{R}]{ask:,.2f}[/]  "
            f"[{GR}]spd:[/][{Y}]{spread_bps:.1f}bps[/]  "
            f"[{GR}]vol:[/][{W}]{vol:>8,}[/]  "
            f"[{GR}]bsz:[/][{C}]{bid_sz:>8,}[/]"
        )

    # ── docker logs ─────────────────────────────────────────────────────────────

    def _render_logs(self, raw: str) -> None:
        self._update_header()
        log_w = self.query_one("#log-view", RichLog)
        log_w.clear()
        for line in raw.splitlines()[-55:]:
            if "[TRADE" in line:
                log_w.write(f"[{G}]{line}[/]")
            elif "[ORDER]" in line or "[signal]" in line.lower():
                log_w.write(f"[{B}]{line}[/]")
            elif "ERROR" in line or "error" in line or "WARN" in line:
                log_w.write(f"[{R}]{line}[/]")
            elif "WARNING" in line.upper():
                log_w.write(f"[{Y}]{line}[/]")
            else:
                log_w.write(f"[{GR}]{line}[/]")
