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
from datetime import datetime

from textual.app import ComposeResult
from textual.screen import ModalScreen
from textual.widgets import Button, Input, Label, Select, Static
from textual.containers import Horizontal, Vertical

from commands import (
    SERVICES,
    ENGINE_CONTAINERS,
    TOKEN_SYMBOLS,
    get_backtest_instruments,
    start_container,
    stop_container,
    restart_container,
    start_all_engines,
    stop_all_engines,
    inject_test_tick,
    run_historical_backfill,
    run_backtest,
    export_instrument_to_alpha,
    date_to_start_ns,
    date_to_end_ns,
    get_container_logs,
    get_container_info,
    run_populate_backtest_ticks,
    run_signal_engine_batch,
    cleanup_backtest_data,
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
    """
    Configure and run the full backtest pipeline automatically.

    Workflow (fully automated, no manual steps):
      1. Backfill any missing data for the requested date range (smart skip if present)
      2. Populate backtest_ticks from QuestDB candles
      3. Export each instrument to .alpha binary
      4. Launch simulation and stream initial output
    """

    BINDINGS = [("escape", "dismiss", "Close")]

    def compose(self) -> ComposeResult:
        from datetime import date as _date, timedelta as _td
        today          = _date.today().strftime("%Y-%m-%d")
        three_years_ago = (_date.today() - _td(days=3 * 365)).strftime("%Y-%m-%d")

        with Vertical(id="bt-dialog"):
            yield Label(f"[bold {B}]Run Backtest — Top 10 NSE (3Y)[/]", id="modal-title")

            # ── Scrollable form body ──────────────────────────────────────────
            with Vertical(id="bt-form"):
                yield Label("From date (YYYY-MM-DD)", classes="modal-label")
                yield Input(value=three_years_ago, id="inp-bt-from", classes="modal-input",
                            placeholder="2023-04-18")

                yield Label("To date (YYYY-MM-DD)", classes="modal-label")
                yield Input(value=today, id="inp-bt-to", classes="modal-input",
                            placeholder="2024-12-31")

                yield Label("Starting capital (₹)", classes="modal-label")
                yield Input(value="1000000", id="inp-capital", classes="modal-input")

                yield Label("Brokerage per leg (₹)", classes="modal-label")
                yield Input(value="20", id="inp-brokerage", classes="modal-input")

                yield Label("Slippage (bps)", classes="modal-label")
                yield Input(value="2", id="inp-slippage", classes="modal-input")

                yield Label(
                    f"[{GR}]Pipeline: backfill → signals → ticks → export → simulate\n"
                    f"Instruments resolved live from PostgreSQL at run time[/]",
                    classes="modal-label",
                )

            # ── Always-visible footer: buttons + progress ─────────────────────
            with Horizontal(id="bt-buttons"):
                yield Button("✕ Cancel",  id="btn-cancel", classes="secondary")
                yield Button("▶ Run All", id="btn-run",    classes="primary")

            yield Static("", id="bt-output")

    def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "btn-cancel":
            self.dismiss()
            return
        self.query_one("#btn-run", Button).disabled = True
        asyncio.ensure_future(self._run())

    def _update(self, lines: list[str]) -> None:
        # Always show the LAST 10 lines so current step stays visible
        # (Static doesn't scroll — older lines are clipped at top)
        self.query_one("#bt-output", Static).update("\n".join(lines[-10:]))

    async def _run(self) -> None:
        loop = asyncio.get_event_loop()

        # ── Parse inputs ──────────────────────────────────────────────────────
        from_date = self.query_one("#inp-bt-from",   Input).value.strip()
        to_date   = self.query_one("#inp-bt-to",     Input).value.strip()

        try:
            datetime.strptime(from_date, "%Y-%m-%d")
            datetime.strptime(to_date,   "%Y-%m-%d")
        except ValueError:
            self._update([f"[{R}]Invalid date format — use YYYY-MM-DD.[/]"])
            self.query_one("#btn-run", Button).disabled = False
            return

        if from_date > to_date:
            self._update([f"[{R}]From date must be before To date.[/]"])
            self.query_one("#btn-run", Button).disabled = False
            return

        try:
            capital   = float(self.query_one("#inp-capital",   Input).value or 1_000_000)
            brokerage = float(self.query_one("#inp-brokerage", Input).value or 20)
            slippage  = float(self.query_one("#inp-slippage",  Input).value or 2)
        except ValueError:
            self._update([f"[{R}]Invalid number in simulation params.[/]"])
            self.query_one("#btn-run", Button).disabled = False
            return

        start_ns = date_to_start_ns(from_date)
        end_ns   = date_to_end_ns(to_date)
        # Dynamically resolve instrument IDs from PostgreSQL (falls back to TOKEN_SYMBOLS)
        instruments = await loop.run_in_executor(None, get_backtest_instruments)
        tokens   = list(instruments.items())
        lines: list[str] = []

        def abort(msg: str) -> None:
            lines.append(f"[{R}]{msg}[/]")
            self._update(lines)
            self.query_one("#btn-run", Button).disabled = False

        # ── Step 0: cleanup stale .alpha files from previous runs ──────────────
        lines += [f"[{B}]Step 0/6 — Cleaning up old backtest data[/]"]
        self._update(lines)
        await loop.run_in_executor(None, cleanup_backtest_data)
        lines[-1] = f"[{G}]✓ Old .alpha files removed[/]"
        self._update(lines)

        # ── Step 1: backfill missing data ─────────────────────────────────────
        # historical_feed.py already checks existing candles per chunk and skips
        # dates already present — so this is a no-op when data is up to date.
        lines += [
            f"[{B}]Step 1/6 — Backfill (auto-skip if data present)[/]",
            f"[{GR}]{from_date} → {to_date}  starting container…[/]",
        ]
        self._update(lines)

        cid = await loop.run_in_executor(
            None, lambda: run_historical_backfill(from_date, to_date)
        )
        if cid.startswith("ERROR"):
            return abort(f"Backfill launch failed: {cid}")

        lines[-1] = f"[{GR}]Container {cid} — waiting for completion…[/]"
        self._update(lines)

        dots = 0
        while True:
            await asyncio.sleep(2)
            info = await loop.run_in_executor(
                None, lambda: get_container_info("alpha-data-feed-historical-tui")
            )
            status = info.get("status", "absent")
            if status in ("exited", "dead", "absent"):
                ec = info.get("exit_code", -1)
                if ec == 0:
                    lines[-1] = f"[{G}]✓ Backfill done[/]"
                    self._update(lines)
                    break
                else:
                    # Fetch last few log lines to surface the error
                    err_logs = await loop.run_in_executor(
                        None,
                        lambda: get_container_logs("alpha-data-feed-historical-tui", tail=5),
                    )
                    lines[-1] = f"[{R}]✗ Backfill exit {ec}[/]"
                    if err_logs.strip():
                        lines.append(f"[{R}]{err_logs.strip()[-300:]}[/]")
                    return abort("Aborting pipeline.")
            dots = (dots % 3) + 1
            lines[-1] = f"[{GR}]Backfill {status}{'.' * dots}[/]"
            self._update(lines)

        # ── Step 2: compute OHLCV signals (RSI, MACD, BB, VWAP dev) ─────────────
        lines += ["", f"[{B}]Step 2/6 — Computing bar signals per instrument[/]"]
        self._update(lines)

        for i, (tok, sym) in enumerate(tokens, 1):
            lines.append(f"[{GR}]  [{i}/{len(tokens)}] {sym:<12} computing signals…[/]")
            self._update(lines)
            result = await loop.run_in_executor(
                None,
                lambda t=tok, s=sym: run_signal_engine_batch(t, s, start_ns, end_ns),
            )
            if result.startswith("ERROR"):
                lines[-1] = f"[{R}]  [{i}/{len(tokens)}] {sym:<12} ✗ {result}[/]"
                self._update(lines)
                return abort(f"Signal computation failed for {sym} — aborting.")
            else:
                lines[-1] = f"[{G}]  [{i}/{len(tokens)}] {sym:<12} ✓ signals ready[/]"
            self._update(lines)

        lines.append(f"[{G}]Signal computation done.[/]")
        self._update(lines)

        # ── Step 3: populate backtest_ticks ───────────────────────────────────
        lines += ["", f"[{B}]Step 3/6 — Populating backtest_ticks[/]",
                  f"[{GR}]QuestDB candles → backtest_ticks…[/]"]
        self._update(lines)

        result = await loop.run_in_executor(None, run_populate_backtest_ticks)
        if result.startswith("ERROR"):
            lines[-1] = f"[{R}]✗ {result}[/]"
            return abort("Aborting pipeline.")
        lines[-1] = f"[{G}]✓ backtest_ticks ready[/]"
        self._update(lines)

        # ── Step 4: export .alpha files ───────────────────────────────────────
        lines += ["", f"[{B}]Step 4/6 — Exporting .alpha files[/]"]
        self._update(lines)

        for i, (tok, sym) in enumerate(tokens, 1):
            lines.append(f"[{GR}]  [{i}/{len(tokens)}] {sym:<12} exporting…[/]")
            self._update(lines)

            result = await loop.run_in_executor(
                None, lambda t=tok: export_instrument_to_alpha(t, start_ns, end_ns)
            )
            if result.startswith("ERROR"):
                lines[-1] = f"[{R}]  [{i}/{len(tokens)}] {sym:<12} ✗ {result}[/]"
                return abort("Export failed — aborting.")

            tick_info = next(
                (f"{int(w):,} ticks" for w in result.split() if w.isdigit()), ""
            )
            lines[-1] = f"[{G}]  [{i}/{len(tokens)}] {sym:<12} ✓  {tick_info}[/]"
            self._update(lines)

        lines.append(f"[{G}]All exports done.[/]")
        self._update(lines)

        # ── Step 5: run simulation ────────────────────────────────────────────
        lines += ["", f"[{B}]Step 5/6 — Launching simulation…[/]"]
        self._update(lines)

        cid = await loop.run_in_executor(
            None, lambda: run_backtest(capital, brokerage, slippage)
        )
        if cid.startswith("ERROR"):
            return abort(f"Failed to start backtest: {cid}")

        lines.append(
            f"[{G}]✓ Started: {cid}[/]  "
            f"[{GR}]₹{capital:,.0f}  brok=₹{brokerage}  slip={slippage}bps[/]"
        )
        self._update(lines)

        # Poll until the backtest container exits (may take several minutes for 3Y data)
        dots = 0
        sim_deadline = loop.time() + 600  # 10-minute hard timeout
        while True:
            await asyncio.sleep(1.0)
            info = await loop.run_in_executor(
                None, lambda: get_container_info("alpha-backtest-tui")
            )
            status = info.get("status", "absent")
            if status in ("exited", "dead"):
                ec = info.get("exit_code", -1)
                if ec == 0:
                    lines[-1] = f"[{G}]✓ Simulation complete[/]"
                else:
                    lines[-1] = f"[{R}]✗ Simulation exit {ec}[/]"
                self._update(lines)
                break
            if status == "absent":
                # Container cleaned up before we polled — check logs to determine outcome
                logs_quick = await loop.run_in_executor(
                    None, lambda: get_container_logs("alpha-backtest-tui", tail=5)
                )
                if "total_net_pnl" in logs_quick or "Completed" in logs_quick:
                    lines[-1] = f"[{G}]✓ Simulation complete[/]"
                else:
                    lines[-1] = f"[{R}]✗ Container absent — may have been removed[/]"
                self._update(lines)
                break
            if loop.time() > sim_deadline:
                lines[-1] = f"[{R}]✗ Simulation timed out (>10 min)[/]"
                self._update(lines)
                break
            dots = (dots % 3) + 1
            lines[-1] = (
                f"[{G}]✓ Started: {cid}[/]  "
                f"[{GR}]simulating{'.' * dots}[/]"
            )
            self._update(lines)

        # Fetch and display results — escape [ ] to avoid Rich markup conflicts
        logs = await loop.run_in_executor(
            None, lambda: get_container_logs("alpha-backtest-tui", tail=30)
        )
        if logs.strip():
            # Strip timestamps, escape brackets so Rich doesn't misparse [backtest] etc.
            clean = "\n".join(
                l.split("Z ", 1)[-1] if "Z " in l else l
                for l in logs.strip().splitlines()
            )
            safe = clean[-600:].replace("[", "\\[")
            lines += [f"[{GR}]── results ──[/]", f"[dim]{safe}[/dim]"]
            self._update(lines)

        # ── Auto-close on success ─────────────────────────────────────────────
        if any("✓ Simulation complete" in ln for ln in lines):
            lines.append(f"\n[{G}]✓ Pipeline finished. Closing automatically in 3s...[/]")
            self._update(lines)
            await asyncio.sleep(3.0)
            try:
                self.dismiss()
            except Exception:
                pass  # Already dismissed via Cancel?
