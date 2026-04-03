import fastapi
import pytest
from fastapi.testclient import TestClient
from unittest.mock import MagicMock

from services.worker_service.api.routes import build_router
from services.worker_service.core.base_worker import BaseWorker


class FakeWorker(BaseWorker):
    def run(self):
        pass


def make_client(worker_names: set):
    registry = MagicMock()
    registry.get.side_effect = lambda name: FakeWorker() if name in worker_names else None
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
