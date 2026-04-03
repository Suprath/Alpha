import os
import yaml
from apscheduler.schedulers.background import BackgroundScheduler

from services.worker_service.core.registry import WorkerRegistry

_CONFIG_PATH = os.path.join(os.path.dirname(__file__), "..", "workers.yaml")


def _run_worker(registry: WorkerRegistry, name: str) -> None:
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
            continue  # on-call only — not auto-scheduled
        parts = schedule.split()
        if len(parts) != 5:
            print(f"[scheduler] Invalid cron expression for '{name}': {schedule}")
            continue
        minute, hour, day, month, day_of_week = parts
        scheduler.add_job(
            _run_worker,
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
