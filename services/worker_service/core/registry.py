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
