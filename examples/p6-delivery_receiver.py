"""Receive LyFlow v1 publications; ACK only after native archive validation and durable save."""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import time
import uuid


class Core:
    def __init__(self, path):
        self._directory = os.add_dll_directory(str(path.resolve().parent)) if os.name == "nt" else None
        self.lib = ctypes.CDLL(str(path.resolve()))
        self.lib.lyflow_frame_load.argtypes = [ctypes.c_char_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_void_p)]
        self.lib.lyflow_frame_load.restype = ctypes.c_void_p
        self.lib.lyflow_frame_metadata.argtypes = [ctypes.c_void_p]
        self.lib.lyflow_frame_metadata.restype = ctypes.c_void_p
        self.lib.lyflow_frame_free.argtypes = [ctypes.c_void_p]
        self.lib.lyflow_frame_free.restype = None
        self.lib.lyflow_string_free.argtypes = [ctypes.c_void_p]
        self.lib.lyflow_string_free.restype = None

    def text(self, pointer):
        if not pointer:
            raise ValueError("core returned null")
        try:
            return ctypes.string_at(pointer).decode("utf-8")
        finally:
            self.lib.lyflow_string_free(pointer)

    def inspect(self, path, maximum):
        frame = ctypes.c_void_p()
        error = self.text(self.lib.lyflow_frame_load(str(path).encode("utf-8"), maximum, ctypes.byref(frame)))
        if error:
            raise ValueError(error)
        if not frame.value:
            raise ValueError("core returned no frame")
        try:
            return json.loads(self.text(self.lib.lyflow_frame_metadata(frame)))
        finally:
            self.lib.lyflow_frame_free(frame)


def exact(connection, count, deadline):
    data = bytearray()
    while len(data) < count:
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError("publication deadline exceeded")
        connection.settimeout(min(left, 0.1))
        try:
            chunk = connection.recv(min(count - len(data), 65536))
        except socket.timeout:
            continue
        if not chunk:
            raise ConnectionError("publication ended early")
        data.extend(chunk)
    return data


def receive(connection, directory, core, maximum, timeout, topic=None):
    deadline = time.monotonic() + timeout
    header = None
    path = directory / f"received-{uuid.uuid4().hex}.lyframe"
    temporary = path.with_suffix(".partial")
    accepted = False
    try:
        if exact(connection, 8, deadline) != b"LYFPUB1\0":
            raise ValueError("unsupported publication magic")
        header_size, size = struct.unpack("<IQ", exact(connection, 12, deadline))
        if not 0 < header_size <= 2 * 1024 * 1024 or not 0 < size <= maximum:
            raise ValueError("publication exceeds receiver limits")
        header = json.loads(exact(connection, header_size, deadline))
        if header.get("type") != "lyflow.Publish.v1" or header.get("schemaVersion") != 1:
            raise ValueError("unsupported publication version")
        if header.get("payloadFormat") != "lyflow.FrameArchive.v1" or header.get("payloadBytes") != str(size):
            raise ValueError("payload declaration mismatch")
        if topic is not None and header.get("topic") != topic:
            raise ValueError("topic mismatch")
        digest = hashlib.sha256()
        with temporary.open("xb") as output:
            remaining = size
            while remaining:
                chunk = exact(connection, min(remaining, 65536), deadline)
                digest.update(chunk)
                output.write(chunk)
                remaining -= len(chunk)
            output.flush()
            os.fsync(output.fileno())
        if digest.hexdigest() != header.get("payloadSha256"):
            raise ValueError("payload integrity mismatch")
        metadata = core.inspect(temporary, maximum)
        advertised = header.get("metadata", {})
        for key in set(metadata) | set(advertised):
            if key in {"viewRunId", "entries", "payloadBytes"}:
                continue
            if metadata.get(key) != advertised.get(key):
                raise ValueError(f"archive/header {key} mismatch")
        if metadata.get("frameId") != header.get("frameId"):
            raise ValueError("frame identity mismatch")
        if time.monotonic() >= deadline:
            raise TimeoutError("native validation exceeded deadline")
        with path.open("xb") as output, temporary.open("rb") as source:
            while chunk := source.read(65536):
                if time.monotonic() >= deadline:
                    raise TimeoutError("persistence exceeded deadline")
                output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        accepted = True
        response = {"type": "lyflow.PublishAck.v1", "schemaVersion": 1, "frameId": metadata["frameId"],
                    "session": metadata.get("session"), "accepted": True}
        connection.settimeout(max(0.001, deadline - time.monotonic()))
        connection.sendall((json.dumps(response, separators=(",", ":")) + "\n").encode())
        return {"type": "lyflow.Received.v1", "file": str(path), "metadata": metadata,
                "payloadBytes": str(size), "payloadSha256": digest.hexdigest()}
    except Exception as error:
        if header is not None and not accepted:
            response = {"type": "lyflow.PublishAck.v1", "schemaVersion": 1, "frameId": header.get("frameId"),
                        "session": header.get("metadata", {}).get("session"), "accepted": False, "message": str(error)}
            try:
                connection.sendall((json.dumps(response) + "\n").encode())
            except OSError:
                pass
        raise
    finally:
        temporary.unlink(missing_ok=True)
        if not accepted:
            path.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=54547)
    parser.add_argument("--count", type=int, default=1)
    parser.add_argument("--max-bytes", type=int, default=1024**3)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--topic")
    args = parser.parse_args()
    if not 0 < args.max_bytes <= 2 * 1024**3 or not 0 < args.timeout <= 3600 or args.count <= 0:
        parser.error("invalid maximum, timeout or count")
    args.directory.mkdir(parents=True, exist_ok=True)
    core = Core(args.core)
    family = socket.AF_INET6 if ":" in args.bind else socket.AF_INET
    with socket.socket(family, socket.SOCK_STREAM) as listener:
        listener.bind((args.bind, args.port))
        listener.listen(4)
        listener.settimeout(args.timeout)
        print(json.dumps({"type": "lyflow.ReceiverReady.v1", "address": listener.getsockname()}), flush=True)
        for _ in range(args.count):
            connection, _ = listener.accept()
            with connection:
                print(json.dumps(receive(connection, args.directory, core, args.max_bytes, args.timeout, args.topic)), flush=True)


if __name__ == "__main__":
    main()
