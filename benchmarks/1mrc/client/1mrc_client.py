"""
Test client for 1 Million Request Challenge (1MRC)

Sends 1,000,000 concurrent requests to test FasterAPI performance.
"""

import asyncio
import aiohttp
import time
import random
import json
from typing import Dict, List
from dataclasses import dataclass
import sys


@dataclass
class TestConfig:
    """Test configuration."""
    total_requests: int = 1_000_000
    concurrent_workers: int = 1000
    batch_size: int = 100
    server_url: str = "http://localhost:8000"
    progress_interval: int = 100_000


@dataclass
class TestResults:
    """Test results."""
    total_time: float
    requests_per_second: float
    errors: int
    stats: Dict


class MRCClient:
    """Client for 1 Million Request Challenge."""
    
    def __init__(self, config: TestConfig):
        self.config = config
        self.completed = 0
        self.errors = 0
        self.start_time = 0.0
        self.lock = asyncio.Lock()
    
    async def send_event(self, session: aiohttp.ClientSession, user_id: str, value: float) -> bool:
        """
        Send a single event to the server.
        
        Args:
            session: aiohttp session
            user_id: User identifier
            value: Event value
            
        Returns:
            True if successful, False otherwise
        """
        try:
            async with session.post(
                f"{self.config.server_url}/event",
                json={"userId": user_id, "value": value},
                timeout=aiohttp.ClientTimeout(total=10)
            ) as response:
                return response.status == 201
        except Exception as e:
            async with self.lock:
                self.errors += 1
            return False
    
    async def worker(self, session: aiohttp.ClientSession, worker_id: int, requests_per_worker: int):
        """
        Worker coroutine that sends multiple requests.
        
        Args:
            session: aiohttp session
            worker_id: Worker identifier
            requests_per_worker: Number of requests this worker should send
        """
        for i in range(requests_per_worker):
            # Generate deterministic user_id for consistent unique user count
            # Using modulo to ensure we get ~75k unique users out of 1M requests
            user_id = f"user_{(worker_id * requests_per_worker + i) % 75000}"
            
            # Generate deterministic value (0-999) for consistent sum
            value = (worker_id * requests_per_worker + i) % 1000
            
            # Send the event
            await self.send_event(session, user_id, value)
            
            # Update progress
            async with self.lock:
                self.completed += 1
                
                # Print progress every N requests
                if self.completed % self.config.progress_interval == 0:
                    elapsed = time.time() - self.start_time
                    rps = self.completed / elapsed if elapsed > 0 else 0
                    print(f"Completed: {self.completed:,}/{self.config.total_requests:,} ({rps:,.1f} req/s)")
    
    async def get_stats(self) -> Dict:
        """
        Get statistics from the server.
        
        Returns:
            Statistics dictionary
        """
        async with aiohttp.ClientSession() as session:
            async with session.get(f"{self.config.server_url}/stats") as response:
                if response.status == 200:
                    return await response.json()
                else:
                    return {}
    
    async def reset_server(self):
        """Reset server statistics before test."""
        async with aiohttp.ClientSession() as session:
            async with session.post(f"{self.config.server_url}/reset") as response:
                if response.status == 200:
                    print("✅ Server statistics reset")
                else:
                    print("⚠️  Could not reset server statistics")
    
    async def run_test(self) -> TestResults:
        """
        Run the 1 million request test.
        
        Returns:
            Test results
        """
        print("=" * 60)
        print("FasterAPI - 1 Million Request Challenge (1MRC)")
        print("=" * 60)
        print(f"Total requests:      {self.config.total_requests:,}")
        print(f"Concurrent workers:  {self.config.concurrent_workers:,}")
        print(f"Server URL:          {self.config.server_url}")
        print("=" * 60)
        print()
        
        # Reset server statistics
        await self.reset_server()
        
        # Calculate requests per worker
        requests_per_worker = self.config.total_requests // self.config.concurrent_workers
        
        # Create connector with optimized settings
        connector = aiohttp.TCPConnector(
            limit=self.config.concurrent_workers,
            limit_per_host=self.config.concurrent_workers,
            ttl_dns_cache=300,
            force_close=False,
            enable_cleanup_closed=True
        )
        
        # Create session with keep-alive
        timeout = aiohttp.ClientTimeout(total=3600)  # 1 hour timeout
        async with aiohttp.ClientSession(
            connector=connector,
            timeout=timeout,
            headers={"Connection": "keep-alive"}
        ) as session:
            # Start timer
            self.start_time = time.time()
            print(f"🚀 Starting test at {time.strftime('%H:%M:%S')}")
            print()
            
            # Create worker tasks
            tasks = [
                self.worker(session, worker_id, requests_per_worker)
                for worker_id in range(self.config.concurrent_workers)
            ]
            
            # Run all workers concurrently
            await asyncio.gather(*tasks)
            
            # Calculate results
            total_time = time.time() - self.start_time
            rps = self.config.total_requests / total_time
        
        print()
        print("=" * 60)
        print("Test Results")
        print("=" * 60)
        print(f"Total time:          {total_time:.3f}s")
        print(f"Requests per second: {rps:,.2f}")
        print(f"Errors:              {self.errors:,}")
        print()
        
        # Get final statistics from server
        print("Fetching server statistics...")
        stats = await self.get_stats()
        
        print()
        print("=" * 60)
        print("Server Statistics")
        print("=" * 60)
        print(f"Total Requests:      {stats.get('totalRequests', 0):,}")
        print(f"Unique Users:        {stats.get('uniqueUsers', 0):,}")
        print(f"Sum:                 {stats.get('sum', 0):,.2f}")
        print(f"Average:             {stats.get('avg', 0):,.2f}")
        print()
        
        # Validate results
        expected_requests = self.config.total_requests
        actual_requests = stats.get('totalRequests', 0)
        
        if actual_requests == expected_requests:
            print("✅ SUCCESS: All requests processed correctly!")
        else:
            print(f"❌ FAILED: Expected {expected_requests:,} requests, got {actual_requests:,}")
            print(f"   Lost: {expected_requests - actual_requests:,} requests")
        
        print("=" * 60)
        
        return TestResults(
            total_time=total_time,
            requests_per_second=rps,
            errors=self.errors,
            stats=stats
        )


async def main():
    """Main entry point."""
    # Parse command line arguments
    config = TestConfig()
    
    if len(sys.argv) > 1:
        config.total_requests = int(sys.argv[1])
    if len(sys.argv) > 2:
        config.concurrent_workers = int(sys.argv[2])
    
    # Create and run client
    client = MRCClient(config)
    results = await client.run_test()
    
    # Generate log file
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    log_file = f"logs/1mrc_fasterapi_{timestamp}.txt"
    
    try:
        import os
        os.makedirs("logs", exist_ok=True)
        
        with open(log_file, "w") as f:
            f.write("=" * 60 + "\n")
            f.write("FasterAPI - 1 Million Request Challenge (1MRC)\n")
            f.write("=" * 60 + "\n")
            f.write(f"Timestamp: {timestamp}\n")
            f.write(f"Total requests: {config.total_requests:,}\n")
            f.write(f"Concurrent workers: {config.concurrent_workers:,}\n")
            f.write(f"\n")
            f.write("Results:\n")
            f.write(f"  Total time: {results.total_time:.3f}s\n")
            f.write(f"  Requests per second: {results.requests_per_second:,.2f}\n")
            f.write(f"  Errors: {results.errors:,}\n")
            f.write(f"\n")
            f.write("Server Statistics:\n")
            f.write(f"  Total Requests: {results.stats.get('totalRequests', 0):,}\n")
            f.write(f"  Unique Users: {results.stats.get('uniqueUsers', 0):,}\n")
            f.write(f"  Sum: {results.stats.get('sum', 0):,.2f}\n")
            f.write(f"  Average: {results.stats.get('avg', 0):,.2f}\n")
        
        print(f"\n📝 Results logged to: {log_file}")
    except Exception as e:
        print(f"⚠️  Could not write log file: {e}")


if __name__ == "__main__":
    # Run the async main function
    asyncio.run(main())

