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
