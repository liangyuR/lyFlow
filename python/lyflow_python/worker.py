from __future__ import annotations

import argparse
import inspect
import json
import queue
import struct
import sys
import threading
import traceback
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

MAX_FRAME_BYTES = 16 * 1024 * 1024


class ProtocolError(ValueError):
    pass


class TaskFailure(Exception):
    def __init__(self, code, error):
        super().__init__(str(error))
        self.code = code
        self.error = error


def _read_exact(stream, size, allow_eof=False):
    chunks = bytearray()
    while len(chunks) < size:
        chunk = stream.read(size - len(chunks))
        if not chunk:
            if allow_eof and not chunks:
                return None
            raise ProtocolError("truncated frame")
        chunks.extend(chunk)
    return bytes(chunks)


def read_frame(stream):
    header = _read_exact(stream, 4, allow_eof=True)
    if header is None:
        return None
    size, = struct.unpack("<I", header)
    if size == 0 or size > MAX_FRAME_BYTES:
        raise ProtocolError(f"invalid JSON frame length: {size}")
    try:
        value = json.loads(_read_exact(stream, size).decode("utf-8"), parse_constant=lambda value: (_ for _ in ()).throw(ProtocolError(f"invalid JSON constant: {value}")))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ProtocolError(f"invalid UTF-8 JSON frame: {error}") from error
    if not isinstance(value, dict):
        raise ProtocolError("JSON frame must be an object")
    return value


def write_frame(stream, payload):
    data = json.dumps(payload, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
    if not data or len(data) > MAX_FRAME_BYTES:
        raise ProtocolError("response exceeds JSON frame limit")
    remaining = memoryview(struct.pack("<I", len(data)) + data)
    while remaining:
        count = stream.write(remaining)
        if count is None or count <= 0:
            raise BrokenPipeError("unable to write JSON frame")
        remaining = remaining[count:]
    stream.flush()


class Worker:
    def __init__(self, writer):
        self.writer = writer
        self.write_lock = threading.Lock()
        self.state_lock = threading.Lock()
        self.tasks = queue.Queue(maxsize=64)
        self.cancellations = {}
        self.discovery = None
        self.disconnected = False

    def emit(self, identifier, event):
        with self.write_lock:
            if self.disconnected:
                return
            try:
                write_frame(self.writer, {"id": identifier, **event})
            except (OSError, ValueError):
                self.disconnected = True
                with self.state_lock:
                    for cancellation in self.cancellations.values():
                        cancellation.set()

    def error(self, identifier, code, error):
        self.emit(identifier, {"event": "error", "code": code, "message": str(error), "traceback": "".join(traceback.format_exception(error))})

    def execute(self, request, context):
        from lyflow_python.api import NodeCancelled, NodeError, resolve_params
        from lyflow_python.codec import DataCodec

        identifier = request.get("operatorId")
        if self.discovery is None or identifier not in self.discovery.operators:
            raise LookupError(f"Python operator not discovered: {identifier}")
        function, manifest, _ = self.discovery.operators[identifier]
        codec = DataCodec(request.get("requestDir"), context.check_cancelled)
        success = False
        try:
            inputs = request.get("inputs", {})
            ports = {port["name"]: port for port in manifest["inputs"]}
            if not isinstance(inputs, dict) or set(inputs) - ports.keys():
                raise NodeError("inputs contain undeclared ports")
            parameters = resolve_params(manifest["params"], request.get("params", {}))
            decoded_inputs = {}
            for name, spec in ports.items():
                if name not in inputs:
                    if spec.get("required", True):
                        raise NodeError(f"missing required input port: {name}")
                    continue
                try:
                    decoded_inputs[name] = codec.decode(inputs[name], spec["type"])
                except Exception as error:
                    raise NodeError(f"input port {name}: {error}") from error
            if getattr(function, "__lyflow_call_style__", None) == "dict":
                arguments = {"inputs": decoded_inputs, "params": parameters, "ctx": context}
            else:
                arguments = {**parameters, **decoded_inputs}
                signature = inspect.signature(function)
                if "ctx" in signature.parameters or any(p.kind == p.VAR_KEYWORD for p in signature.parameters.values()):
                    arguments["ctx"] = context
            context.check_cancelled()
            try:
                result = function(**arguments)
            except BaseException as error:
                if isinstance(error, NodeCancelled):
                    raise
                raise TaskFailure("python_dependency_missing" if isinstance(error, ImportError) else "python_exception", error) from error
            context.check_cancelled()
            try:
                expected = {port["name"]: port for port in manifest["outputs"]}
                if not isinstance(result, dict) or set(result) != set(expected):
                    raise NodeError(f"outputs must contain exactly the declared ports: {sorted(expected)}")
                outputs = {}
                for name, spec in expected.items():
                    try:
                        outputs[name] = codec.encode(result[name], spec["type"])
                    except Exception as error:
                        from lyflow_python.api import NodeCancelled
                        if isinstance(error, NodeCancelled):
                            raise
                        raise NodeError(f"output port {name}: {error}") from error
                context.check_cancelled()
            except BaseException as error:
                if isinstance(error, NodeCancelled):
                    raise
                raise TaskFailure("python_output_invalid", error) from error
            success = True
            return {"outputs": outputs}
        finally:
            codec.close(success=success)

    def run_tasks(self):
        while True:
            task = self.tasks.get()
            if task is None:
                return
            request, cancellation = task
            identifier = request["id"]
            try:
                if sys.version_info < (3, 11):
                    version = ".".join(str(part) for part in sys.version_info[:3])
                    raise TaskFailure("python_version_unsupported", RuntimeError(f"LyFlow Python nodes require Python 3.11 or newer; current interpreter is {version}"))
                from lyflow_python.api import Context, NodeCancelled
                context = Context(base_dir=request.get("baseDir", ""), thread_budget=request.get("threadBudget", 1),
                                  cancel_event=cancellation, emit=lambda event: self.emit(identifier, event))
                context.check_cancelled()
                if request["command"] == "discover":
                    from lyflow_python.discovery import Discovery
                    if self.discovery is None:
                        self.discovery = Discovery()
                    result = self.discovery.discover(request.get("packagePaths"), context.check_cancelled)
                else:
                    result = self.execute(request, context)
                context.check_cancelled()
                if result is not None:
                    self.emit(identifier, {"event": "result", **result})
            except BaseException as error:
                if cancellation.is_set() or type(error).__name__ == "NodeCancelled":
                    self.emit(identifier, {"event": "cancelled"})
                else:
                    if isinstance(error, TaskFailure):
                        code, error = error.code, error.error
                    else:
                        code = "python_dependency_missing" if isinstance(error, ImportError) else "python_operator_not_found" if isinstance(error, LookupError) else "python_input_invalid" if isinstance(error, Exception) else "python_exception"
                    self.error(identifier, code, error)
            finally:
                with self.state_lock:
                    if self.cancellations.get(identifier) is cancellation:
                        del self.cancellations[identifier]

    def serve(self, reader):
        executor = threading.Thread(target=self.run_tasks, name="lyflow-python-task", daemon=True)
        executor.start()
        try:
            while not self.disconnected:
                try:
                    request = read_frame(reader)
                except Exception as error:
                    self.error(None, "python_protocol_error", error)
                    return
                if request is None:
                    return
                identifier, command = request.get("id"), request.get("command")
                if isinstance(identifier, bool) or not isinstance(identifier, (str, int)):
                    self.error(None, "python_protocol_error", ProtocolError("request id must be a string or integer"))
                    continue
                if command == "cancel":
                    with self.state_lock:
                        cancellation = self.cancellations.get(identifier)
                        if cancellation is not None:
                            cancellation.set()
                    continue
                if command not in {"discover", "execute"}:
                    self.error(identifier, "python_protocol_error", ProtocolError(f"unknown command: {command}"))
                    continue
                with self.state_lock:
                    if identifier in self.cancellations:
                        duplicate = True
                    else:
                        duplicate = False
                        cancellation = threading.Event()
                        self.cancellations[identifier] = cancellation
                if duplicate:
                    self.error(identifier, "python_protocol_error", ProtocolError("duplicate active request id"))
                    continue
                try:
                    self.tasks.put_nowait((request, cancellation))
                except queue.Full:
                    with self.state_lock:
                        del self.cancellations[identifier]
                    self.error(identifier, "python_protocol_error", ProtocolError("worker task queue is full"))
        finally:
            with self.state_lock:
                for cancellation in self.cancellations.values():
                    cancellation.set()
            try:
                self.tasks.put_nowait(None)
            except queue.Full:
                pass
            executor.join(timeout=0.5)


def main():
    parser = argparse.ArgumentParser(description="LyFlow Python node worker")
    parser.add_argument("--pipe", required=True)
    args = parser.parse_args()
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    if __package__ in (None, ""):
        from winpipe import NamedPipe
    else:
        from .winpipe import NamedPipe
    with NamedPipe(args.pipe) as pipe:
        Worker(pipe).serve(pipe)


if __name__ == "__main__":
    main()
