"""Asyncio-based worker process for executing Python handlers.

Each worker process runs an asyncio event loop and processes requests from
the shared memory queue. Supports both sync and async handlers.
"""

import asyncio
import sys
import os
import importlib
import inspect
import traceback
import logging
from typing import Dict, Callable, Any
from .shared_memory_protocol import SharedMemoryIPC


# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='[Worker %(process)d] %(levelname)s: %(message)s'
)
logger = logging.getLogger(__name__)


class WorkerProcess:
    """Asyncio-based worker that executes Python handlers from shared memory queue."""

    def __init__(self, shm_name: str, worker_id: int):
        """Initialize worker.

        Args:
            shm_name: Name of shared memory region
            worker_id: Unique ID for this worker (for logging)
        """
        self.shm_name = shm_name
        self.worker_id = worker_id
        self.ipc = None
        self.module_cache: Dict[str, Any] = {}  # Cache imported modules
        self.function_cache: Dict[tuple, Callable] = {}  # Cache (module, function) -> callable
        self.stats = {
            'requests_processed': 0,
            'requests_failed': 0,
            'handlers_cached': 0
        }

    async def run(self):
        """Main worker loop. Reads requests and executes handlers."""
        logger.info(f"Worker {self.worker_id} starting with shared memory: {self.shm_name}")

        # Connect to shared memory
        try:
            self.ipc = SharedMemoryIPC(self.shm_name)
        except Exception as e:
            logger.error(f"Failed to connect to shared memory: {e}")
            return

        logger.info(f"Worker {self.worker_id} connected to shared memory successfully")

        # Main request processing loop
        while not self.ipc.shutdown:
            try:
                # Read request from queue (blocking)
                request_data = await asyncio.to_thread(self.ipc.read_request)

                if request_data is None:
                    # Shutdown signal
                    logger.info(f"Worker {self.worker_id} received shutdown signal")
                    break

                request_id, module_name, function_name, kwargs = request_data
                logger.debug(f"Processing request {request_id}: {module_name}.{function_name}")

                # Execute handler
                await self.process_request(request_id, module_name, function_name, kwargs)

            except Exception as e:
                logger.error(f"Error in main loop: {e}")
                logger.error(traceback.format_exc())

        # Cleanup
        logger.info(f"Worker {self.worker_id} shutting down. Stats: {self.stats}")
        if self.ipc:
            self.ipc.close()

    async def process_request(self, request_id: int, module_name: str,
                             function_name: str, kwargs: dict):
        """Process a single request by executing the handler.

        Args:
            request_id: Unique request ID
            module_name: Python module containing the handler
            function_name: Name of the handler function
            kwargs: Arguments to pass to the handler
        """
        try:
            # Get handler function
            handler = self.get_handler(module_name, function_name)

            # Execute handler (sync or async)
            if inspect.iscoroutinefunction(handler):
                result = await handler(**kwargs)
            else:
                # Run sync handler in thread pool to avoid blocking
                result = await asyncio.to_thread(handler, **kwargs)

            # Write success response
            await asyncio.to_thread(
                self.ipc.write_response,
                request_id,
                200,  # HTTP 200 OK
                True,  # success
                result
            )

            self.stats['requests_processed'] += 1

        except Exception as e:
            # Handle errors
            error_msg = f"{type(e).__name__}: {str(e)}"
            error_trace = traceback.format_exc()
            logger.error(f"Handler error for request {request_id}: {error_msg}")
            logger.debug(error_trace)

            # Write error response
            await asyncio.to_thread(
                self.ipc.write_response,
                request_id,
                500,  # HTTP 500 Internal Server Error
                False,  # error
                {"error": error_msg, "traceback": error_trace},
                error_msg
            )

            self.stats['requests_failed'] += 1

    def get_handler(self, module_name: str, function_name: str) -> Callable:
        """Get handler function, using cache if available.

        Args:
            module_name: Python module containing the handler
            function_name: Name of the handler function

        Returns:
            Callable handler function

        Raises:
            ImportError: If module cannot be imported
            AttributeError: If function not found in module
        """
        cache_key = (module_name, function_name)

        # Check cache first
        if cache_key in self.function_cache:
            return self.function_cache[cache_key]

        # Import module (with caching)
        if module_name in self.module_cache:
            module = self.module_cache[module_name]
        else:
            logger.debug(f"Importing module: {module_name}")
            module = importlib.import_module(module_name)
            self.module_cache[module_name] = module

        # Get function from module
        if not hasattr(module, function_name):
            raise AttributeError(f"Module '{module_name}' has no function '{function_name}'")

        handler = getattr(module, function_name)

        # Cache it
        self.function_cache[cache_key] = handler
        self.stats['handlers_cached'] += 1

        logger.debug(f"Cached handler: {module_name}.{function_name}")

        return handler


async def worker_main(shm_name: str, worker_id: int):
    """Entry point for worker process.

    Args:
        shm_name: Shared memory region name
        worker_id: Worker ID
    """
    worker = WorkerProcess(shm_name, worker_id)
    await worker.run()


def start_worker(shm_name: str, worker_id: int):
    """Start a worker process (synchronous entry point).

    Args:
        shm_name: Shared memory region name
        worker_id: Worker ID
    """
    # Run asyncio event loop
    asyncio.run(worker_main(shm_name, worker_id))


if __name__ == "__main__":
    # Entry point when launched directly
    if len(sys.argv) < 3:
        print("Usage: python -m fasterapi.core.worker_pool <shm_name> <worker_id>")
        sys.exit(1)

    shm_name = sys.argv[1]
    worker_id = int(sys.argv[2])

    # Add project directory to path if specified
    project_dir = os.environ.get('FASTERAPI_PROJECT_DIR')
    if project_dir and project_dir not in sys.path:
        sys.path.insert(0, project_dir)

    logger.info(f"Starting worker {worker_id} with shared memory: {shm_name}")
    logger.info(f"Python path: {sys.path[:3]}")  # Log first 3 entries

    try:
        start_worker(shm_name, worker_id)
    except KeyboardInterrupt:
        logger.info(f"Worker {worker_id} interrupted")
    except Exception as e:
        logger.error(f"Worker {worker_id} failed: {e}")
        logger.error(traceback.format_exc())
        sys.exit(1)
