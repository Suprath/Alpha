from abc import ABC, abstractmethod


class BaseWorker(ABC):
    @abstractmethod
    def run(self) -> None:
        """Execute the worker logic. Called by scheduler, API, or CLI."""
        ...
