# Worker Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `services/instrument_worker` into a general-purpose `worker_service` container that hosts multiple workers, each in its own folder, triggerable via CLI, HTTP API, or cron schedule.

**Architecture:** A FastAPI + APScheduler server runs inside a single Docker container. Workers are auto-discovered from `workers/*/worker.py` and registered by name. `workers.yaml` declares per-worker cron schedules (optional) and enabled flags. CLI mode (`--worker <name>`) bypasses the server and runs a single worker synchronously.

**Tech Stack:** Python 3.11, FastAPI, uvicorn, APScheduler 3.x, PyYAML, psycopg2-binary, requests

---

## File Map

| Action | Path | Responsibility |
|---|---|---|
| Delete | `services/instrument_worker/main.py` | Replaced by new structure |
| Delete | `services/instrument_worker/Dockerfile` | Replaced |
| Delete | `services/instrument_worker/requirements.txt` | Replaced |
| Create | `services/worker_service/main.py` | Entry point: CLI or server mode |
| Create | `services/worker_service/workers.yaml` | Per-worker schedule config |
| Create | `services/worker_service/requirements.txt` | Python dependencies |
| Create | `services/worker_service/Dockerfile` | Container definition |
| Create | `services/worker_service/core/__init__.py` | Package marker |
| Create | `services/worker_service/core/base_worker.py` | Abstract BaseWorker |
| Create | `services/worker_service/core/registry.py` | Auto-discovery of workers |
| Create | `services/worker_service/core/scheduler.py` | APScheduler cron setup |
| Create | `services/worker_service/api/__init__.py` | Package marker |
| Create | `services/worker_service/api/routes.py` | POST /run/{worker_name} |
| Create | `services/worker_service/workers/__init__.py` | Package marker |
| Create | `services/worker_service/workers/instrument_loader/__init__.py` | Package marker |
| Create | `services/worker_service/workers/instrument_loader/worker.py` | Migrated instrument loader |
| Modify | `docker-compose.yml` | Rename instrument-worker → worker-service |
| Create | `tests/unit/worker_service/test_registry.py` | Registry unit tests |
| Create | `tests/unit/worker_service/test_routes.py` | API route tests |

---

### Task 1: Create BaseWorker and core package

**Files:**
- Create: `services/worker_service/core/__init__.py`
- Create: `services/worker_service/core/base_worker.py`
- Create: `tests/unit/worker_service/test_base_worker.py`

- [ ] **Step 1: Create the test file**

```python
# tests/unit/worker_service/test_base_worker.py
import pytest
from services.worker_service.core.base_worker import BaseWorker

def test_base_worker_is_abstract():
    with pytest.raises(TypeError):
        BaseWorker()

def test_concrete_worker_must_implement_run():
    class BadWorker(BaseWorker):
        pass
    with pytest.raises(TypeError):
        BadWorker()

def test_concrete_worker_with_run_is_valid():
    class GoodWorker(BaseWorker):
        def run(self):
            pass
    worker = GoodWorker()
    assert hasattr(worker, 'run')
```

- [ ] **Step 2: Run test to confirm it fails**

```bash
docker-compose run --rm worker-service pytest tests/unit/worker_service/test_base_worker.py -v
```
Expected: `ModuleNotFoundError` or `ImportError`

- [ ] **Step 3: Create core package files**

```python
# services/worker_service/core/__init__.py
```

```python
# services/worker_service/core/base_worker.py
from abc import ABC, abstractmethod


class BaseWorker(ABC):
    @abstractmethod
    def run(self) -> None:
        """Execute the worker logic. Called by scheduler, API, or CLI."""
        ...
```

- [ ] **Step 4: Run test to confirm it passes**

```bash
docker-compose run --rm worker-service pytest tests/unit/worker_service/test_base_worker.py -v
```
Expected: 3 PASSED

- [ ] **Step 5: Commit**

```bash
git add services/worker_service/core/ tests/unit/worker_service/test_base_worker.py
git commit -m "feat(worker-service): add BaseWorker abstract class"
```

---

### Task 2: Create worker registry

**Files:**
- Create: `services/worker_service/core/registry.py`
- Create: `tests/unit/worker_service/test_registry.py`

- [ ] **Step 1: Write the failing test**

```python
# tests/unit/worker_service/test_registry.py
import pytest
from unittest.mock import patch, MagicMock
from services.worker_service.core.registry import WorkerRegistry
from services.worker_service.core.base_worker import BaseWorker


def test_registry_returns_empty_when_no_workers(tmp_path):
    registry = WorkerRegistry(workers_dir=str(tmp_path))
    assert registry.all() == {}


def test_registry_get_unknown_worker_returns_none(tmp_path):
    registry = WorkerRegistry(workers_dir=str(tmp_path))
    assert registry.get("nonexistent") is None


def test_registry_discovers_valid_worker(tmp_path):
    # Create a fake worker module structure
    worker_dir = tmp_path / "my_worker"
    worker_dir.mkdir()
    (worker_dir / "__init__.py").write_text("")
    (worker_dir / "worker.py").write_text("""
from services.worker_service.core.base_worker import BaseWorker
class MyWorker(BaseWorker):
    def run(self):
        pass
""")
    registry = WorkerRegistry(workers_dir=str(tmp_path))
    assert "my_worker" in registry.all()
    instance = registry.get("my_worker")
    assert isinstance(instance, BaseWorker)
```

- [ ] **Step 2: Run test to confirm it fails**

```bash
docker-compose run --rm worker-service pytest tests/unit/worker_service/test_registry.py -v
```
Expected: `ImportError` — registry module not found

- [ ] **Step 3: Implement registry**

```python
# services/worker_service/core/registry.py
import os
import importlib.util
import inspect
from typing import Dict, Optional

from services.worker_service.core.base_worker import BaseWorker

_DEFAULT_WORKERS_DIR = os.path.join(os.path.dirname(__file__), "..", "workers")


class WorkerRegistry:
    def __init__(self, workers_dir: str = _DEFAULT_WORKERS_DIR):
        self._workers: Dict[str, type] = {}
        self._discover(os.path.abspath(workers_dir))

    def _discover(self, workers_dir: str) -> None:
        if not os.path.isdir(workers_dir):
            return
        for name in os.listdir(workers_dir):
            worker_file = os.path.join(workers_dir, name, "worker.py")
            if not os.path.isfile(worker_file):
                continue
            spec = importlib.util.spec_from_file_location(f"workers.{name}.worker", worker_file)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            for _, obj in inspect.getmembers(module, inspect.isclass):
                if issubclass(obj, BaseWorker) and obj is not BaseWorker:
                    self._workers[name] = obj
                    break

    def get(self, name: str) -> Optional[BaseWorker]:
        cls = self._workers.get(name)
        return cls() if cls else None

    def all(self) -> Dict[str, type]:
        return dict(self._workers)
```

- [ ] **Step 4: Run test to confirm it passes**

```bash
docker-compose run --rm worker-service pytest tests/unit/worker_service/test_registry.py -v
```
Expected: 3 PASSED

- [ ] **Step 5: Commit**

```bash
git add services/worker_service/core/registry.py tests/unit/worker_service/test_registry.py
git commit -m "feat(worker-service): add auto-discovering WorkerRegistry"
```

---

### Task 3: Migrate instrument loader worker

**Files:**
- Create: `services/worker_service/workers/__init__.py`
- Create: `services/worker_service/workers/instrument_loader/__init__.py`
- Create: `services/worker_service/workers/instrument_loader/worker.py`

- [ ] **Step 1: Create package markers**

```python
# services/worker_service/workers/__init__.py
```

```python
# services/worker_service/workers/instrument_loader/__init__.py
```

- [ ] **Step 2: Create the instrument loader worker**

Migrate logic from `services/instrument_worker/main.py` (`ensure_db_schema` and `fetch_and_load`) into a class:

```python
# services/worker_service/workers/instrument_loader/worker.py
import os
import gzip
import json
import time
import requests
import psycopg2
from psycopg2.extras import execute_values
from datetime import datetime

from services.worker_service.core.base_worker import BaseWorker

DB_HOST = os.getenv("POSTGRES_HOST", "postgres-db")
DB_PORT = os.getenv("POSTGRES_PORT", "5432")
DB_NAME = os.getenv("POSTGRES_DB", "alpha_db")
DB_USER = os.getenv("POSTGRES_USER", "alpha_user")
DB_PASS = os.getenv("POSTGRES_PASSWORD", "alpha_password")
TEST_MODE = os.getenv("TEST_MODE", "0") == "1"

URL = "https://assets.upstox.com/market-quote/instruments/exchange/NSE.json.gz"


class InstrumentLoaderWorker(BaseWorker):
    def run(self) -> None:
        print("[instrument_loader] Starting...")
        conn = self._connect_with_retry()
        if not conn:
            print("[instrument_loader] Failed to connect to database. Aborting.")
            return
        try:
            self._ensure_schema(conn)
            self._fetch_and_load(conn)
        finally:
            conn.close()
        print("[instrument_loader] Finished successfully.")

    def _connect_with_retry(self):
        retries = 15
        while retries > 0:
            try:
                return psycopg2.connect(
                    host=DB_HOST, port=DB_PORT, dbname=DB_NAME,
                    user=DB_USER, password=DB_PASS
                )
            except Exception as e:
                print(f"[instrument_loader] DB not ready, retrying... ({e})")
                time.sleep(2)
                retries -= 1
        return None

    def _ensure_schema(self, conn):
        with conn.cursor() as cur:
            cur.execute("""
                CREATE TABLE IF NOT EXISTS instrument_universe (
                    id SERIAL PRIMARY KEY,
                    date DATE NOT NULL,
                    instrument_key VARCHAR(64) NOT NULL,
                    trading_symbol VARCHAR(128),
                    name VARCHAR(256),
                    exchange VARCHAR(16),
                    segment VARCHAR(32),
                    lot_size INT,
                    tick_size FLOAT,
                    instrument_type VARCHAR(16),
                    expiry_ms BIGINT,
                    strike_price FLOAT,
                    UNIQUE(date, instrument_key)
                );
            """)
        conn.commit()

    def _fetch_and_load(self, conn):
        print(f"[instrument_loader] Downloading NSE instruments from {URL}...")
        headers = {'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'}
        response = requests.get(URL, headers=headers, stream=True)
        response.raise_for_status()

        raw_json = gzip.decompress(response.content).decode('utf-8')
        try:
            json_data = json.loads(raw_json)
        except json.JSONDecodeError:
            json_data = [json.loads(line) for line in raw_json.splitlines() if line.strip()]

        print(f"[instrument_loader] Parsed {len(json_data)} instruments.")

        today = datetime.now().date()
        target_keys = ["NSE_EQ|INE002A01018", "NSE_INDEX|Nifty 50", "NSE_INDEX|Nifty Bank"]
        records = []

        for item in json_data:
            key = item.get("instrument_key", "")
            if not key:
                continue
            if TEST_MODE and key not in target_keys:
                continue

            try:
                strike = float(item.get("strike_price", 0.0) or 0.0)
            except (ValueError, TypeError):
                strike = 0.0

            records.append((
                today,
                key,
                item.get("trading_symbol"),
                item.get("name", ""),
                item.get("exchange", ""),
                item.get("segment", ""),
                int(item.get("lot_size", 1) or 1),
                float(item.get("tick_size", 0.0) or 0.0),
                item.get("instrument_type", ""),
                int(item.get("expiry", 0) or 0),
                strike,
            ))

            if TEST_MODE and len(records) >= 3:
                print("[instrument_loader] TEST_MODE: matched 3 target instruments.")
                break

        print(f"[instrument_loader] Inserting {len(records)} records into PostgreSQL...")
        with conn.cursor() as cur:
            execute_values(cur, """
                INSERT INTO instrument_universe
                (date, instrument_key, trading_symbol, name, exchange, segment,
                 lot_size, tick_size, instrument_type, expiry_ms, strike_price)
                VALUES %s
                ON CONFLICT (date, instrument_key) DO NOTHING;
            """, records)
        conn.commit()
        print("[instrument_loader] Insertion complete.")
```

- [ ] **Step 3: Verify registry discovers the worker**

```bash
docker-compose run --rm worker-service python -c "
from services.worker_service.core.registry import WorkerRegistry
r = WorkerRegistry()
print('Discovered workers:', list(r.all().keys()))
assert 'instrument_loader' in r.all()
print('OK')
"
```
Expected: `Discovered workers: ['instrument_loader']` then `OK`

- [ ] **Step 4: Commit**

```bash
git add services/worker_service/workers/
git commit -m "feat(worker-service): migrate instrument loader as first worker"
```

---

### Task 4: Create workers.yaml and scheduler

**Files:**
- Create: `services/worker_service/workers.yaml`
- Create: `services/worker_service/core/scheduler.py`

- [ ] **Step 1: Create workers.yaml**

```yaml
# services/worker_service/workers.yaml
instrument_loader:
  schedule: "0 6 * * *"   # 6:00 AM IST daily
  enabled: true
```

- [ ] **Step 2: Write the scheduler**

```python
# services/worker_service/core/scheduler.py
import os
import threading
import yaml
from apscheduler.schedulers.background import BackgroundScheduler

from services.worker_service.core.registry import WorkerRegistry

_CONFIG_PATH = os.path.join(os.path.dirname(__file__), "..", "workers.yaml")


def _run_worker_in_thread(registry: WorkerRegistry, name: str) -> None:
    worker = registry.get(name)
    if worker:
        try:
            worker.run()
        except Exception as e:
            print(f"[scheduler] Worker '{name}' raised an exception: {e}")


def build_scheduler(registry: WorkerRegistry, config_path: str = _CONFIG_PATH) -> BackgroundScheduler:
    scheduler = BackgroundScheduler(timezone="Asia/Kolkata")

    with open(config_path, "r") as f:
        config = yaml.safe_load(f) or {}

    for name, cfg in config.items():
        if not cfg.get("enabled", True):
            continue
        schedule = cfg.get("schedule")
        if not schedule:
            continue  # on-call only — no automatic scheduling
        parts = schedule.split()
        if len(parts) != 5:
            print(f"[scheduler] Invalid cron expression for '{name}': {schedule}")
            continue
        minute, hour, day, month, day_of_week = parts
        scheduler.add_job(
            _run_worker_in_thread,
            trigger="cron",
            args=[registry, name],
            minute=minute,
            hour=hour,
            day=day,
            month=month,
            day_of_week=day_of_week,
            id=name,
            replace_existing=True,
        )
        print(f"[scheduler] Registered '{name}' with schedule '{schedule}'")

    return scheduler
```

- [ ] **Step 3: Verify scheduler registers jobs**

```bash
docker-compose run --rm worker-service python -c "
from services.worker_service.core.registry import WorkerRegistry
from services.worker_service.core.scheduler import build_scheduler
r = WorkerRegistry()
s = build_scheduler(r)
jobs = s.get_jobs()
print('Scheduled jobs:', [j.id for j in jobs])
assert any(j.id == 'instrument_loader' for j in jobs)
print('OK')
"
```
Expected: `Scheduled jobs: ['instrument_loader']` then `OK`

- [ ] **Step 4: Commit**

```bash
git add services/worker_service/workers.yaml services/worker_service/core/scheduler.py
git commit -m "feat(worker-service): add workers.yaml and APScheduler cron setup"
```

---

### Task 5: Create FastAPI routes

**Files:**
- Create: `services/worker_service/api/__init__.py`
- Create: `services/worker_service/api/routes.py`
- Create: `tests/unit/worker_service/test_routes.py`

- [ ] **Step 1: Write the failing tests**

```python
# tests/unit/worker_service/test_routes.py
import pytest
from unittest.mock import MagicMock, patch
from fastapi.testclient import TestClient
from services.worker_service.api.routes import build_router
from services.worker_service.core.base_worker import BaseWorker
import fastapi


class FakeWorker(BaseWorker):
    def run(self):
        pass


def make_client(workers: dict):
    registry = MagicMock()
    registry.get.side_effect = lambda name: FakeWorker() if name in workers else None
    app = fastapi.FastAPI()
    app.include_router(build_router(registry))
    return TestClient(app)


def test_run_known_worker_returns_202():
    client = make_client({"instrument_loader"})
    response = client.post("/run/instrument_loader")
    assert response.status_code == 202
    assert response.json()["status"] == "accepted"


def test_run_unknown_worker_returns_404():
    client = make_client({"instrument_loader"})
    response = client.post("/run/nonexistent")
    assert response.status_code == 404
```

- [ ] **Step 2: Run test to confirm it fails**

```bash
docker-compose run --rm worker-service pytest tests/unit/worker_service/test_routes.py -v
```
Expected: `ImportError` — routes module not found

- [ ] **Step 3: Implement routes**

```python
# services/worker_service/api/__init__.py
```

```python
# services/worker_service/api/routes.py
from fastapi import APIRouter, BackgroundTasks, HTTPException
from services.worker_service.core.registry import WorkerRegistry


def build_router(registry: WorkerRegistry) -> APIRouter:
    router = APIRouter()

    @router.post("/run/{worker_name}", status_code=202)
    async def run_worker(worker_name: str, background_tasks: BackgroundTasks):
        worker = registry.get(worker_name)
        if worker is None:
            raise HTTPException(status_code=404, detail=f"Worker '{worker_name}' not found.")
        background_tasks.add_task(worker.run)
        return {"status": "accepted", "worker": worker_name}

    return router
```

- [ ] **Step 4: Run tests to confirm they pass**

```bash
docker-compose run --rm worker-service pytest tests/unit/worker_service/test_routes.py -v
```
Expected: 2 PASSED

- [ ] **Step 5: Commit**

```bash
git add services/worker_service/api/ tests/unit/worker_service/test_routes.py
git commit -m "feat(worker-service): add FastAPI POST /run/{worker_name} endpoint"
```

---

### Task 6: Create main entry point

**Files:**
- Create: `services/worker_service/main.py`

- [ ] **Step 1: Create main.py**

```python
# services/worker_service/main.py
import argparse
import uvicorn
import fastapi

from services.worker_service.core.registry import WorkerRegistry
from services.worker_service.core.scheduler import build_scheduler
from services.worker_service.api.routes import build_router


def run_server():
    registry = WorkerRegistry()
    scheduler = build_scheduler(registry)
    scheduler.start()
    print("[main] Scheduler started.")

    app = fastapi.FastAPI(title="Alpha Worker Service")
    app.include_router(build_router(registry))

    print("[main] Starting HTTP server on port 8000...")
    uvicorn.run(app, host="0.0.0.0", port=8000)


def run_cli(worker_name: str):
    registry = WorkerRegistry()
    worker = registry.get(worker_name)
    if worker is None:
        print(f"[main] Unknown worker: '{worker_name}'")
        print(f"[main] Available: {list(registry.all().keys())}")
        raise SystemExit(1)
    print(f"[main] Running worker '{worker_name}' in CLI mode...")
    worker.run()
    print(f"[main] Worker '{worker_name}' completed.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Alpha Worker Service")
    parser.add_argument("--worker", type=str, default=None, help="Run a specific worker and exit")
    args = parser.parse_args()

    if args.worker:
        run_cli(args.worker)
    else:
        run_server()
```

- [ ] **Step 2: Smoke test CLI mode**

```bash
docker-compose run --rm -e TEST_MODE=1 worker-service python main.py --worker instrument_loader
```
Expected: instrument loader runs, prints download/insert progress, exits cleanly.

- [ ] **Step 3: Commit**

```bash
git add services/worker_service/main.py
git commit -m "feat(worker-service): add main entry point with CLI and server mode"
```

---

### Task 7: Create requirements.txt and Dockerfile

**Files:**
- Create: `services/worker_service/requirements.txt`
- Create: `services/worker_service/Dockerfile`

- [ ] **Step 1: Create requirements.txt**

```
# services/worker_service/requirements.txt
fastapi==0.111.0
uvicorn==0.30.1
apscheduler==3.10.4
pyyaml==6.0.1
psycopg2-binary==2.9.9
requests==2.31.0
httpx==0.27.0
pytest==8.2.2
```

- [ ] **Step 2: Create Dockerfile**

```dockerfile
# services/worker_service/Dockerfile
FROM python:3.11-slim

WORKDIR /app

RUN apt-get update && apt-get install -y libpq-dev gcc && rm -rf /var/lib/apt/lists/*

COPY services/worker_service/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy the full repo so imports like services.worker_service.core work
COPY . .

WORKDIR /app/services/worker_service

ENTRYPOINT ["python", "main.py"]
```

- [ ] **Step 3: Build the image**

```bash
docker-compose build worker-service
```
Expected: image builds successfully with no errors.

- [ ] **Step 4: Commit**

```bash
git add services/worker_service/requirements.txt services/worker_service/Dockerfile
git commit -m "feat(worker-service): add Dockerfile and requirements"
```

---

### Task 8: Update docker-compose.yml

**Files:**
- Modify: `docker-compose.yml`

- [ ] **Step 1: Replace instrument-worker service**

Replace the `instrument-worker` block in `docker-compose.yml`:

```yaml
  worker-service:
    build:
      context: .
      dockerfile: services/worker_service/Dockerfile
    image: alpha-worker-service:latest
    container_name: alpha-worker-service
    restart: unless-stopped
    ports:
      - "8001:8000"
    environment:
      - TZ=Asia/Kolkata
      - POSTGRES_HOST=postgres-db
      - POSTGRES_PORT=5432
      - POSTGRES_DB=alpha_db
      - POSTGRES_USER=alpha_user
      - POSTGRES_PASSWORD=alpha_password
      - TEST_MODE=1
    depends_on:
      - postgres-db
```

> **Note:** `restart: unless-stopped` because this is now a long-running server. For one-shot CLI runs, override with: `command: ["python", "main.py", "--worker", "instrument_loader"]` and `restart: "no"`.

- [ ] **Step 2: Build and start**

```bash
docker-compose build worker-service
docker-compose up -d worker-service
```

- [ ] **Step 3: Verify API is reachable**

```bash
curl -X POST http://localhost:8001/run/instrument_loader
```
Expected: `{"status":"accepted","worker":"instrument_loader"}`

- [ ] **Step 4: Check logs show worker running**

```bash
docker logs alpha-worker-service -f
```
Expected: download progress and insertion completion printed in logs.

- [ ] **Step 5: Commit**

```bash
git add docker-compose.yml
git commit -m "feat(worker-service): update docker-compose to run worker-service"
```

---

### Task 9: Delete old instrument_worker service

**Files:**
- Delete: `services/instrument_worker/main.py`
- Delete: `services/instrument_worker/Dockerfile`
- Delete: `services/instrument_worker/requirements.txt`

- [ ] **Step 1: Remove old service directory**

```bash
git rm -r services/instrument_worker/
```

- [ ] **Step 2: Commit**

```bash
git commit -m "chore: remove old instrument_worker service (replaced by worker_service)"
```

---

### Task 10: Run full test suite

- [ ] **Step 1: Run all worker_service unit tests**

```bash
docker-compose run --rm worker-service pytest tests/unit/worker_service/ -v
```
Expected: all tests PASSED.

- [ ] **Step 2: Run CLI smoke test**

```bash
docker-compose run --rm -e TEST_MODE=1 worker-service python main.py --worker instrument_loader
```
Expected: exits 0, instrument data inserted.

- [ ] **Step 3: Run server mode smoke test**

```bash
docker-compose up -d worker-service
sleep 3
curl -s -X POST http://localhost:8001/run/instrument_loader | python3 -m json.tool
docker logs alpha-worker-service --tail 20
```
Expected: 202 response, logs show worker start and finish.

- [ ] **Step 4: Final commit**

```bash
git add .
git commit -m "feat(worker-service): complete worker service refactor with tests"
```
