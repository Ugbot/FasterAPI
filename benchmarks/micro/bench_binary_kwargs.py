#!/usr/bin/env python3
"""
Micro-benchmark for binary kwargs serialization.

Compares performance of:
- JSON serialization (baseline)
- Binary TLV decoding (Cython)
- Binary TLV decoding (pure Python fallback)

Target: ~26x improvement over JSON for simple kwargs.
"""

import json
import struct
import time
import statistics
from typing import Callable, Dict, List, Any

# Try to import the Cython binary decoder
try:
    from fasterapi.core.binary_kwargs import decode_kwargs as decode_binary_cython
    from fasterapi.core.binary_kwargs import decode_kwargs_pure_python
    from fasterapi.core.binary_kwargs import is_binary_format
    HAS_CYTHON = True
except ImportError:
    HAS_CYTHON = False
    print("WARNING: Cython binary_kwargs module not available")
    print("Build with: python setup.py build_ext --inplace")

# Binary TLV format constants
BINARY_KWARGS_MAGIC = 0xFA

# Type tags
TAG_NULL = 0x00
TAG_BOOL_FALSE = 0x01
TAG_BOOL_TRUE = 0x02
TAG_INT8 = 0x10
TAG_INT16 = 0x11
TAG_INT32 = 0x12
TAG_INT64 = 0x13
TAG_UINT8 = 0x18
TAG_UINT16 = 0x19
TAG_UINT32 = 0x1A
TAG_UINT64 = 0x1B
TAG_FLOAT32 = 0x20
TAG_FLOAT64 = 0x21
TAG_STR_TINY = 0x30
TAG_STR_SHORT = 0x31
TAG_STR_MEDIUM = 0x32


def encode_binary_kwargs(kwargs: Dict[str, Any]) -> bytes:
    """
    Encode kwargs to binary TLV format (Python reference implementation).
    Used for benchmark data generation.
    """
    parts = [
        struct.pack('<B', BINARY_KWARGS_MAGIC),  # Magic byte
        struct.pack('<H', len(kwargs)),  # Param count
    ]

    for name, value in kwargs.items():
        name_bytes = name.encode('utf-8')
        parts.append(struct.pack('<B', len(name_bytes)))  # Name length
        parts.append(name_bytes)  # Name

        if value is None:
            parts.append(struct.pack('<B', TAG_NULL))
        elif isinstance(value, bool):
            parts.append(struct.pack('<B', TAG_BOOL_TRUE if value else TAG_BOOL_FALSE))
        elif isinstance(value, int):
            if -128 <= value <= 127:
                parts.append(struct.pack('<Bb', TAG_INT8, value))
            elif -32768 <= value <= 32767:
                parts.append(struct.pack('<Bh', TAG_INT16, value))
            elif -2147483648 <= value <= 2147483647:
                parts.append(struct.pack('<Bi', TAG_INT32, value))
            else:
                parts.append(struct.pack('<Bq', TAG_INT64, value))
        elif isinstance(value, float):
            parts.append(struct.pack('<Bd', TAG_FLOAT64, value))
        elif isinstance(value, str):
            str_bytes = value.encode('utf-8')
            if len(str_bytes) <= 255:
                parts.append(struct.pack('<BB', TAG_STR_TINY, len(str_bytes)))
                parts.append(str_bytes)
            elif len(str_bytes) <= 65535:
                parts.append(struct.pack('<BH', TAG_STR_SHORT, len(str_bytes)))
                parts.append(str_bytes)
            else:
                parts.append(struct.pack('<BI', TAG_STR_MEDIUM, len(str_bytes)))
                parts.append(str_bytes)
        else:
            raise ValueError(f"Unsupported type: {type(value)}")

    return b''.join(parts)


def benchmark_function(func: Callable, data: Any, iterations: int = 10000) -> Dict[str, float]:
    """Run a benchmark and return statistics."""
    times = []

    # Warmup
    for _ in range(min(1000, iterations // 10)):
        func(data)

    # Timed runs
    for _ in range(iterations):
        start = time.perf_counter_ns()
        result = func(data)
        end = time.perf_counter_ns()
        times.append(end - start)

    return {
        'mean_ns': statistics.mean(times),
        'median_ns': statistics.median(times),
        'stdev_ns': statistics.stdev(times) if len(times) > 1 else 0,
        'min_ns': min(times),
        'max_ns': max(times),
        'p95_ns': sorted(times)[int(len(times) * 0.95)],
        'p99_ns': sorted(times)[int(len(times) * 0.99)],
    }


def run_benchmarks():
    """Run all benchmarks and print results."""
    print("=" * 70)
    print("Binary Kwargs Serialization Micro-Benchmark")
    print("=" * 70)
    print()

    # Test cases with varying complexity
    test_cases = [
        {
            'name': 'Simple (5 params)',
            'kwargs': {
                'user_id': 12345,
                'name': 'Alice',
                'age': 30,
                'active': True,
                'score': 98.5,
            }
        },
        {
            'name': 'Minimal (1 param)',
            'kwargs': {
                'id': 1,
            }
        },
        {
            'name': 'String-heavy (5 params)',
            'kwargs': {
                'first_name': 'Christopher',
                'last_name': 'Wellington',
                'email': 'christopher.wellington@example.com',
                'department': 'Engineering',
                'title': 'Senior Software Engineer',
            }
        },
        {
            'name': 'Integer-heavy (10 params)',
            'kwargs': {
                'a': 1, 'b': 2, 'c': 3, 'd': 4, 'e': 5,
                'f': 6, 'g': 7, 'h': 8, 'i': 9, 'j': 10,
            }
        },
        {
            'name': 'Mixed types (8 params)',
            'kwargs': {
                'count': 42,
                'ratio': 3.14159,
                'enabled': True,
                'label': 'test-label',
                'nothing': None,
                'big_int': 9223372036854775807,
                'negative': -12345,
                'small': 7,
            }
        },
    ]

    iterations = 50000

    for case in test_cases:
        print(f"\n--- {case['name']} ---")
        kwargs = case['kwargs']

        # Prepare data
        json_data = json.dumps(kwargs).encode('utf-8')
        binary_data = encode_binary_kwargs(kwargs)

        print(f"JSON size:   {len(json_data):5d} bytes")
        print(f"Binary size: {len(binary_data):5d} bytes ({100 * len(binary_data) / len(json_data):.1f}% of JSON)")
        print()

        # Benchmark JSON decode
        json_stats = benchmark_function(
            lambda d: json.loads(d.decode('utf-8')),
            json_data,
            iterations
        )
        print(f"JSON decode:        {json_stats['mean_ns']:8.0f} ns (median: {json_stats['median_ns']:.0f} ns, p99: {json_stats['p99_ns']:.0f} ns)")

        if HAS_CYTHON:
            # Benchmark Cython binary decode
            cython_stats = benchmark_function(
                decode_binary_cython,
                binary_data,
                iterations
            )
            speedup_cython = json_stats['mean_ns'] / cython_stats['mean_ns']
            print(f"Binary (Cython):    {cython_stats['mean_ns']:8.0f} ns (median: {cython_stats['median_ns']:.0f} ns, p99: {cython_stats['p99_ns']:.0f} ns) [{speedup_cython:.1f}x faster]")

            # Benchmark pure Python binary decode
            python_stats = benchmark_function(
                decode_kwargs_pure_python,
                binary_data,
                iterations
            )
            speedup_python = json_stats['mean_ns'] / python_stats['mean_ns']
            print(f"Binary (pure Py):   {python_stats['mean_ns']:8.0f} ns (median: {python_stats['median_ns']:.0f} ns, p99: {python_stats['p99_ns']:.0f} ns) [{speedup_python:.1f}x faster]")

            # Verify correctness
            decoded_json = json.loads(json_data.decode('utf-8'))
            decoded_cython = decode_binary_cython(binary_data)
            decoded_python = decode_kwargs_pure_python(binary_data)

            assert decoded_cython == decoded_json, f"Cython decode mismatch: {decoded_cython} != {decoded_json}"
            assert decoded_python == decoded_json, f"Python decode mismatch: {decoded_python} != {decoded_json}"
        else:
            print("  (Cython decoder not available)")

    print("\n" + "=" * 70)
    if HAS_CYTHON:
        print("Benchmark complete. Target: ~26x improvement over JSON.")
    else:
        print("Build Cython extension for accurate benchmarks:")
        print("  python setup.py build_ext --inplace")
    print("=" * 70)


if __name__ == '__main__':
    run_benchmarks()
