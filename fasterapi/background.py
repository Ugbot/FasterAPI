"""
Background tasks for FastAPI compatibility.

Provides BackgroundTasks for running tasks after the response is sent.
"""

import asyncio
from typing import Any, Callable, Coroutine, List, Tuple, Union


class BackgroundTask:
    """A single background task."""

    def __init__(
        self,
        func: Callable[..., Any],
        *args: Any,
        **kwargs: Any,
    ) -> None:
        self.func = func
        self.args = args
        self.kwargs = kwargs

    async def __call__(self) -> None:
        """Execute the task."""
        if asyncio.iscoroutinefunction(self.func):
            await self.func(*self.args, **self.kwargs)
        else:
            # Run sync function in thread pool
            loop = asyncio.get_event_loop()
            await loop.run_in_executor(None, self._run_sync)

    def _run_sync(self) -> None:
        """Run synchronous function."""
        self.func(*self.args, **self.kwargs)


class BackgroundTasks:
    """
    Collection of background tasks to run after response is sent.

    Usage:
        @app.post("/send-notification")
        async def send_notification(
            background_tasks: BackgroundTasks,
            email: str,
        ):
            background_tasks.add_task(send_email, email, "Hello!")
            return {"message": "Notification scheduled"}

    Tasks can be sync or async functions:
        def sync_task(message: str):
            print(f"Sync: {message}")

        async def async_task(message: str):
            await asyncio.sleep(1)
            print(f"Async: {message}")

        background_tasks.add_task(sync_task, "hello")
        background_tasks.add_task(async_task, "world")
    """

    def __init__(self) -> None:
        self.tasks: List[BackgroundTask] = []

    def add_task(
        self,
        func: Callable[..., Any],
        *args: Any,
        **kwargs: Any,
    ) -> None:
        """
        Add a task to be run in the background.

        Args:
            func: The function to call (sync or async)
            *args: Positional arguments to pass to the function
            **kwargs: Keyword arguments to pass to the function
        """
        task = BackgroundTask(func, *args, **kwargs)
        self.tasks.append(task)

    async def __call__(self) -> None:
        """Execute all tasks."""
        for task in self.tasks:
            try:
                await task()
            except Exception as e:
                # Log error but continue with remaining tasks
                # In production, this should use proper logging
                import sys

                print(f"Background task error: {e}", file=sys.stderr)

    def __len__(self) -> int:
        return len(self.tasks)

    def __bool__(self) -> bool:
        return bool(self.tasks)


async def run_background_tasks(tasks: BackgroundTasks) -> None:
    """
    Helper function to run background tasks.

    This is typically called by the response handler after sending the response.
    """
    if tasks:
        await tasks()


__all__ = [
    "BackgroundTask",
    "BackgroundTasks",
    "run_background_tasks",
]
