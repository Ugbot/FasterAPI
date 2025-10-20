"""
Reactor Control from Python

Manages the C++ reactor lifecycle.
"""

from typing import Optional
from . import bindings as _bindings


class Reactor:
    """
    Per-core event loop manager.
    
    Controls the C++ reactor subsystem from Python.
    """
    
    _initialized = False
    _num_cores: Optional[int] = None
    
    @classmethod
    def initialize(cls, num_cores: int = 0) -> None:
        """
        Initialize reactor subsystem.
        
        Args:
            num_cores: Number of reactor cores (0 = auto-detect)
        """
        if cls._initialized:
            return
        
        if _bindings.reactor_initialize:
            result = _bindings.reactor_initialize(num_cores)
            if result == 0:
                cls._initialized = True
                if _bindings.reactor_num_cores:
                    cls._num_cores = _bindings.reactor_num_cores()
            else:
                raise RuntimeError(f"Failed to initialize reactor: {result}")
        else:
            # Fallback: no C++ reactor available
            import multiprocessing
            cls._num_cores = num_cores or multiprocessing.cpu_count()
            cls._initialized = True
    
    @classmethod
    def shutdown(cls) -> None:
        """Shutdown reactor subsystem."""
        if not cls._initialized:
            return
        
        if _bindings.reactor_shutdown:
            _bindings.reactor_shutdown()
        
        cls._initialized = False
        cls._num_cores = None
    
    @classmethod
    def current_core(cls) -> int:
        """Get current core ID."""
        if _bindings.reactor_current_core:
            return _bindings.reactor_current_core()
        return 0
    
    @classmethod
    def num_cores(cls) -> int:
        """Get total number of cores."""
        if not cls._initialized:
            cls.initialize()
        return cls._num_cores or 1
    
    @classmethod
    def is_initialized(cls) -> bool:
        """Check if reactor is initialized."""
        return cls._initialized

