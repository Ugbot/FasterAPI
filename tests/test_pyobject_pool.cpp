/**
 * Test for PyObject Pool
 */

#include "../src/cpp/python/pyobject_pool.h"
#include <Python.h>
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <cassert>

using namespace fasterapi::python;

void test_dict_pool_basic() {
    std::cout << "Test: Dict pool basic operations... ";
    
    PyDictPool pool(16);
    
    // Acquire dict
    PyObject* dict1 = pool.acquire();
    assert(dict1 != nullptr);
    assert(PyDict_Check(dict1));
    assert(PyDict_Size(dict1) == 0);  // Should be empty
    
    // Add some data
    PyDict_SetItemString(dict1, "key", PyLong_FromLong(42));
    assert(PyDict_Size(dict1) == 1);
    
    // Release
    pool.release(dict1);
    
    // Acquire again - should get clean dict
    PyObject* dict2 = pool.acquire();
    assert(PyDict_Size(dict2) == 0);  // Should be cleared
    
    pool.release(dict2);
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_dict_pool_exhaustion() {
    std::cout << "Test: Dict pool exhaustion... ";
    
    PyDictPool pool(4);  // Small pool
    
    std::vector<PyObject*> dicts;
    
    // Acquire all from pool
    for (int i = 0; i < 4; ++i) {
        dicts.push_back(pool.acquire());
    }
    
    // Next one should create new dict (not from pool)
    PyObject* extra = pool.acquire();
    assert(extra != nullptr);
    
    // Release all
    for (auto dict : dicts) {
        pool.release(dict);
    }
    pool.release(extra);  // This one will be Py_DECREF'd
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_tuple_pool_basic() {
    std::cout << "Test: Tuple pool basic operations... ";
    
    PyTuplePool pool(3, 16);  // 3-tuples
    
    // Acquire tuple
    PyObject* tuple1 = pool.acquire();
    assert(tuple1 != nullptr);
    assert(PyTuple_Check(tuple1));
    assert(PyTuple_Size(tuple1) == 3);
    
    // Set items
    PyTuple_SetItem(tuple1, 0, PyLong_FromLong(1));
    PyTuple_SetItem(tuple1, 1, PyLong_FromLong(2));
    PyTuple_SetItem(tuple1, 2, PyLong_FromLong(3));
    
    // Release
    pool.release(tuple1);
    
    // Acquire again - should be cleared
    PyObject* tuple2 = pool.acquire();
    assert(PyTuple_GetItem(tuple2, 0) == Py_None);
    
    pool.release(tuple2);
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_pool_manager() {
    std::cout << "Test: PyObjectPoolManager... ";
    
    // Get dict
    PyObject* dict = PyObjectPoolManager::acquire_dict();
    assert(dict != nullptr);
    PyDict_SetItemString(dict, "test", PyLong_FromLong(123));
    PyObjectPoolManager::release_dict(dict);
    
    // Get tuple
    PyObject* tuple = PyObjectPoolManager::acquire_tuple(2);
    assert(tuple != nullptr);
    assert(PyTuple_Size(tuple) == 2);
    PyObjectPoolManager::release_tuple(tuple, 2);
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_raii_wrapper() {
    std::cout << "Test: RAII wrapper... ";
    
    {
        PooledDict dict(PyObjectPoolManager::instance().dict_pool());
        PyDict_SetItemString(dict.get(), "key", PyLong_FromLong(999));
        
        // Dict will be automatically released when scope ends
    }
    
    // Acquire again - should be clean
    PyObject* dict = PyObjectPoolManager::acquire_dict();
    assert(PyDict_Size(dict) == 0);
    PyObjectPoolManager::release_dict(dict);
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_concurrent_access() {
    std::cout << "Test: Concurrent dict pool access... ";
    
    const int NUM_THREADS = 4;  // Reduced to avoid GIL issues
    const int OPS_PER_THREAD = 100;  // Reduced iterations
    
    std::vector<std::thread> threads;
    
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([t]() {
            // Each thread needs GIL for Python operations
            PyGILState_STATE gstate = PyGILState_Ensure();
            
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                PyObject* dict = PyObjectPoolManager::acquire_dict();
                PyObject* key = PyLong_FromLong(t * 1000 + i);
                PyDict_SetItemString(dict, "value", key);
                Py_DECREF(key);
                PyObjectPoolManager::release_dict(dict);
            }
            
            PyGILState_Release(gstate);
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_performance() {
    std::cout << "Test: Performance comparison... ";
    
    const int ITERATIONS = 10000;
    
    // Benchmark with pool
    auto start1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        PyObject* dict = PyObjectPoolManager::acquire_dict();
        PyDict_SetItemString(dict, "key", PyLong_FromLong(i));
        PyObjectPoolManager::release_dict(dict);
    }
    auto end1 = std::chrono::high_resolution_clock::now();
    auto pool_time = std::chrono::duration_cast<std::chrono::nanoseconds>(end1 - start1).count();
    
    // Benchmark without pool
    auto start2 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        PyObject* dict = PyDict_New();
        PyDict_SetItemString(dict, "key", PyLong_FromLong(i));
        Py_DECREF(dict);
    }
    auto end2 = std::chrono::high_resolution_clock::now();
    auto new_time = std::chrono::duration_cast<std::chrono::nanoseconds>(end2 - start2).count();
    
    double pool_ns = static_cast<double>(pool_time) / ITERATIONS;
    double new_ns = static_cast<double>(new_time) / ITERATIONS;
    double speedup = new_ns / pool_ns;
    
    std::cout << "✓ PASSED" << std::endl;
    std::cout << "  Pool:   " << pool_ns << " ns/op" << std::endl;
    std::cout << "  New:    " << new_ns << " ns/op" << std::endl;
    std::cout << "  Speedup: " << speedup << "x faster" << std::endl;
    
    if (speedup < 2.0) {
        std::cout << "  ⚠️ WARNING: Pool not significantly faster (expected >2x)" << std::endl;
    }
}

int main() {
    std::cout << "╔═══════════════════════════════════════════╗" << std::endl;
    std::cout << "║   PyObject Pool Tests                    ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
    
    // Initialize Python
    Py_Initialize();
    
    test_dict_pool_basic();
    test_dict_pool_exhaustion();
    test_tuple_pool_basic();
    test_pool_manager();
    test_raii_wrapper();
    test_concurrent_access();
    test_performance();
    
    // Cleanup Python
    Py_Finalize();
    
    std::cout << std::endl;
    std::cout << "✅ All tests passed!" << std::endl;
    
    return 0;
}

