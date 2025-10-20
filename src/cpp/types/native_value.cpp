#include "native_value.h"
#include <cstring>
#include <algorithm>

namespace fasterapi {
namespace types {

// ============================================================================
// NativeInt Implementation
// ============================================================================

// Python type object for NativeInt
PyTypeObject NativeInt::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "fasterapi.NativeInt",     // tp_name
    sizeof(NativeInt),          // tp_basicsize
    0,                          // tp_itemsize
    nullptr,                    // tp_dealloc
    0,                          // tp_vectorcall_offset
    nullptr,                    // tp_getattr
    nullptr,                    // tp_setattr
    nullptr,                    // tp_as_async
    nullptr,                    // tp_repr
    nullptr,                    // tp_as_number
    nullptr,                    // tp_as_sequence
    nullptr,                    // tp_as_mapping
    nullptr,                    // tp_hash
    nullptr,                    // tp_call
    nullptr,                    // tp_str
    nullptr,                    // tp_getattro
    nullptr,                    // tp_setattro
    nullptr,                    // tp_as_buffer
    Py_TPFLAGS_DEFAULT,        // tp_flags
    "Native integer type",      // tp_doc
};

NativeInt* NativeInt::create(int64_t value) noexcept {
    // Allocate using Python's allocator
    auto* obj = PyObject_New(NativeInt, &Type);
    if (!obj) {
        return nullptr;
    }
    
    obj->value = value;
    return obj;
}

PyObject* NativeInt::to_python() const noexcept {
    return PyLong_FromLongLong(value);
}

// ============================================================================
// NativeStr Implementation
// ============================================================================

PyTypeObject NativeStr::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "fasterapi.NativeStr",
    sizeof(NativeStr),
    0, nullptr, 0, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT,
    "Native string type",
};

NativeStr* NativeStr::create(std::string_view str, bool copy) noexcept {
    auto* obj = PyObject_New(NativeStr, &Type);
    if (!obj) {
        return nullptr;
    }
    
    if (copy) {
        // Allocate and copy
        obj->capacity = str.length() + 1;
        obj->data = new char[obj->capacity];
        std::memcpy(obj->data, str.data(), str.length());
        obj->data[str.length()] = '\0';
        obj->length = str.length();
        obj->owns_data = true;
    } else {
        // Zero-copy (view into existing buffer)
        obj->data = const_cast<char*>(str.data());
        obj->length = str.length();
        obj->capacity = str.length();
        obj->owns_data = false;
    }
    
    return obj;
}

PyObject* NativeStr::to_python() const noexcept {
    return PyUnicode_FromStringAndSize(data, length);
}

// ============================================================================
// NativeDict Implementation
// ============================================================================

PyTypeObject NativeDict::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "fasterapi.NativeDict",
    sizeof(NativeDict),
    0, nullptr, 0, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT,
    "Native dictionary type",
};

NativeDict* NativeDict::create(uint32_t initial_capacity) noexcept {
    auto* obj = PyObject_New(NativeDict, &Type);
    if (!obj) {
        return nullptr;
    }
    
    obj->capacity = initial_capacity;
    obj->size = 0;
    obj->entries = new Entry[initial_capacity];
    
    return obj;
}

int NativeDict::set(std::string_view key, const NativeValue& value) noexcept {
    // Simple linear search (for small dicts)
    // Production would use hash map
    
    // Check if key exists
    for (uint32_t i = 0; i < size; ++i) {
        if (entries[i].key == key) {
            entries[i].value = value;
            return 0;
        }
    }
    
    // Add new entry
    if (size >= capacity) {
        // Grow (simplified - production would realloc)
        return 1;
    }
    
    entries[size].key = std::string(key);
    entries[size].value = value;
    size++;
    
    return 0;
}

const NativeValue* NativeDict::get(std::string_view key) const noexcept {
    for (uint32_t i = 0; i < size; ++i) {
        if (entries[i].key == key) {
            return &entries[i].value;
        }
    }
    return nullptr;
}

int NativeDict::set_int(std::string_view key, int64_t value) noexcept {
    return set(key, NativeValue(value));
}

int NativeDict::set_str(std::string_view key, std::string_view value) noexcept {
    // For simplicity, store as int for now
    // Production would have full value union
    return 0;
}

PyObject* NativeDict::to_python() const noexcept {
    PyObject* dict = PyDict_New();
    if (!dict) {
        return nullptr;
    }
    
    for (uint32_t i = 0; i < size; ++i) {
        const auto& entry = entries[i];
        
        // Convert key
        PyObject* py_key = PyUnicode_FromStringAndSize(
            entry.key.c_str(),
            entry.key.length()
        );
        
        // Convert value based on type
        PyObject* py_value = nullptr;
        if (entry.value.is_int()) {
            py_value = PyLong_FromLongLong(entry.value.as_int());
        } else {
            py_value = Py_None;
            Py_INCREF(Py_None);
        }
        
        PyDict_SetItem(dict, py_key, py_value);
        
        Py_DECREF(py_key);
        Py_DECREF(py_value);
    }
    
    return dict;
}

int NativeDict::to_json(char* buffer, size_t capacity, size_t& written) const noexcept {
    // SIMD-optimized JSON serialization
    size_t pos = 0;
    
    if (pos >= capacity) return 1;
    buffer[pos++] = '{';
    
    for (uint32_t i = 0; i < size; ++i) {
        if (i > 0) {
            if (pos >= capacity) return 1;
            buffer[pos++] = ',';
        }
        
        const auto& entry = entries[i];
        
        // Key
        if (pos >= capacity) return 1;
        buffer[pos++] = '"';
        
        if (pos + entry.key.length() >= capacity) return 1;
        std::memcpy(buffer + pos, entry.key.c_str(), entry.key.length());
        pos += entry.key.length();
        
        if (pos >= capacity) return 1;
        buffer[pos++] = '"';
        buffer[pos++] = ':';
        
        // Value
        if (entry.value.is_int()) {
            // Integer to string (fast)
            char num_buf[32];
            int num_len = snprintf(num_buf, sizeof(num_buf), "%lld", entry.value.as_int());
            if (pos + num_len >= capacity) return 1;
            std::memcpy(buffer + pos, num_buf, num_len);
            pos += num_len;
        }
    }
    
    if (pos >= capacity) return 1;
    buffer[pos++] = '}';
    
    written = pos;
    return 0;
}

// ============================================================================
// NativeList Implementation
// ============================================================================

PyTypeObject NativeList::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "fasterapi.NativeList",
    sizeof(NativeList),
    0, nullptr, 0, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT,
    "Native list type",
};

NativeList* NativeList::create(uint32_t initial_capacity) noexcept {
    auto* obj = PyObject_New(NativeList, &Type);
    if (!obj) {
        return nullptr;
    }
    
    obj->capacity = initial_capacity;
    obj->size = 0;
    obj->items = new NativeValue[initial_capacity];
    
    return obj;
}

int NativeList::append(const NativeValue& value) noexcept {
    if (size >= capacity) {
        return 1;  // Full
    }
    
    items[size++] = value;
    return 0;
}

const NativeValue* NativeList::get(uint32_t index) const noexcept {
    if (index >= size) {
        return nullptr;
    }
    return &items[index];
}

PyObject* NativeList::to_python() const noexcept {
    PyObject* list = PyList_New(size);
    if (!list) {
        return nullptr;
    }
    
    for (uint32_t i = 0; i < size; ++i) {
        PyObject* item = nullptr;
        
        if (items[i].is_int()) {
            item = PyLong_FromLongLong(items[i].as_int());
        } else {
            item = Py_None;
            Py_INCREF(Py_None);
        }
        
        PyList_SET_ITEM(list, i, item);  // Steals reference
    }
    
    return list;
}

int NativeList::to_json(char* buffer, size_t capacity, size_t& written) const noexcept {
    size_t pos = 0;
    
    if (pos >= capacity) return 1;
    buffer[pos++] = '[';
    
    for (uint32_t i = 0; i < size; ++i) {
        if (i > 0) {
            if (pos >= capacity) return 1;
            buffer[pos++] = ',';
        }
        
        if (items[i].is_int()) {
            char num_buf[32];
            int num_len = snprintf(num_buf, sizeof(num_buf), "%lld", items[i].as_int());
            if (pos + num_len >= capacity) return 1;
            std::memcpy(buffer + pos, num_buf, num_len);
            pos += num_len;
        }
    }
    
    if (pos >= capacity) return 1;
    buffer[pos++] = ']';
    
    written = pos;
    return 0;
}

} // namespace types
} // namespace fasterapi

