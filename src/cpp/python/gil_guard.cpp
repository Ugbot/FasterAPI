#include "gil_guard.h"
#include <iostream>

namespace fasterapi {
namespace python {

int initialize_python_threading() noexcept {
    // Check if Python is initialized
    if (!Py_IsInitialized()) {
        std::cerr << "Python not initialized" << std::endl;
        return 1;
    }
    
    // Initialize Python threading
    // This enables multi-threading support in Python
    // Note: PyEval_InitThreads() is deprecated in Python 3.9+ (called automatically)
    // No need to call it explicitly in Python 3.9+
    
    // Release GIL so worker threads can acquire it
    PyEval_SaveThread();
    
    std::cout << "Python threading initialized" << std::endl;
    return 0;
}

int shutdown_python_threading() noexcept {
    // Reacquire GIL before shutdown
    PyGILState_Ensure();
    
    std::cout << "Python threading shutdown" << std::endl;
    return 0;
}

} // namespace python
} // namespace fasterapi

