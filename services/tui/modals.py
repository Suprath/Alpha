"""
modals.py — All modal screens for the Alpha Terminal UI.

  ConfirmModal        — generic yes/no confirmation
  ServiceModal        — start / stop / restart a specific container
  TestTickModal       — inject a synthetic tick into Redis
  BackfillModal       — run historical data backfill
  BacktestModal       — run a backtest with config params
"""
from __future__ import annotations

import asyncio

from textual.app import ComposeResult
from textual.screen import ModalScreen
from textual.widgets import Button, Input, Label, Select, Static
from textual.containers import Horizontal, Vertical

from commands import (
    SERVICES,
    ENGINE_CONTAINERS,
    TOKEN_SYMBOLS,
    start_container,
    stop_container,
    restart_container,
    start_all_engines,
    stop_all_engines,
    inject_test_tick,
    run_historical_backfill,
    run_backtest,
    get_container_logs,
)

B   = "#58a6ff"
G   = "#3fb950"
R   = "#f85149"
GR  = "#8b949e"
W   = "#e6edf3"


# ── Confirm Modal ──────────────────────────────────────────────────────────────

class ConfirmModal(ModalScreen[bool]):
    """Simple yes/no confirmation dialog."""

    def __init__(self, message: str, danger: bool = False) -> None:
        super().__init__()
        self._message = message
        self._danger  = danger

    def compose(self) -> ComposeResult:
        with Vertical(id="confirm-dialog"):
            yield Label(f"[bold {B}]Confirm Action[/]", id="modal-title")
            yield Label(self._message, id="confirm-message")
            with Horizontal(id="modal-buttons"):
                yield Button("Cancel", variant="default", id="btn-cancel",
                             classes="secondary")
                yield Button(
                    "Confirm",
                    variant="error" if self._danger else "success",
                    id="btn-confirm",
                    classes="danger" if self._danger else "primary",
                )

    def on_button_pressed(self, event: Button.Pressed) -> None:
        self.dismiss(event.button.id == "btn-confirm")


# ── Service Control Modal ──────────────────────────────────────────────────────

class ServiceModal(ModalScreen):
    """Start / stop / restart a specific container."""

    BINDINGS = [("escape", "dismiss", "Close")]

    def compose(self) -> ComposeResult:
        svc_options = [(dname, cname) for dname, cname in SERVICES]

        with Vertical(id="modal-dialog"):
            yield Label(f"[bold {B}]Service Control[/]", id="modal-title")

            yield Label("Container", classes="modal-label")
            yield Select(
                [(dname, cname) for dname, cname in SERVICES],
                id="svc-select",
                classes="modal-select",
                prompt="Select container…",
            )

            with Horizontal(id="modal-buttons"):
                yield Button("✕ Close",   id="btn-close",   classes="secondary")
                yield Button("■ Stop",    id="btn-stop",    classes="danger")
                yield Button("▶ Start",   id="btn-start",   classes="primary")
                yield Button("↻ Restart", id="btn-restart", classes="primary")

            yield Static("", id="modal-output")

    def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "btn-close":
            self.dismiss()
            return

        sel = self.query_one("#svc-select", Select)
        cname = sel.value
        if cname is Select.BLANK:
            self.query_one("#modal-output", Static).update(
                f"[{R}]Select a container first.[/]"
            )
            return

        out = self.query_one("#modal-output", Static)
        btn = event.button.id

        asyncio.ensure_future(self._run_action(btn, cname, out))

    async def _run_action(
        self, btn: str, cname: str, out: Static
    ) -> None:
        loop = asyncio.get_event_loop()
        out.update(f"[{GR}]Working…[/]")

        if btn == "btn-start":
            ok = await loop.run_in_executor(None, start_container, cname)
            out.update(
                f"[{G}]Started {cname}[/]" if ok
                else f"[{R}]Failed to start {cname}[/]"
            )
        elif btn == "btn-stop":
            ok = await loop.run_in_executor(None, stop_container, cname)
            out.update(
                f"[{G}]Stopped {cname}[/]" if ok
                else f"[{R}]Failed to stop {cname}[/]"
            )
        elif btn == "btn-restart":
            ok = await loop.run_in_executor(None, restart_container, cname)
            out.update(
                f"[{G}]Restarted {cname}[/]" if ok
                else f"[{R}]Failed to restart {cname}[/]"
            )


# ── Test Tick Modal ────────────────────────────────────────────────────────────

class TestTickModal(ModalScreen):
    """Inject a synthetic tick into Redis alpha:ticks."""

    BINDINGS = [("escape", "dismiss", "Close")]

    def compose(self) -> ComposeResult:
        tok_options = [
            (f"{sym} ({tok})", tok)
            for tok, sym in TOKEN_SYMBOLS.items()
        ]

        with Vertical(id="modal-dialog"):
            yield Label(f"[bold {B}]Inject Test Tick[/]", id="modal-title")

            yield Label("Instrument", classes="modal-label")
            yield Select(tok_options, id="tok-select", classes="modal-select",
                         prompt="Select instrument…")

            yield Label("Price", classes="modal-label")
            yield Input(placeholder="e.g. 2750.50", id="inp-price",
                        classes="modal-input")

            yield Label("Bid size (OFI trigger ≥ 50,000)", classes="modal-label")
            yield Input(placeholder="e.g. 60000", id="inp-bid-size",
                        classes="modal-input")

            yield Label("Volume", classes="modal-label")
            yield Input(placeholder="e.g. 1000", id="inp-volume",
                        classes="modal-input")

            with Horizontal(id="modal-buttons"):
                yield Button("✕ Cancel", id="btn-cancel", classes="secondary")
                yield Button("⚡ Inject", id="btn-inject", classes="primary")

            yield Static("", id="modal-output")

    def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "btn-cancel":
            self.dismiss()
            return

        asyncio.ensure_future(self._inject())

    async def _inject(self) -> None:
        out = self.query_one("#modal-output", Static)
        sel = self.query_one("#tok-select", Select)
        tok_str = sel.value
        if tok_str is Select.BLANK:
            out.update(f"[{R}]Select an instrument.[/]")
            return

        try:
            price    = float(self.query_one("#inp-price",    Input).value or 0)
            bid_size = int(self.query_one("#inp-bid-size", Input).value or 0)
            volume   = int(self.query_one("#inp-volume",   Input).value or 0)
        except ValueError:
            out.update(f"[{R}]Invalid number.[/]")
            return

        token  = int(tok_str)
        symbol = TOKEN_SYMBOLS.get(tok_str, f"T_{tok_str}")
        spread = max(price * 0.0002, 0.05)

        loop = asyncio.get_event_loop()
        ok   = await loop.run_in_executor(
            None,
            lambda: inject_test_tick(
                token,
                price,
                price - spread,
                price + spread,
                bid_size,
                volume,
                symbol,
            )
        )
        if ok:
            out.update(
                f"[{G}]✓ Injected: {symbol} @ ₹{price:,.2f}  "
                f"bid_size={bid_size:,}[/]\n"
                f"[{GR}]OFI signal fires if bid_size ≥ 50,000[/]"
            )
        else:
            out.update(f"[{R}]Failed — is Redis reachable?[/]")


# ── Backfill Modal ─────────────────────────────────────────────────────────────

class BackfillModal(ModalScreen):
    """Run historical data backfill."""

    BINDINGS = [("escape", "dismiss", "Close")]

    def compose(self) -> ComposeResult:
        interval_opts = [
            ("1 minute", "1minute"),
            ("5 minutes", "5minute"),
            ("15 minutes", "15minute"),
            ("30 minutes", "30minute"),
            ("60 minutes", "60minute"),
            ("Daily", "day"),
        ]

        with Vertical(id="modal-dialog"):
            yield Label(f"[bold {B}]Historical Backfill[/]", id="modal-title")

            yield Label("From date (YYYY-MM-DD)", classes="modal-label")
            yield Input(placeholder="2026-04-01", id="inp-from",
                        classes="modal-input")

            yield Label("To date (YYYY-MM-DD)", classes="modal-label")
            yield Input(placeholder="2026-04-16", id="inp-to",
                        classes="modal-input")

            yield Label("Interval", classes="modal-label")
            yield Select(interval_opts, id="sel-interval", classes="modal-select",
                         value="1minute")

            with Horizontal(id="modal-buttons"):
                yield Button("✕ Cancel", id="btn-cancel", classes="secondary")
                yield Button("▶ Run", id="btn-run", classes="primary")

            yield Static("", id="modal-output")

    def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "btn-cancel":
            self.dismiss()
            return
        asyncio.ensure_future(self._run())

    async def _run(self) -> None:
        out      = self.query_one("#modal-output", Static)
        from_d   = self.query_one("#inp-from",      Input).value.strip()
        to_d     = self.query_one("#inp-to",        Input).value.strip()
        interval = self.query_one("#sel-interval",  Select).value

        if not from_d or not to_d:
            out.update(f"[{R}]Both dates are required.[/]")
            return

        out.update(f"[{GR}]Launching backfill container…[/]")
        loop = asyncio.get_event_loop()
        cid  = await loop.run_in_executor(
            None,
            lambda: run_historical_backfill(from_d, to_d, str(interval))
        )

        if cid.startswith("ERROR"):
            out.update(f"[{R}]{cid}[/]")
        else:
            out.update(
                f"[{G}]Container started: {cid}[/]\n"
                f"[{GR}]Backfill: {from_d} → {to_d}  interval={interval}\n"
                f"Monitor via: docker logs alpha-data-feed-historical-tui -f[/]"
            )


# ── Backtest Modal ─────────────────────────────────────────────────────────────

class BacktestModal(ModalScreen):
    """Configure and run the backtest engine."""

    BINDINGS = [("escape", "dismiss", "Close")]

    def compose(self) -> ComposeResult:
        with Vertical(id="modal-dialog"):
            yield Label(f"[bold {B}]Run Backtest[/]", id="modal-title")

            yield Label("Starting capital (₹)", classes="modal-label")
            yield Input(value="1000000", id="inp-capital", classes="modal-input")

            yield Label("Brokerage per leg (₹)", classes="modal-label")
            yield Input(value="20", id="inp-brokerage", classes="modal-input")

            yield Label("Slippage (bps)", classes="modal-label")
            yield Input(value="2", id="inp-slippage", classes="modal-input")

            yield Label(
                f"[{GR}]Note: .alpha files in the backtest volume are used.\n"
                f"Run a backfill first, then use 'backtest export' to generate them.[/]",
                classes="modal-label"
            )

            with Horizontal(id="modal-buttons"):
                yield Button("✕ Cancel", id="btn-cancel", classes="secondary")
                yield Button("▶ Run",    id="btn-run",    classes="primary")

            yield Static("", id="modal-output")

    def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "btn-cancel":
            self.dismiss()
            return
        asyncio.ensure_future(self._run())

    async def _run(self) -> None:
        out = self.query_one("#modal-output", Static)

        try:
            capital    = float(self.query_one("#inp-capital",   Input).value or 1_000_000)
            brokerage  = float(self.query_one("#inp-brokerage", Input).value or 20)
            slippage   = float(self.query_one("#inp-slippage",  Input).value or 2)
        except ValueError:
            out.update(f"[{R}]Invalid number — check inputs.[/]")
            return

        out.update(f"[{GR}]Launching backtest container…[/]")
        loop = asyncio.get_event_loop()
        cid  = await loop.run_in_executor(
            None,
            lambda: run_backtest(capital, brokerage, slippage)
        )

        if cid.startswith("ERROR"):
            out.update(f"[{R}]{cid}[/]")
        else:
            out.update(
                f"[{G}]Container started: {cid}[/]\n"
                f"[{GR}]capital=₹{capital:,.0f}  brok=₹{brokerage}  slip={slippage}bps\n"
                f"Monitor: docker logs alpha-backtest-tui -f[/]"
            )
            # Stream backtest output into the modal
            await asyncio.sleep(2.0)
            loop2 = asyncio.get_event_loop()
            logs  = await loop2.run_in_executor(
                None,
                lambda: get_container_logs("alpha-backtest-tui", tail=60)
            )
            if logs.strip():
                out.update(
                    f"[{G}]Container: {cid}[/]\n"
                    f"[{W}]{logs[:800]}[/]"
                )
