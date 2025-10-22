#pragma once

// Python C API
#ifndef PY_SSIZE_T_CLEAN
#define PY_SSIZE_T_CLEAN
#endif
#include <Python.h>

namespace fasterapi {
namespace python {

// Forward declaration for free-threading support
class FreeThreading;

/**
 * RAII guard for acquiring the Python GIL.
 * 
 * Use when calling into Python from a C++ thread.
 * Ensures GIL is properly released on scope exit.
 * 
 * Example:
 *   void worker_thread() {
 *       GILGuard gil;  // Acquire GIL
 *       PyObject* result = PyObject_CallFunction(...);
 *       // GIL released automatically on scope exit
 *   }
 */
class GILGuard {
public:
    /**
     * Acquire the GIL.
     * 
     * Blocks until GIL is available.
     * Thread-safe - can be called from any thread.
     */
    GILGuard() noexcept
        : state_(PyGILState_Ensure()) {
    }
    
    /**
     * Release the GIL.
     */
    ~GILGuard() noexcept {
        PyGILState_Release(state_);
    }
    
    // Non-copyable, non-movable
    GILGuard(const GILGuard&) = delete;
    GILGuard& operator=(const GILGuard&) = delete;
    GILGuard(GILGuard&&) = delete;
    GILGuard& operator=(GILGuard&&) = delete;
    
private:
    PyGILState_STATE state_;
};

/**
 * RAII guard for releasing the Python GIL.
 * 
 * Use when doing blocking I/O or long computations
 * from Python code to allow other threads to run.
 * 
 * Example:
 *   def my_handler():
 *       with GILRelease():  # Python context manager
 *           blocking_io()   # Other Python threads can run
 */
class GILRelease {
public:
    /**
     * Release the GIL.
     * 
     * Must be called from a thread that holds the GIL.
     */
    GILRelease() noexcept
        : state_(PyEval_SaveThread()) {
    }
    
    /**
     * Reacquire the GIL.
     */
    ~GILRelease() noexcept {
        if (state_) {
            PyEval_RestoreThread(state_);
        }
    }
    
    // Non-copyable, non-movable
    GILRelease(const GILRelease&) = delete;
    GILRelease& operator=(const GILRelease&) = delete;
    GILRelease(GILRelease&&) = delete;
    GILRelease& operator=(GILRelease&&) = delete;
    
private:
    PyThreadState* state_;
};

/**
 * Initialize Python threading support.
 * 
 * Must be called once during startup before creating
 * any worker threads.
 * 
 * @return 0 on success, error code otherwise
 */
int initialize_python_threading() noexcept;

/**
 * Shutdown Python threading support.
 *
 * @return 0 on success
 */
int shutdown_python_threading() noexcept;

/**
 * NOTE: For Python 3.13+ free-threading support, use ConditionalGILGuard
 * and ConditionalGILReleaseGuard from free_threading.h instead.
 *
 * These guards automatically become no-ops when Python is built with
 * --disable-gil, avoiding unnecessary overhead.
 *
 * Migration example:
 *   Old: GILGuard gil;
 *   New: ConditionalGILGuard gil;  // No-op if free-threading
 */

} // namespace python
} // namespace fasterapi

