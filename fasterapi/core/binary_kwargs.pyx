# cython: language_level=3, boundscheck=False, wraparound=False, cdivision=True
# cython: initializedcheck=False, nonecheck=False
"""
Binary Kwargs Decoder (Cython)

High-performance TLV decoder for Python IPC.
Target: ~100ns for 5 simple parameters.

This module decodes the binary format produced by C++ BinaryKwargsEncoder.
"""

from libc.stdint cimport uint8_t, uint16_t, uint32_t, uint64_t, int8_t, int16_t, int32_t, int64_t
from libc.string cimport memcpy
from cpython.bytes cimport PyBytes_AS_STRING, PyBytes_GET_SIZE
from cpython.dict cimport PyDict_SetItem
from cpython.unicode cimport PyUnicode_DecodeUTF8

# Magic byte to identify binary TLV format
DEF BINARY_KWARGS_MAGIC = 0xFA

# Type tags (must match C++ KwargsTypeTag)
DEF TAG_NULL = 0x00
DEF TAG_BOOL_FALSE = 0x01
DEF TAG_BOOL_TRUE = 0x02
DEF TAG_INT8 = 0x10
DEF TAG_INT16 = 0x11
DEF TAG_INT32 = 0x12
DEF TAG_INT64 = 0x13
DEF TAG_UINT8 = 0x18
DEF TAG_UINT16 = 0x19
DEF TAG_UINT32 = 0x1A
DEF TAG_UINT64 = 0x1B
DEF TAG_FLOAT32 = 0x20
DEF TAG_FLOAT64 = 0x21
DEF TAG_STR_TINY = 0x30
DEF TAG_STR_SHORT = 0x31
DEF TAG_STR_MEDIUM = 0x32
DEF TAG_BYTES_TINY = 0x40
DEF TAG_BYTES_SHORT = 0x41
DEF TAG_BYTES_MEDIUM = 0x42
DEF TAG_MSGPACK = 0x70
DEF TAG_JSON = 0x7F


cdef inline uint16_t read_u16_le(const uint8_t* p) noexcept nogil:
    """Read little-endian uint16_t from buffer."""
    return <uint16_t>p[0] | (<uint16_t>p[1] << 8)


cdef inline uint32_t read_u32_le(const uint8_t* p) noexcept nogil:
    """Read little-endian uint32_t from buffer."""
    return (<uint32_t>p[0] |
            (<uint32_t>p[1] << 8) |
            (<uint32_t>p[2] << 16) |
            (<uint32_t>p[3] << 24))


cdef inline uint64_t read_u64_le(const uint8_t* p) noexcept nogil:
    """Read little-endian uint64_t from buffer."""
    cdef uint64_t val = 0
    cdef int i
    for i in range(8):
        val |= (<uint64_t>p[i]) << (i * 8)
    return val


cdef inline double read_f64_le(const uint8_t* p) noexcept nogil:
    """Read little-endian float64 from buffer."""
    cdef uint64_t bits = read_u64_le(p)
    cdef double result
    memcpy(&result, &bits, sizeof(result))
    return result


cdef inline float read_f32_le(const uint8_t* p) noexcept nogil:
    """Read little-endian float32 from buffer."""
    cdef uint32_t bits = read_u32_le(p)
    cdef float result
    memcpy(&result, &bits, sizeof(result))
    return result


cpdef bint is_binary_format(object data):
    """Check if data is in binary TLV format."""
    cdef const uint8_t* buf
    cdef Py_ssize_t length

    if isinstance(data, bytes):
        buf = <const uint8_t*>PyBytes_AS_STRING(data)
        length = PyBytes_GET_SIZE(data)
        return length >= 1 and buf[0] == BINARY_KWARGS_MAGIC
    elif isinstance(data, memoryview):
        length = len(data)
        if length >= 1:
            return data[0] == BINARY_KWARGS_MAGIC
    return False


cpdef dict decode_kwargs(object data):
    """
    Decode TLV-encoded kwargs to Python dict.

    Args:
        data: bytes or memoryview containing binary-encoded kwargs

    Returns:
        dict: Decoded key-value pairs

    Raises:
        ValueError: If data is invalid or corrupt
    """
    cdef const uint8_t* buf
    cdef Py_ssize_t length
    cdef Py_ssize_t pos = 0
    cdef uint16_t param_count
    cdef uint16_t i
    cdef uint8_t name_len
    cdef uint8_t tag
    cdef str name
    cdef object value
    cdef Py_ssize_t str_len
    cdef dict result

    # Get buffer pointer
    if isinstance(data, bytes):
        buf = <const uint8_t*>PyBytes_AS_STRING(data)
        length = PyBytes_GET_SIZE(data)
    elif isinstance(data, memoryview):
        # For memoryview, we need to get the underlying buffer
        temp_bytes = bytes(data)
        buf = <const uint8_t*>PyBytes_AS_STRING(temp_bytes)
        length = PyBytes_GET_SIZE(temp_bytes)
    else:
        raise TypeError("Expected bytes or memoryview")

    # Validate minimum size (magic + param_count)
    if length < 3:
        raise ValueError("Data too short for binary kwargs")

    # Check magic byte
    if buf[0] != BINARY_KWARGS_MAGIC:
        raise ValueError("Invalid binary kwargs magic byte")

    # Read param count
    param_count = read_u16_le(&buf[1])
    pos = 3

    # Pre-allocate result dict
    result = {}

    # Decode each parameter
    for i in range(param_count):
        if pos >= length:
            raise ValueError("Unexpected end of data")

        # Read name length
        name_len = buf[pos]
        pos += 1

        if pos + name_len > length:
            raise ValueError("Name extends beyond data")

        # Decode name as UTF-8 string
        name = PyUnicode_DecodeUTF8(<const char*>&buf[pos], name_len, NULL)
        pos += name_len

        if pos >= length:
            raise ValueError("Missing type tag")

        # Read type tag
        tag = buf[pos]
        pos += 1

        # Decode value based on tag
        if tag == TAG_NULL:
            value = None

        elif tag == TAG_BOOL_FALSE:
            value = False

        elif tag == TAG_BOOL_TRUE:
            value = True

        elif tag == TAG_INT8:
            if pos + 1 > length:
                raise ValueError("INT8 extends beyond data")
            value = <int8_t>buf[pos]
            pos += 1

        elif tag == TAG_INT16:
            if pos + 2 > length:
                raise ValueError("INT16 extends beyond data")
            value = <int16_t>read_u16_le(&buf[pos])
            pos += 2

        elif tag == TAG_INT32:
            if pos + 4 > length:
                raise ValueError("INT32 extends beyond data")
            value = <int32_t>read_u32_le(&buf[pos])
            pos += 4

        elif tag == TAG_INT64:
            if pos + 8 > length:
                raise ValueError("INT64 extends beyond data")
            value = <int64_t>read_u64_le(&buf[pos])
            pos += 8

        elif tag == TAG_UINT8:
            if pos + 1 > length:
                raise ValueError("UINT8 extends beyond data")
            value = <uint8_t>buf[pos]
            pos += 1

        elif tag == TAG_UINT16:
            if pos + 2 > length:
                raise ValueError("UINT16 extends beyond data")
            value = <uint16_t>read_u16_le(&buf[pos])
            pos += 2

        elif tag == TAG_UINT32:
            if pos + 4 > length:
                raise ValueError("UINT32 extends beyond data")
            value = <uint32_t>read_u32_le(&buf[pos])
            pos += 4

        elif tag == TAG_UINT64:
            if pos + 8 > length:
                raise ValueError("UINT64 extends beyond data")
            value = <uint64_t>read_u64_le(&buf[pos])
            pos += 8

        elif tag == TAG_FLOAT32:
            if pos + 4 > length:
                raise ValueError("FLOAT32 extends beyond data")
            value = <double>read_f32_le(&buf[pos])
            pos += 4

        elif tag == TAG_FLOAT64:
            if pos + 8 > length:
                raise ValueError("FLOAT64 extends beyond data")
            value = read_f64_le(&buf[pos])
            pos += 8

        elif tag == TAG_STR_TINY:
            if pos + 1 > length:
                raise ValueError("STR_TINY length extends beyond data")
            str_len = <Py_ssize_t>buf[pos]
            pos += 1
            if pos + str_len > length:
                raise ValueError("STR_TINY data extends beyond data")
            value = PyUnicode_DecodeUTF8(<const char*>&buf[pos], str_len, NULL)
            pos += str_len

        elif tag == TAG_STR_SHORT:
            if pos + 2 > length:
                raise ValueError("STR_SHORT length extends beyond data")
            str_len = <Py_ssize_t>read_u16_le(&buf[pos])
            pos += 2
            if pos + str_len > length:
                raise ValueError("STR_SHORT data extends beyond data")
            value = PyUnicode_DecodeUTF8(<const char*>&buf[pos], str_len, NULL)
            pos += str_len

        elif tag == TAG_STR_MEDIUM:
            if pos + 4 > length:
                raise ValueError("STR_MEDIUM length extends beyond data")
            str_len = <Py_ssize_t>read_u32_le(&buf[pos])
            pos += 4
            if pos + str_len > length:
                raise ValueError("STR_MEDIUM data extends beyond data")
            value = PyUnicode_DecodeUTF8(<const char*>&buf[pos], str_len, NULL)
            pos += str_len

        elif tag == TAG_BYTES_TINY:
            if pos + 1 > length:
                raise ValueError("BYTES_TINY length extends beyond data")
            str_len = <Py_ssize_t>buf[pos]
            pos += 1
            if pos + str_len > length:
                raise ValueError("BYTES_TINY data extends beyond data")
            value = bytes(data[pos:pos + str_len])
            pos += str_len

        elif tag == TAG_BYTES_SHORT:
            if pos + 2 > length:
                raise ValueError("BYTES_SHORT length extends beyond data")
            str_len = <Py_ssize_t>read_u16_le(&buf[pos])
            pos += 2
            if pos + str_len > length:
                raise ValueError("BYTES_SHORT data extends beyond data")
            value = bytes(data[pos:pos + str_len])
            pos += str_len

        elif tag == TAG_BYTES_MEDIUM:
            if pos + 4 > length:
                raise ValueError("BYTES_MEDIUM length extends beyond data")
            str_len = <Py_ssize_t>read_u32_le(&buf[pos])
            pos += 4
            if pos + str_len > length:
                raise ValueError("BYTES_MEDIUM data extends beyond data")
            value = bytes(data[pos:pos + str_len])
            pos += str_len

        elif tag == TAG_MSGPACK:
            if pos + 4 > length:
                raise ValueError("MSGPACK length extends beyond data")
            str_len = <Py_ssize_t>read_u32_le(&buf[pos])
            pos += 4
            if pos + str_len > length:
                raise ValueError("MSGPACK data extends beyond data")
            # Import msgpack lazily
            import msgpack
            value = msgpack.unpackb(bytes(data[pos:pos + str_len]), raw=False)
            pos += str_len

        elif tag == TAG_JSON:
            if pos + 4 > length:
                raise ValueError("JSON length extends beyond data")
            str_len = <Py_ssize_t>read_u32_le(&buf[pos])
            pos += 4
            if pos + str_len > length:
                raise ValueError("JSON data extends beyond data")
            # Import json lazily
            import json
            json_str = PyUnicode_DecodeUTF8(<const char*>&buf[pos], str_len, NULL)
            value = json.loads(json_str)
            pos += str_len

        else:
            raise ValueError(f"Unknown type tag: {tag}")

        # Add to result dict
        PyDict_SetItem(result, name, value)

    return result


# Pure Python fallback (used when Cython not compiled)
def decode_kwargs_pure_python(data: bytes) -> dict:
    """
    Pure Python implementation of binary kwargs decoder.

    This is a fallback when Cython extension is not available.
    About 10-20x slower than Cython version.
    """
    import struct

    if len(data) < 3:
        raise ValueError("Data too short for binary kwargs")

    if data[0] != BINARY_KWARGS_MAGIC:
        raise ValueError("Invalid binary kwargs magic byte")

    param_count = struct.unpack_from('<H', data, 1)[0]
    pos = 3
    result = {}

    for _ in range(param_count):
        if pos >= len(data):
            raise ValueError("Unexpected end of data")

        name_len = data[pos]
        pos += 1

        if pos + name_len > len(data):
            raise ValueError("Name extends beyond data")

        name = data[pos:pos + name_len].decode('utf-8')
        pos += name_len

        if pos >= len(data):
            raise ValueError("Missing type tag")

        tag = data[pos]
        pos += 1

        if tag == TAG_NULL:
            value = None
        elif tag == TAG_BOOL_FALSE:
            value = False
        elif tag == TAG_BOOL_TRUE:
            value = True
        elif tag == TAG_INT8:
            value = struct.unpack_from('<b', data, pos)[0]
            pos += 1
        elif tag == TAG_INT16:
            value = struct.unpack_from('<h', data, pos)[0]
            pos += 2
        elif tag == TAG_INT32:
            value = struct.unpack_from('<i', data, pos)[0]
            pos += 4
        elif tag == TAG_INT64:
            value = struct.unpack_from('<q', data, pos)[0]
            pos += 8
        elif tag == TAG_UINT8:
            value = data[pos]
            pos += 1
        elif tag == TAG_UINT16:
            value = struct.unpack_from('<H', data, pos)[0]
            pos += 2
        elif tag == TAG_UINT32:
            value = struct.unpack_from('<I', data, pos)[0]
            pos += 4
        elif tag == TAG_UINT64:
            value = struct.unpack_from('<Q', data, pos)[0]
            pos += 8
        elif tag == TAG_FLOAT32:
            value = struct.unpack_from('<f', data, pos)[0]
            pos += 4
        elif tag == TAG_FLOAT64:
            value = struct.unpack_from('<d', data, pos)[0]
            pos += 8
        elif tag == TAG_STR_TINY:
            str_len = data[pos]
            pos += 1
            value = data[pos:pos + str_len].decode('utf-8')
            pos += str_len
        elif tag == TAG_STR_SHORT:
            str_len = struct.unpack_from('<H', data, pos)[0]
            pos += 2
            value = data[pos:pos + str_len].decode('utf-8')
            pos += str_len
        elif tag == TAG_STR_MEDIUM:
            str_len = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            value = data[pos:pos + str_len].decode('utf-8')
            pos += str_len
        elif tag == TAG_BYTES_TINY:
            bytes_len = data[pos]
            pos += 1
            value = bytes(data[pos:pos + bytes_len])
            pos += bytes_len
        elif tag == TAG_BYTES_SHORT:
            bytes_len = struct.unpack_from('<H', data, pos)[0]
            pos += 2
            value = bytes(data[pos:pos + bytes_len])
            pos += bytes_len
        elif tag == TAG_BYTES_MEDIUM:
            bytes_len = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            value = bytes(data[pos:pos + bytes_len])
            pos += bytes_len
        elif tag == TAG_MSGPACK:
            import msgpack
            data_len = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            value = msgpack.unpackb(data[pos:pos + data_len], raw=False)
            pos += data_len
        elif tag == TAG_JSON:
            import json
            data_len = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            value = json.loads(data[pos:pos + data_len].decode('utf-8'))
            pos += data_len
        else:
            raise ValueError(f"Unknown type tag: {tag}")

        result[name] = value

    return result
