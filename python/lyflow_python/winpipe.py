from __future__ import annotations

import ctypes
from ctypes import wintypes
import os


class _Overlapped(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD), ("hEvent", wintypes.HANDLE)]


class NamedPipe:
    def __init__(self, name):
        if os.name != "nt":
            raise OSError("Win32 named pipes require Windows")
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        signatures = {
            "CreateFileW": ([wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE], wintypes.HANDLE),
            "WaitNamedPipeW": ([wintypes.LPCWSTR, wintypes.DWORD], wintypes.BOOL),
            "CreateEventW": ([ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR], wintypes.HANDLE),
            "ReadFile": ([wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(_Overlapped)], wintypes.BOOL),
            "WriteFile": ([wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(_Overlapped)], wintypes.BOOL),
            "GetOverlappedResult": ([wintypes.HANDLE, ctypes.POINTER(_Overlapped), ctypes.POINTER(wintypes.DWORD), wintypes.BOOL], wintypes.BOOL),
            "CancelIoEx": ([wintypes.HANDLE, ctypes.POINTER(_Overlapped)], wintypes.BOOL),
            "CloseHandle": ([wintypes.HANDLE], wintypes.BOOL),
        }
        for method, (arguments, result) in signatures.items():
            function = getattr(self.kernel, method)
            function.argtypes, function.restype = arguments, result
        self.handle = None
        handle = self.kernel.CreateFileW(name, 0xC0000000, 0, None, 3, 0x40000080, None)
        invalid = ctypes.c_void_p(-1).value
        if handle == invalid and ctypes.get_last_error() == 231:
            if not self.kernel.WaitNamedPipeW(name, 5000):
                raise ctypes.WinError(ctypes.get_last_error())
            handle = self.kernel.CreateFileW(name, 0xC0000000, 0, None, 3, 0x40000080, None)
        if handle == invalid:
            raise ctypes.WinError(ctypes.get_last_error())
        self.handle = handle

    def _transfer(self, buffer, size, writing):
        if self.handle is None:
            raise BrokenPipeError("named pipe is closed")
        event = self.kernel.CreateEventW(None, True, False, None)
        if not event:
            raise ctypes.WinError(ctypes.get_last_error())
        operation = _Overlapped()
        operation.hEvent = event
        count = wintypes.DWORD()
        try:
            method = self.kernel.WriteFile if writing else self.kernel.ReadFile
            completed = method(self.handle, buffer, size, ctypes.byref(count), ctypes.byref(operation))
            error = 0 if completed else ctypes.get_last_error()
            if not completed and error == 997:
                completed = self.kernel.GetOverlappedResult(self.handle, ctypes.byref(operation), ctypes.byref(count), True)
                error = 0 if completed else ctypes.get_last_error()
            if not completed:
                if not writing and error in (109, 233):
                    return 0
                raise ctypes.WinError(error)
            return count.value
        finally:
            self.kernel.CloseHandle(event)

    def read(self, size):
        if size <= 0:
            return b""
        buffer = ctypes.create_string_buffer(size)
        count = self._transfer(buffer, size, False)
        return buffer.raw[:count]

    def write(self, data):
        data = bytes(data)
        buffer = ctypes.create_string_buffer(data)
        return self._transfer(buffer, len(data), True)

    def flush(self):
        pass

    def close(self):
        if self.handle is not None:
            self.kernel.CancelIoEx(self.handle, None)
            self.kernel.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
