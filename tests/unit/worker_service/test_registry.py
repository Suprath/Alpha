import pytest
from unittest.mock import MagicMock
from services.worker_service.core.registry import WorkerRegistry
from services.worker_service.core.base_worker import BaseWorker


def test_registry_returns_empty_when_no_workers(tmp_path):
    registry = WorkerRegistry(workers_dir=str(tmp_path))
    assert registry.all() == {}


def test_registry_get_unknown_worker_returns_none(tmp_path):
    registry = WorkerRegistry(workers_dir=str(tmp_path))
    assert registry.get("nonexistent") is None


def test_registry_discovers_valid_worker(tmp_path):
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
