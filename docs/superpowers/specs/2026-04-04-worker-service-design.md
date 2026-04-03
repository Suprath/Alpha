# Worker Service Design

**Date:** 2026-04-04  
**Status:** Approved

## Overview

Refactor `services/instrument_worker` into a general-purpose `worker_service` — a single Docker container that hosts multiple workers. Each worker runs as a Python class in its own folder. Workers can be triggered via CLI argument, HTTP API, or on a fixed cron schedule defined in `workers.yaml`. The existing NSE instrument loader becomes the first worker inside this service.

## Directory Structure

```
services/worker_service/
├── workers/
│   └── instrument_loader/
│       ├── __init__.py
│       └── worker.py          # InstrumentLoaderWorker(BaseWorker)
├── core/
│   ├── __init__.py
│   ├── base_worker.py         # Abstract BaseWorker with run() interface
│   ├── registry.py            # Auto-discovers worker classes from workers/*/worker.py
│   └── scheduler.py           # APScheduler setup; reads workers.yaml on startup
├── api/
│   ├── __init__.py
│   └── routes.py              # FastAPI router: POST /run/<worker_name>
├── workers.yaml               # Per-worker config (schedule, enabled)
├── main.py                    # Entry point: CLI mode or server mode
├── requirements.txt
└── Dockerfile
```

## workers.yaml Format

```yaml
instrument_loader:
  schedule: "0 6 * * *"   # cron syntax (IST); omit for on-call only
  enabled: true

example_on_call_worker:
  enabled: true            # no schedule — API/CLI trigger only
```

- `schedule` is optional. Workers without a schedule are never auto-run by the scheduler.
- `enabled: false` disables the worker from all trigger modes.

## Components

### `core/base_worker.py`
Abstract base class all workers must extend:
```python
class BaseWorker(ABC):
    @abstractmethod
    def run(self) -> None: ...
```

### `core/registry.py`
Auto-discovers workers by scanning `workers/*/worker.py`, importing each module, and finding the class that extends `BaseWorker`. Workers are keyed by their folder name (e.g., `instrument_loader`). No manual registration required — adding a folder is sufficient.

### `core/scheduler.py`
Reads `workers.yaml` on startup. For each enabled worker with a `schedule` field, registers a cron job with APScheduler using the cron expression. Calls the worker's `run()` method in a background thread so the scheduler loop is never blocked.

### `api/routes.py`
Single FastAPI router:
- `POST /run/{worker_name}` — looks up worker in registry, validates it exists and is enabled, fires `worker.run()` via `BackgroundTasks`, returns `202 Accepted` immediately.
- Logs printed inside `run()` appear in container stdout, showing start, progress, and completion.

### `main.py`
Two modes:
- **Server mode** (no args): starts APScheduler + FastAPI via uvicorn on port `8000`.
- **CLI mode** (`--worker <name>`): imports registry, runs the named worker synchronously, exits. No HTTP server or scheduler started.

## Trigger Modes Summary

| Mode | How | Behavior |
|---|---|---|
| CLI | `--worker instrument_loader` | Runs synchronously, exits |
| API | `POST /run/instrument_loader` | Fires async, returns 202 |
| Schedule | Defined in `workers.yaml` | Auto-runs on cron, async |

## Adding a New Worker

1. Create `workers/my_worker/worker.py` with a class extending `BaseWorker` and implementing `run()`.
2. Add an entry in `workers.yaml` (with or without `schedule`).
3. No changes to core code required — registry auto-discovers it.

## docker-compose Changes

- Rename service from `instrument-worker` to `worker-service`.
- Default `command` runs server mode (no args).
- For one-shot CLI mode, override `command: ["python", "main.py", "--worker", "instrument_loader"]`.

## Dependencies

- `fastapi`, `uvicorn` — HTTP server
- `apscheduler` — cron scheduling
- `pyyaml` — parse `workers.yaml`
- `psycopg2-binary`, `requests` — existing instrument loader deps
