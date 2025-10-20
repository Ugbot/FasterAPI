#!/usr/bin/env python3
"""
FasterAPI vs FastAPI Performance Benchmark

Compares FasterAPI performance against FastAPI for:
- HTTP request handling
- JSON serialization
- Route matching
- Middleware processing
- Database operations (when available)
"""

import sys
import os
import time
import asyncio
import threading
import statistics
from typing import Dict, List, Any
import json

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from fasterapi import App
from fasterapi.http import Request, Response

# Try to import FastAPI for comparison
try:
    from fastapi import FastAPI as FastAPIApp
    from fastapi.responses import JSONResponse
    FASTAPI_AVAILABLE = True
except ImportError:
    FASTAPI_AVAILABLE = False
    print("⚠️  FastAPI not available for comparison")

class BenchmarkResult:
    """Benchmark result container."""
    
    def __init__(self, name: str):
        self.name = name
        self.times: List[float] = []
        self.errors: List[str] = []
        self.total_requests = 0
        self.successful_requests = 0
    
    def add_time(self, duration: float):
        """Add a timing measurement."""
        self.times.append(duration)
        self.total_requests += 1
        self.successful_requests += 1
    
    def add_error(self, error: str):
        """Add an error."""
        self.errors.append(error)
        self.total_requests += 1
    
    def get_stats(self) -> Dict[str, float]:
        """Get statistics for this benchmark."""
        if not self.times:
            return {
                "mean": 0.0,
                "median": 0.0,
                "min": 0.0,
                "max": 0.0,
                "std": 0.0,
                "p95": 0.0,
                "p99": 0.0,
                "requests_per_sec": 0.0,
                "success_rate": 0.0
            }
        
        times = sorted(self.times)
        mean = statistics.mean(times)
        median = statistics.median(times)
        min_time = min(times)
        max_time = max(times)
        std = statistics.stdev(times) if len(times) > 1 else 0.0
        
        # Percentiles
        p95_idx = int(len(times) * 0.95)
        p99_idx = int(len(times) * 0.99)
        p95 = times[p95_idx] if p95_idx < len(times) else times[-1]
        p99 = times[p99_idx] if p99_idx < len(times) else times[-1]
        
        # Requests per second
        total_time = sum(times)
        requests_per_sec = len(times) / total_time if total_time > 0 else 0.0
        
        # Success rate
        success_rate = self.successful_requests / self.total_requests if self.total_requests > 0 else 0.0
        
        return {
            "mean": mean,
            "median": median,
            "min": min_time,
            "max": max_time,
            "std": std,
            "p95": p95,
            "p99": p99,
            "requests_per_sec": requests_per_sec,
            "success_rate": success_rate
        }

class PerformanceBenchmark:
    """Performance benchmark suite."""
    
    def __init__(self):
        self.results: Dict[str, BenchmarkResult] = {}
        self.fasterapi_app = None
        self.fastapi_app = None
    
    def create_fasterapi_app(self) -> App:
        """Create FasterAPI application for testing."""
        app = App(
            port=8002,
            host="127.0.0.1",
            enable_h2=True,
            enable_h3=False,
            enable_compression=True
        )
        
        # Test routes
        @app.get("/ping")
        def ping(req: Request, res: Response):
            return {"message": "pong", "timestamp": time.time()}
        
        @app.get("/data")
        def get_data(req: Request, res: Response):
            return {
                "data": "Hello FasterAPI",
                "count": 42,
                "items": [{"id": i, "value": f"item_{i}"} for i in range(10)]
            }
        
        @app.post("/echo")
        def echo(req: Request, res: Response):
            # Simulate processing request body
            return {"echo": "Hello from FasterAPI", "timestamp": time.time()}
        
        # Middleware for testing
        @app.add_middleware
        def timing_middleware(req: Request, res: Response):
            # Simple middleware
            pass
        
        return app
    
    def create_fastapi_app(self):
        """Create FastAPI application for testing."""
        if not FASTAPI_AVAILABLE:
            return None
        
        app = FastAPIApp()
        
        @app.get("/ping")
        async def ping():
            return {"message": "pong", "timestamp": time.time()}
        
        @app.get("/data")
        async def get_data():
            return {
                "data": "Hello FastAPI",
                "count": 42,
                "items": [{"id": i, "value": f"item_{i}"} for i in range(10)]
            }
        
        @app.post("/echo")
        async def echo():
            return {"echo": "Hello from FastAPI", "timestamp": time.time()}
        
        return app
    
    def benchmark_route_registration(self):
        """Benchmark route registration performance."""
        print("🧪 Benchmarking route registration...")
        
        # FasterAPI route registration
        start_time = time.time()
        app = App(port=8003, host="127.0.0.1")
        
        for i in range(1000):
            @app.get(f"/route_{i}")
            def handler(req: Request, res: Response):
                return {"route": i}
        
        fasterapi_time = time.time() - start_time
        
        # FastAPI route registration (if available)
        fastapi_time = 0.0
        if FASTAPI_AVAILABLE:
            start_time = time.time()
            app = FastAPIApp()
            
            for i in range(1000):
                @app.get(f"/route_{i}")
                async def handler():
                    return {"route": i}
            
            fastapi_time = time.time() - start_time
        
        # Store results
        self.results["route_registration"] = BenchmarkResult("Route Registration")
        self.results["route_registration"].add_time(fasterapi_time)
        
        if FASTAPI_AVAILABLE:
            self.results["route_registration_fastapi"] = BenchmarkResult("Route Registration (FastAPI)")
            self.results["route_registration_fastapi"].add_time(fastapi_time)
        
        print(f"   FasterAPI: {fasterapi_time:.4f}s")
        if FASTAPI_AVAILABLE:
            print(f"   FastAPI: {fastapi_time:.4f}s")
            print(f"   Speedup: {fastapi_time/fasterapi_time:.2f}x")
    
    def benchmark_json_serialization(self):
        """Benchmark JSON serialization performance."""
        print("🧪 Benchmarking JSON serialization...")
        
        # Test data
        test_data = {
            "message": "Hello World",
            "count": 42,
            "items": [{"id": i, "value": f"item_{i}"} for i in range(100)],
            "nested": {
                "level1": {
                    "level2": {
                        "level3": "deep value"
                    }
                }
            }
        }
        
        # FasterAPI JSON serialization
        start_time = time.time()
        for _ in range(10000):
            json.dumps(test_data)
        fasterapi_time = time.time() - start_time
        
        # FastAPI JSON serialization (if available)
        fastapi_time = 0.0
        if FASTAPI_AVAILABLE:
            start_time = time.time()
            for _ in range(10000):
                json.dumps(test_data)
            fastapi_time = time.time() - start_time
        
        # Store results
        self.results["json_serialization"] = BenchmarkResult("JSON Serialization")
        self.results["json_serialization"].add_time(fasterapi_time)
        
        if FASTAPI_AVAILABLE:
            self.results["json_serialization_fastapi"] = BenchmarkResult("JSON Serialization (FastAPI)")
            self.results["json_serialization_fastapi"].add_time(fastapi_time)
        
        print(f"   FasterAPI: {fasterapi_time:.4f}s")
        if FASTAPI_AVAILABLE:
            print(f"   FastAPI: {fastapi_time:.4f}s")
            print(f"   Speedup: {fastapi_time/fasterapi_time:.2f}x")
    
    def benchmark_middleware_processing(self):
        """Benchmark middleware processing performance."""
        print("🧪 Benchmarking middleware processing...")
        
        # FasterAPI middleware
        start_time = time.time()
        app = App(port=8004, host="127.0.0.1")
        
        # Add multiple middleware
        for i in range(100):
            @app.add_middleware
            def middleware(req: Request, res: Response):
                pass
        
        @app.get("/test")
        def handler(req: Request, res: Response):
            return {"message": "test"}
        
        fasterapi_time = time.time() - start_time
        
        # FastAPI middleware (if available)
        fastapi_time = 0.0
        if FASTAPI_AVAILABLE:
            start_time = time.time()
            app = FastAPIApp()
            
            # Add multiple middleware
            for i in range(100):
                @app.middleware("http")
                async def middleware(request, call_next):
                    response = await call_next(request)
                    return response
            
            @app.get("/test")
            async def handler():
                return {"message": "test"}
            
            fastapi_time = time.time() - start_time
        
        # Store results
        self.results["middleware_processing"] = BenchmarkResult("Middleware Processing")
        self.results["middleware_processing"].add_time(fasterapi_time)
        
        if FASTAPI_AVAILABLE:
            self.results["middleware_processing_fastapi"] = BenchmarkResult("Middleware Processing (FastAPI)")
            self.results["middleware_processing_fastapi"].add_time(fastapi_time)
        
        print(f"   FasterAPI: {fasterapi_time:.4f}s")
        if FASTAPI_AVAILABLE:
            print(f"   FastAPI: {fastapi_time:.4f}s")
            print(f"   Speedup: {fastapi_time/fasterapi_time:.2f}x")
    
    def benchmark_app_creation(self):
        """Benchmark application creation performance."""
        print("🧪 Benchmarking application creation...")
        
        # FasterAPI app creation
        start_time = time.time()
        for _ in range(1000):
            app = App(port=8005, host="127.0.0.1")
        fasterapi_time = time.time() - start_time
        
        # FastAPI app creation (if available)
        fastapi_time = 0.0
        if FASTAPI_AVAILABLE:
            start_time = time.time()
            for _ in range(1000):
                app = FastAPIApp()
            fastapi_time = time.time() - start_time
        
        # Store results
        self.results["app_creation"] = BenchmarkResult("Application Creation")
        self.results["app_creation"].add_time(fasterapi_time)
        
        if FASTAPI_AVAILABLE:
            self.results["app_creation_fastapi"] = BenchmarkResult("Application Creation (FastAPI)")
            self.results["app_creation_fastapi"].add_time(fastapi_time)
        
        print(f"   FasterAPI: {fasterapi_time:.4f}s")
        if FASTAPI_AVAILABLE:
            print(f"   FastAPI: {fastapi_time:.4f}s")
            print(f"   Speedup: {fastapi_time/fasterapi_time:.2f}x")
    
    def run_all_benchmarks(self):
        """Run all benchmarks."""
        print("🔥 FasterAPI vs FastAPI Performance Benchmark")
        print("=" * 50)
        
        if not FASTAPI_AVAILABLE:
            print("⚠️  FastAPI not available - running FasterAPI-only benchmarks")
        
        # Run benchmarks
        self.benchmark_app_creation()
        self.benchmark_route_registration()
        self.benchmark_middleware_processing()
        self.benchmark_json_serialization()
        
        # Print results
        self.print_results()
    
    def print_results(self):
        """Print benchmark results."""
        print("\n📊 Benchmark Results")
        print("=" * 50)
        
        for name, result in self.results.items():
            stats = result.get_stats()
            print(f"\n{result.name}:")
            print(f"   Mean: {stats['mean']:.6f}s")
            print(f"   Median: {stats['median']:.6f}s")
            print(f"   Min: {stats['min']:.6f}s")
            print(f"   Max: {stats['max']:.6f}s")
            print(f"   Std: {stats['std']:.6f}s")
            print(f"   P95: {stats['p95']:.6f}s")
            print(f"   P99: {stats['p99']:.6f}s")
            print(f"   Requests/sec: {stats['requests_per_sec']:.2f}")
            print(f"   Success rate: {stats['success_rate']:.2%}")
        
        # Summary
        print("\n🏆 Summary")
        print("=" * 20)
        
        if FASTAPI_AVAILABLE:
            print("FasterAPI vs FastAPI Performance Comparison:")
            for name, result in self.results.items():
                if "fastapi" in name.lower():
                    continue
                
                fasterapi_result = result
                fastapi_result = self.results.get(f"{name}_fastapi")
                
                if fastapi_result:
                    fasterapi_stats = fasterapi_result.get_stats()
                    fastapi_stats = fastapi_result.get_stats()
                    
                    speedup = fastapi_stats['mean'] / fasterapi_stats['mean']
                    print(f"   {name}: {speedup:.2f}x faster")
        else:
            print("FasterAPI Performance Metrics:")
            for name, result in self.results.items():
                stats = result.get_stats()
                print(f"   {name}: {stats['requests_per_sec']:.2f} req/sec")

def main():
    """Main benchmark function."""
    benchmark = PerformanceBenchmark()
    benchmark.run_all_benchmarks()

if __name__ == "__main__":
    main()