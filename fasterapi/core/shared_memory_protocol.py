"""Shared memory IPC protocol for communicating with C++ server.

This module provides Python bindings for the shared memory ring buffer system
used to communicate between the C++ HTTP server and Python worker processes.
"""

import struct
import json
from multiprocessing import shared_memory
from typing import Optional, Tuple
import mmap
import os
import posix_ipc


# Message type constants (must match C++ enum)
class MessageType:
    REQUEST = 1
    RESPONSE = 2
    SHUTDOWN = 3


# Message header structures (must match C++ packed structs)
class MessageHeader:
    """Request message header (21 bytes)."""
    FORMAT = '=BIIIII'  # type, request_id, total_length, module_name_len, function_name_len, kwargs_json_len
    SIZE = struct.calcsize(FORMAT)

    @staticmethod
    def pack(msg_type: int, request_id: int, module_name: str, function_name: str, kwargs_json: str) -> bytes:
        module_name_bytes = module_name.encode('utf-8')
        function_name_bytes = function_name.encode('utf-8')
        kwargs_json_bytes = kwargs_json.encode('utf-8')

        total_length = MessageHeader.SIZE + len(module_name_bytes) + len(function_name_bytes) + len(kwargs_json_bytes)

        header = struct.pack(
            MessageHeader.FORMAT,
            msg_type,
            request_id,
            total_length,
            len(module_name_bytes),
            len(function_name_bytes),
            len(kwargs_json_bytes)
        )

        return header + module_name_bytes + function_name_bytes + kwargs_json_bytes

    @staticmethod
    def unpack(data: bytes) -> Tuple[int, int, str, str, str]:
        """Unpack request message. Returns (msg_type, request_id, module_name, function_name, kwargs_json)."""
        if len(data) < MessageHeader.SIZE:
            raise ValueError(f"Data too short: {len(data)} < {MessageHeader.SIZE}")

        msg_type, request_id, total_length, module_name_len, function_name_len, kwargs_json_len = struct.unpack(
            MessageHeader.FORMAT, data[:MessageHeader.SIZE]
        )

        offset = MessageHeader.SIZE
        module_name = data[offset:offset+module_name_len].decode('utf-8')
        offset += module_name_len
        function_name = data[offset:offset+function_name_len].decode('utf-8')
        offset += function_name_len
        kwargs_json = data[offset:offset+kwargs_json_len].decode('utf-8')

        return msg_type, request_id, module_name, function_name, kwargs_json


class ResponseHeader:
    """Response message header (19 bytes)."""
    FORMAT = '=BIIIHIB'  # type, request_id, total_length, body_json_len, status_code, error_message_len, success
    SIZE = struct.calcsize(FORMAT)

    @staticmethod
    def pack(request_id: int, status_code: int, success: bool, body_json: str, error_message: str = "") -> bytes:
        body_json_bytes = body_json.encode('utf-8')
        error_message_bytes = error_message.encode('utf-8')

        total_length = ResponseHeader.SIZE + len(body_json_bytes) + len(error_message_bytes)

        header = struct.pack(
            ResponseHeader.FORMAT,
            MessageType.RESPONSE,
            request_id,
            total_length,
            len(body_json_bytes),
            status_code,
            len(error_message_bytes),
            1 if success else 0
        )

        return header + body_json_bytes + error_message_bytes

    @staticmethod
    def unpack(data: bytes) -> Tuple[int, int, bool, str, str]:
        """Unpack response message. Returns (request_id, status_code, success, body_json, error_message)."""
        if len(data) < ResponseHeader.SIZE:
            raise ValueError(f"Data too short: {len(data)} < {ResponseHeader.SIZE}")

        msg_type, request_id, total_length, body_json_len, status_code, error_message_len, success = struct.unpack(
            ResponseHeader.FORMAT, data[:ResponseHeader.SIZE]
        )

        offset = ResponseHeader.SIZE
        body_json = data[offset:offset+body_json_len].decode('utf-8')
        offset += body_json_len
        error_message = data[offset:offset+error_message_len].decode('utf-8')

        return request_id, status_code, bool(success), body_json, error_message


class RingBufferSlot:
    """Single slot in the ring buffer (4100 bytes total)."""
    MAX_DATA_SIZE = 4096
    TOTAL_SIZE = 4 + MAX_DATA_SIZE  # length (uint32_t) + data

    def __init__(self, shm_buffer: memoryview, offset: int):
        self.buffer = shm_buffer
        self.offset = offset

    @property
    def length(self) -> int:
        """Get the message length (atomic read)."""
        return struct.unpack('=I', self.buffer[self.offset:self.offset+4])[0]

    @length.setter
    def length(self, value: int):
        """Set the message length (atomic write)."""
        struct.pack_into('=I', self.buffer, self.offset, value)

    def read_data(self, length: int) -> bytes:
        """Read data from the slot."""
        data_offset = self.offset + 4
        return bytes(self.buffer[data_offset:data_offset+length])

    def write_data(self, data: bytes):
        """Write data to the slot and set length."""
        if len(data) > self.MAX_DATA_SIZE:
            raise ValueError(f"Data too large: {len(data)} > {self.MAX_DATA_SIZE}")

        data_offset = self.offset + 4
        self.buffer[data_offset:data_offset+len(data)] = data
        self.length = len(data)

    def clear(self):
        """Clear the slot."""
        self.length = 0


class RingBufferControl:
    """Control block for a ring buffer."""
    SIZE = 24  # head (4) + tail (4) + capacity (4) + padding for semaphore pointers (12)

    def __init__(self, shm_buffer: memoryview, offset: int):
        self.buffer = shm_buffer
        self.offset = offset

    @property
    def head(self) -> int:
        return struct.unpack('=I', self.buffer[self.offset:self.offset+4])[0]

    @head.setter
    def head(self, value: int):
        struct.pack_into('=I', self.buffer, self.offset, value)

    @property
    def tail(self) -> int:
        return struct.unpack('=I', self.buffer[self.offset+4:self.offset+8])[0]

    @tail.setter
    def tail(self, value: int):
        struct.pack_into('=I', self.buffer, self.offset+4, value)

    @property
    def capacity(self) -> int:
        return struct.unpack('=I', self.buffer[self.offset+8:self.offset+12])[0]


class SharedMemoryIPC:
    """Python interface to the shared memory IPC system.

    Workers use this to read requests and write responses.
    """

    DEFAULT_QUEUE_SIZE = 256

    def __init__(self, shm_name: str):
        """Attach to existing shared memory region created by C++.

        Args:
            shm_name: Name of the shared memory object
        """
        self.shm_name = shm_name

        # Open shared memory using posix_ipc (cross-platform)
        # shm_name already includes leading slash from C++
        self.shm = posix_ipc.SharedMemory(shm_name)
        self.shm_fd = self.shm.fd
        shm_size = self.shm.size
        self.shm_buffer = mmap.mmap(self.shm_fd, shm_size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)

        # Parse control blocks
        self.request_control = RingBufferControl(memoryview(self.shm_buffer), 0)
        self.response_control = RingBufferControl(memoryview(self.shm_buffer), RingBufferControl.SIZE)

        # Calculate slot offsets
        control_section_size = 2 * RingBufferControl.SIZE
        request_queue_size = self.request_control.capacity
        response_queue_size = self.response_control.capacity

        # Create slot accessors
        self.request_slots = []
        offset = control_section_size
        for i in range(request_queue_size):
            self.request_slots.append(RingBufferSlot(memoryview(self.shm_buffer), offset))
            offset += RingBufferSlot.TOTAL_SIZE

        self.response_slots = []
        for i in range(response_queue_size):
            self.response_slots.append(RingBufferSlot(memoryview(self.shm_buffer), offset))
            offset += RingBufferSlot.TOTAL_SIZE

        # Open semaphores (shm_name already includes leading slash)
        self.req_write_sem = posix_ipc.Semaphore(f"{shm_name}_req_write")
        self.req_read_sem = posix_ipc.Semaphore(f"{shm_name}_req_read")
        self.resp_write_sem = posix_ipc.Semaphore(f"{shm_name}_resp_write")
        self.resp_read_sem = posix_ipc.Semaphore(f"{shm_name}_resp_read")

        self.shutdown = False

    def read_request(self) -> Optional[Tuple[int, str, str, dict]]:
        """Read a request from the queue. Blocks if empty.

        Returns:
            (request_id, module_name, function_name, kwargs_dict) or None on shutdown
        """
        # Wait for data
        self.req_read_sem.acquire()

        if self.shutdown:
            return None

        # Read from tail
        tail = self.request_control.tail
        slot = self.request_slots[tail]

        length = slot.length
        if length == 0:
            return None

        data = slot.read_data(length)

        # Parse message
        msg_type, request_id, module_name, function_name, kwargs_json = MessageHeader.unpack(data)

        if msg_type == MessageType.SHUTDOWN:
            self.shutdown = True
            return None

        # Parse kwargs JSON
        kwargs = json.loads(kwargs_json) if kwargs_json else {}

        # Clear slot and advance tail
        slot.clear()
        self.request_control.tail = (tail + 1) % self.request_control.capacity

        # Signal space available
        self.req_write_sem.release()

        return request_id, module_name, function_name, kwargs

    def write_response(self, request_id: int, status_code: int, success: bool,
                       body: any, error_message: str = ""):
        """Write a response to the queue. Blocks if full.

        Args:
            request_id: ID of the request being responded to
            status_code: HTTP status code (200, 500, etc.)
            success: True if successful, False if error
            body: Response body (will be JSON-serialized)
            error_message: Error message if success is False
        """
        # Serialize body to JSON
        body_json = json.dumps(body) if body is not None else "{}"

        # Pack message
        message = ResponseHeader.pack(request_id, status_code, success, body_json, error_message)

        # Wait for space
        self.resp_write_sem.acquire()

        # Write to head
        head = self.response_control.head
        slot = self.response_slots[head]

        slot.write_data(message)

        # Advance head
        self.response_control.head = (head + 1) % self.response_control.capacity

        # Signal data available
        self.resp_read_sem.release()

    def close(self):
        """Clean up resources."""
        if self.shm_buffer:
            self.shm_buffer.close()
        if self.shm:
            self.shm.close_fd()

        # Close semaphores
        self.req_write_sem.close()
        self.req_read_sem.close()
        self.resp_write_sem.close()
        self.resp_read_sem.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
