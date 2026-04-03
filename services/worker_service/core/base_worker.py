from abc import ABC, abstractmethod
from typing import Optional, Any


class BaseWorker(ABC):
    @abstractmethod
    def run(self, params: Optional[dict[str, Any]] = None) -> None:
        """Execute the worker logic. Called by scheduler, API, or CLI."""
        ...
