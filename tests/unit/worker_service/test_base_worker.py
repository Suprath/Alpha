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
