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


def run_cli(worker_name: str, date: str = None):
    registry = WorkerRegistry()
    worker = registry.get(worker_name)
    if worker is None:
        print(f"[main] Unknown worker: '{worker_name}'")
        print(f"[main] Available: {list(registry.all().keys())}")
        raise SystemExit(1)
    
    params = {"date": date} if date else None
    print(f"[main] Running worker '{worker_name}' in CLI mode... (Params: {params})")
    worker.run(params)
    print(f"[main] Worker '{worker_name}' completed.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Alpha Worker Service")
    parser.add_argument("--worker", type=str, default=None, help="Run a specific worker and exit")
    parser.add_argument("--date", type=str, default=None, help="Specific date for the worker (YYYY-MM-DD)")
    args = parser.parse_args()

    if args.worker:
        run_cli(args.worker, args.date)
    else:
        run_server()
