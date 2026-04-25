"""
Alpha Terminal UI — main entry point.

Dense single-screen trading dashboard built with Textual.

Modes
-----
  LIVE      Real-time trading dashboard (default)
  RESEARCH  Backtest / backfill research workspace

Live layout (top→bottom):
  ┌─ Header: title / IST time / market status / mode ──────────────────────┐
  ├─ Top row (height 15):  ServicesPanel | PricesPanel | PortfolioPanel     │
  ├─ Mid row (height 11):  SignalsPanel  | TradesPanel                      │
  ├─ Log panel (1fr):      Tick stream / Docker log viewer                  │
  └─ Footer ──────────────────────────────────────────────────────────────  ┘

Research layout (top→bottom):
  ┌─ Header ────────────────────────────────────────────────────────────────┐
  ├─ Top row (height 18):  BacktestStatusPanel | BacktestResultsPanel       │
  ├─ Mid row (height 10):  BackfillStatusPanel | InstrumentPanel | LatestBacktestPanel │
  ├─ Log panel (1fr):      Defaults to backtest-run container logs          │
  └─ Footer ──────────────────────────────────────────────────────────────  ┘

Keybindings:
  M   Toggle Live / Research mode
  F5  Start all engine containers
  F6  Stop all engine containers
  R   Restart service (opens ServiceModal)
  T   Inject test tick (opens TestTickModal)
  B   Historical backfill (opens BackfillModal)
  X   Run backtest (opens BacktestModal)
  E   EOD square-off (confirm → SIGTERM market-engine)
  L   Cycle log source
  Q   Quit
"""
from __future__ import annotations

import asyncio
import os
from datetime import datetime, timezone, timedelta

from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.widgets import Footer, Header, Static
from textual import on

from panels import (
    ServicesPanel, PricesPanel, PortfolioPanel,
    SignalsPanel, TradesPanel, LogPanel,
    BacktestStatusPanel, BacktestResultsPanel,
    BackfillStatusPanel, InstrumentPanel,
    LatestBacktestPanel,
)
from modals import ConfirmModal, ServiceModal, TestTickModal, BackfillModal, BacktestModal
from commands import stop_container, start_all_engines, stop_all_engines

# IST = UTC + 5:30
_IST = timedelta(hours=5, minutes=30)
_MARKET_OPEN  = (9, 15)
_MARKET_CLOSE = (15, 30)

G   = "#3fb950"
R   = "#f85149"
B   = "#58a6ff"
Y   = "#d29922"
GR  = "#8b949e"
W   = "#e6edf3"
P   = "#bc8cff"   # purple — used for Research mode indicator


def _ist_now() -> datetime:
    return datetime.now(timezone.utc) + _IST


def _market_status() -> tuple[str, str]:
    """Returns (label, color)."""
    now  = _ist_now()
    wday = now.weekday()   # 0=Mon … 4=Fri
    h, m = now.hour, now.minute

    if wday >= 5:
        return "CLOSED (weekend)", GR
    open_m  = _MARKET_OPEN[0]  * 60 + _MARKET_OPEN[1]
    close_m = _MARKET_CLOSE[0] * 60 + _MARKET_CLOSE[1]
    cur_m   = h * 60 + m

    if open_m <= cur_m < close_m:
        return "OPEN", G
    elif cur_m < open_m:
        wait = open_m - cur_m
        return f"PRE-MARKET ({wait}m)", Y
    else:
        return "CLOSED", R


class AlphaTUI(App):
    """Alpha Terminal — Live trading dashboard and Research workspace."""

    TITLE   = "α ALPHA TERMINAL"
    CSS_PATH = "app.css"

    BINDINGS = [
        ("m",      "toggle_mode",   "Mode"),
        ("f5",     "start_all",     "Start All"),
        ("f6",     "stop_engines",  "Stop Engines"),
        ("r",      "restart",       "Restart Svc"),
        ("t",      "test_tick",     "Test Tick"),
        ("b",      "backfill",      "Backfill"),
        ("x",      "backtest",      "Backtest"),
        ("e",      "eod",           "EOD Square-off"),
        ("l",      "cycle_log",     "Cycle Log"),
        ("q",      "quit",          "Quit"),
    ]

    _mode: str = "live"   # "live" | "research"

    # ── Layout ─────────────────────────────────────────────────────────────────

    def compose(self) -> ComposeResult:
        yield Static("", id="tui-header")

        # ── Live mode layout ───────────────────────────────────────────────────
        with Vertical(id="live-view"):
            with Horizontal(id="top-row"):
                yield ServicesPanel()
                yield PricesPanel()
                yield PortfolioPanel()
            with Horizontal(id="mid-row"):
                yield SignalsPanel()
                yield TradesPanel()

        # ── Research mode layout (hidden on start) ─────────────────────────────
        with Vertical(id="research-view"):
            with Horizontal(id="res-top-row"):
                yield BacktestStatusPanel()
                yield BacktestResultsPanel()
            with Horizontal(id="res-mid-row"):
                yield BackfillStatusPanel()
                yield InstrumentPanel()
                yield LatestBacktestPanel()

        yield LogPanel()
        yield Footer()

    # ── Lifecycle ──────────────────────────────────────────────────────────────

    def on_mount(self) -> None:
        # Research view hidden by default
        self.query_one("#research-view").display = False
        self._tick_header()
        self.set_interval(1.0, self._tick_header)

    def _tick_header(self) -> None:
        from textual.css.query import NoMatches
        now    = _ist_now()
        time_s = now.strftime("%H:%M:%S IST")
        status, scol = _market_status()
        date_s = now.strftime("%a %d %b %Y")

        if self._mode == "live":
            mode_s = f"[bold {G}] LIVE [/]"
        else:
            mode_s = f"[bold {P}] RESEARCH [/]"

        try:
            self.query_one("#tui-header", Static).update(
                f"[bold {B}]  α  ALPHA TERMINAL[/]"
                f"  {mode_s}"
                f"  [{GR}]{date_s}[/]"
                f"  [{W}]{time_s}[/]"
                f"  MARKET: [{scol}]{status}[/]"
                f"  [{GR}](M=toggle mode)[/]"
            )
        except NoMatches:
            pass  # Modal screen is active; header lives on the background screen

    # ── Action handlers ────────────────────────────────────────────────────────

    def action_toggle_mode(self) -> None:
        """M — switch between Live and Research mode."""
        self._mode = "research" if self._mode == "live" else "live"

        self.query_one("#live-view").display     = (self._mode == "live")
        self.query_one("#research-view").display = (self._mode == "research")

        log = self.query_one(LogPanel)
        if self._mode == "research":
            log.set_backtest_source()
        else:
            log.set_live_source()

        self._tick_header()
        self.notify(
            f"Switched to {'Live trading dashboard' if self._mode == 'live' else 'Research / backtest workspace'}",
            title=f"Mode: {self._mode.upper()}",
            severity="information",
        )

    async def action_start_all(self) -> None:
        """F5 — start all engine containers."""
        loop    = asyncio.get_event_loop()
        started = await loop.run_in_executor(None, start_all_engines)
        self.notify(
            f"Started: {', '.join(started) or 'none'}",
            title="Start All",
            severity="information",
        )

    async def action_stop_engines(self) -> None:
        """F6 — stop all engine containers (confirm first)."""
        confirmed = await self.push_screen_wait(
            ConfirmModal(
                "Stop all engine containers?\n"
                "(data-feed, ingester, signal, strategy, market)",
                danger=True,
            )
        )
        if confirmed:
            loop    = asyncio.get_event_loop()
            stopped = await loop.run_in_executor(None, stop_all_engines)
            self.notify(
                f"Stopped: {', '.join(stopped) or 'none'}",
                title="Stop Engines",
                severity="warning",
            )

    async def action_restart(self) -> None:
        """R — open service restart picker."""
        await self.push_screen(ServiceModal())

    async def action_test_tick(self) -> None:
        """T — inject a synthetic tick."""
        await self.push_screen(TestTickModal())

    async def action_backfill(self) -> None:
        """B — run historical backfill."""
        await self.push_screen(BackfillModal())

    async def action_backtest(self) -> None:
        """X — run backtest."""
        await self.push_screen(BacktestModal())

    async def action_eod(self) -> None:
        """E — EOD square-off all intraday positions."""
        confirmed = await self.push_screen_wait(
            ConfirmModal(
                "Trigger EOD square-off?\n"
                "Sends SIGTERM to market-engine → squares all intraday positions.",
                danger=True,
            )
        )
        if confirmed:
            loop = asyncio.get_event_loop()
            ok   = await loop.run_in_executor(
                None, lambda: stop_container("alpha-market-engine")
            )
            self.notify(
                "Sent SIGTERM to market-engine (EOD square-off)",
                title="EOD",
                severity="warning",
            )

    def action_cycle_log(self) -> None:
        """L — cycle log source in the log panel."""
        self.query_one(LogPanel).cycle_source()

    def action_quit(self) -> None:
        self.exit()


# ── Entry point ────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    app = AlphaTUI()
    app.run()
