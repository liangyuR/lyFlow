import io
import json
import queue
import struct
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lyflow_python import Context, Image, Measurement, NodeCancelled, NodeError, PointCloud, Record, node
from lyflow_python.api import resolve_params
from lyflow_python.codec import DataCodec
from lyflow_python.discovery import Discovery
from lyflow_python.worker import MAX_FRAME_BYTES, ProtocolError, Worker, read_frame, write_frame


def source(identifier, body="return {'out': Record({'ok': True})}", *, inputs="{}", outputs="{'out':'Record'}", params="{}"):
    return (
        "from lyflow_python import node, Record, PointCloud, Image, Measurement\n"
        f"@node(id={identifier!r}, version='1.0.0', label='测试', inputs={inputs}, outputs={outputs}, params={params})\n"
        "def run(ctx, **kwargs):\n" + "\n".join("    " + line for line in body.splitlines()) + "\n"
    )


class SDKTests(unittest.TestCase):
    def test_manifest_signature_defaults_and_context_contract(self):
        @node(id="py.test.amount", version="1.0.0", label="数值", outputs={"value": "Measurement"},
              params={"amount": {"type": "float", "default": 2, "min": 0, "max": 4}})
        def amount(inputs, params, ctx):
            return {"value": Measurement(params["amount"])}

        manifest = amount.__lyflow_node__
        self.assertEqual(manifest["params"], [{"name": "amount", "type": "float", "default": 2, "min": 0, "max": 4}])
        self.assertEqual(manifest["capabilities"], {"deterministic": False, "cancellable": True, "previewable": False})
        self.assertEqual(amount.__lyflow_call_style__, "dict")
        self.assertEqual(resolve_params(manifest["params"], {}), {"amount": 2})
        for supplied in [{"bad": 1}, {"amount": True}, {"amount": 5}, {"amount": float("nan")}]:
            with self.subTest(supplied=supplied), self.assertRaises(NodeError):
                resolve_params(manifest["params"], supplied)
        with self.assertRaisesRegex(NodeError, "signature"):
            node(id="py.test.bad", version="1.0.0", label="坏签名", inputs={"cloud": "PointCloud"}, outputs={"out": "Record"})(lambda: None)
        with self.assertRaisesRegex(NodeError, "unsupported type"):
            node(id="py.test.bad", version="1.0.0", label="不支持张量", outputs={"out": "Tensor"})
        events, cancellation = [], threading.Event()
        ctx = Context(base_dir="C:/data", thread_budget=3, emit=events.append, cancel_event=cancellation)
        ctx.log("中文日志")
        ctx.progress(0.5, "一半")
        self.assertEqual((ctx.thread_budget, len(events), events[1]["ratio"]), (3, 2, 0.5))
        with self.assertRaises(NodeError):
            ctx.progress(1.5)
        cancellation.set()
        with self.assertRaises(NodeCancelled):
            ctx.check_cancelled()

    def test_discovery_file_package_isolation_reloads_and_duplicate_disablement(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "good.py").write_text(source("py.test.good"), encoding="utf-8")
            (root / "bad.py").write_text("import lyflow_missing_dependency_123\n", encoding="utf-8")
            (root / "exit.py").write_text("raise SystemExit('import exit')\n", encoding="utf-8")
            (root / "interrupt.py").write_text("raise KeyboardInterrupt('import interrupt')\n", encoding="utf-8")
            package = root / "package"
            package.mkdir()
            (package / "__init__.py").write_text("from .implementation import run\n", encoding="utf-8")
            (package / "implementation.py").write_text(source("py.test.packaged"), encoding="utf-8")
            (package / "unexported.py").write_text(source("py.test.hidden"), encoding="utf-8")
            discovery = Discovery()
            container = discovery.discover([str(root)])
            self.assertEqual(container["operators"], [])
            self.assertEqual(len(container["problems"]), 1)
            self.assertEqual(container["problems"][0]["path"], str(root))
            self.assertIn("requires __init__.py", container["problems"][0]["message"])
            paths = [root / name for name in ["good.py", "bad.py", "exit.py", "interrupt.py", "package"]]
            result = discovery.discover([str(path) for path in paths])
            self.assertEqual([op["id"] for op in result["operators"]], ["py.test.good", "py.test.packaged"])
            self.assertEqual(len(result["problems"]), 3)
            for exception in ["ModuleNotFoundError", "SystemExit", "KeyboardInterrupt"]:
                self.assertTrue(any(exception in issue["traceback"] for issue in result["problems"]), exception)
            self.assertEqual(result["numpyVersion"], np.__version__)
            (root / "duplicate.py").write_text(source("py.test.good"), encoding="utf-8")
            conflicted = discovery.discover([str(path) for path in [*paths, root / "duplicate.py", root / "good.py"]])
            self.assertEqual([op["id"] for op in conflicted["operators"]], ["py.test.packaged"])
            self.assertEqual(sum("duplicate" in issue["message"] for issue in conflicted["problems"]), 2)

    def test_value_shapes_readonly_inputs_and_editable_copies(self):
        cloud = PointCloud(np.zeros((3, 3), np.float32), intensity=np.ones(3, np.float32),
                           normals=np.ones((3, 3), np.float32), rgb=np.zeros((3, 3), np.uint8))._freeze()
        with self.assertRaises(ValueError):
            cloud.xyz[0, 0] = 3
        with self.assertRaisesRegex(NodeError, "read-only"):
            cloud.xyz = np.ones((3, 3), np.float32)
        copied = cloud.copy()
        copied.xyz[0, 0] = 7
        self.assertEqual((copied.xyz[0, 0], cloud.xyz[0, 0]), (7, 0))
        record = Record({"nested": [{"n": 1}]}, "test")._freeze()
        with self.assertRaises(TypeError):
            record.data["nested"][0]["n"] = 2
        copy = record.copy()
        copy.data["nested"][0]["n"] = 2
        self.assertEqual(record.data["nested"][0]["n"], 1)
        measurement = Measurement(np.float32(2), "mm")._freeze()
        copy = measurement.copy()
        copy.value = 3
        self.assertEqual(measurement.value, 2)
        for name, action in [
            ("xyz类型", lambda: PointCloud(np.zeros((3, 3), np.float64))),
            ("xyz形状", lambda: PointCloud(np.zeros((3, 2), np.float32))),
            ("强度数量", lambda: PointCloud(np.zeros((3, 3), np.float32), intensity=np.zeros(2, np.float32))),
            ("RGB类型", lambda: PointCloud(np.zeros((3, 3), np.float32), rgb=np.zeros((3, 3), np.float32))),
            ("图像通道", lambda: Image(np.zeros((2, 3, 2), np.uint8))),
            ("图像位深", lambda: Image(np.zeros((2, 3), np.int32))),
            ("图像空尺寸", lambda: Image(np.zeros((0, 3), np.uint8))),
            ("量测非有限", lambda: Measurement(float("nan"))),
        ]:
            with self.subTest(name=name), self.assertRaises(NodeError):
                action()

    def test_codec_roundtrips_empty_cloud_channels_images_and_json_values(self):
        with tempfile.TemporaryDirectory() as temporary:
            values = [
                ("PointCloud", PointCloud(np.zeros((0, 3), np.float32))),
                ("PointCloud", PointCloud(np.arange(12, dtype=np.float32).reshape(4, 3), np.ones(4, np.float32),
                                          np.zeros((4, 3), np.float32), np.full((4, 3), 123, np.uint8))),
                ("Measurement", Measurement(1.25, "mm", nominal=1, lower=0, upper=2)),
                ("Measurement", Measurement(None, message="没测出")),
                ("Record", Record({"中文": [1, True, None]}, "test.record")),
            ]
            for dtype in [np.uint8, np.uint16, np.float32]:
                for channels in [None, 1, 3, 4]:
                    shape = (2, 3) if channels is None else (2, 3, channels)
                    values.append(("Image", Image(np.arange(np.prod(shape), dtype=dtype).reshape(shape)[:, ::-1])))
            for kind, value in values:
                with self.subTest(kind=kind, shape=getattr(getattr(value, "pixels", None), "shape", None)):
                    writer, reader = DataCodec(temporary), DataCodec(temporary)
                    try:
                        dto = writer.encode(value, kind)
                        decoded = reader.decode(dto, kind)
                        if kind == "Image":
                            np.testing.assert_array_equal(decoded.pixels, value.pixels)
                            self.assertFalse(decoded.pixels.flags.writeable)
                        elif kind == "PointCloud":
                            np.testing.assert_array_equal(decoded.xyz, value.xyz)
                            if value.rgb is not None:
                                np.testing.assert_array_equal(decoded.rgb, value.rgb)
                        elif kind == "Measurement":
                            self.assertEqual((decoded.value, decoded.ok), (value.value, value.ok))
                        else:
                            self.assertEqual(decoded.record_type, "test.record")
                            self.assertEqual(decoded.copy().data, value.data)
                    finally:
                        reader.close()
                        writer.close()
            self.assertEqual(list(Path(temporary).iterdir()), [], "映射关闭后Windows可删除所有输出文件")

    def test_codec_rejects_escaping_paths_wrong_sizes_and_cleans_failed_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            request = root / "request"
            request.mkdir()
            outside = root / "outside.bin"
            outside.write_bytes(b"\0" * 12)
            inside = request / "input.bin"
            inside.write_bytes(b"\0" * 12)
            valid = {"file": str(inside), "shape": [1, 3], "dtype": "f32"}
            codec = DataCodec(str(request))
            try:
                for name, dto in [
                    ("目录外", {**valid, "file": str(outside)}),
                    ("相对路径", {**valid, "file": "input.bin"}),
                    ("路径穿越", {**valid, "file": str(request / ".." / "outside.bin")}),
                    ("长度不符", {**valid, "shape": [2, 3]}),
                    ("负维度", {**valid, "shape": [-1, 3]}),
                    ("布尔维度", {**valid, "shape": [True, 3]}),
                    ("未知类型", {**valid, "dtype": "f64"}),
                ]:
                    with self.subTest(name=name), self.assertRaises(NodeError):
                        codec.read_array(dto)
                codec.decode({"type": "PointCloud", "xyz": valid}, "PointCloud")
                codec.encode(Image(np.zeros((2, 2), np.uint8)), "Image")
                with self.assertRaises(NodeError):
                    codec.encode(Record({}), "Measurement")
            finally:
                codec.close()
            inside.unlink()
            self.assertEqual(list(request.iterdir()), [], "失败路径也释放输入映射并清理输出")

    def test_framing_partial_io_unicode_and_malformed_frames(self):
        class ShortIO(io.BytesIO):
            def read(self, size=-1):
                return super().read(min(size, 3))

            def write(self, data):
                return super().write(data[:2])

        stream = ShortIO()
        payload = {"id": "中文", "event": "result", "value": 3}
        write_frame(stream, payload)
        stream.seek(0)
        self.assertEqual(read_frame(stream), payload)
        self.assertIsNone(read_frame(stream))
        for raw in [b"\x01", struct.pack("<I", 0), struct.pack("<I", MAX_FRAME_BYTES + 1),
                    struct.pack("<I", 3) + b"{}", struct.pack("<I", 1) + b"\xff", struct.pack("<I", 2) + b"[]"]:
            with self.subTest(raw=raw), self.assertRaises(ProtocolError):
                read_frame(io.BytesIO(raw))


class WorkerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        class Endpoint:
            def __init__(self):
                self.incoming = queue.Queue()
                self.buffer = b""
                self.closed = False
                self.peer = None

            def read(self, size):
                if not self.buffer:
                    self.buffer = self.incoming.get(timeout=10)
                    if not self.buffer:
                        return b""
                result, self.buffer = self.buffer[:size], self.buffer[size:]
                return result

            def write(self, value):
                if self.closed:
                    raise BrokenPipeError()
                self.peer.incoming.put(bytes(value))
                return len(value)

            def flush(self):
                pass

            def close(self):
                if not self.closed:
                    self.closed = True
                    self.peer.incoming.put(b"")

        self.client, self.server = Endpoint(), Endpoint()
        self.client.peer, self.server.peer = self.server, self.client
        self.worker = Worker(self.server)
        self.thread = threading.Thread(target=self.worker.serve, args=(self.server,), daemon=True)
        self.thread.start()

    def tearDown(self):
        self.client.close()
        self.thread.join(timeout=3)
        self.server.close()
        self.temporary.cleanup()
        self.assertFalse(self.thread.is_alive(), "worker主线程应在管道关闭后退出")

    def request(self, request):
        write_frame(self.client, request)
        return self.until_terminal(request["id"])

    def until_terminal(self, identifier):
        events = []
        while True:
            event = read_frame(self.client)
            self.assertIsNotNone(event)
            self.assertEqual(event["id"], identifier)
            events.append(event)
            if event["event"] in {"result", "error", "cancelled"}:
                return events

    def discover(self, *files):
        return self.request({"id": "discover", "command": "discover", "packagePaths": [str(file) for file in files]})[-1]

    def execution(self, identifier, **extra):
        return {"id": identifier, "command": "execute", "operatorId": "py.test.run", "requestDir": str(self.root),
                "inputs": {}, "params": {}, "baseDir": str(self.root), "threadBudget": 2, **extra}

    def test_discover_execute_progress_defaults_and_exact_output_ports(self):
        file = self.root / "node.py"
        file.write_text("from pathlib import Path\nPath(__file__).with_suffix('.imported').write_text('imported')\n" + source("py.test.run", "ctx.log('中文日志')\nctx.progress(0.5, '处理中')\nreturn {'out': Record({'amount': kwargs['amount'], 'threads': ctx.thread_budget, 'base': str(ctx.base_dir)})}",
                               params="{'amount':{'type':'float','default':2}}"), encoding="utf-8")
        with patch.object(sys, "version_info", (3, 10, 99)):
            unsupported = self.discover(file)
        self.assertEqual(unsupported["code"], "python_version_unsupported")
        self.assertIn("Python 3.11", unsupported["message"])
        self.assertIn("3.10.99", unsupported["message"])
        self.assertIn("RuntimeError", unsupported["traceback"])
        self.assertFalse(file.with_suffix(".imported").exists(), "低版本解释器不得导入节点")
        self.assertEqual(len(self.discover(file)["operators"]), 1)
        self.assertTrue(file.with_suffix(".imported").is_file())
        events = self.request(self.execution("execute"))
        self.assertEqual([event["event"] for event in events], ["log", "progress", "result"])
        self.assertEqual(events[-1]["outputs"]["out"]["data"], {"amount": 2, "threads": 2, "base": str(self.root)})
        for body, message in [("return {'wrong': Record({})}", "declared ports"), ("return {'out': Measurement(1)}", "requires Record")]:
            with self.subTest(body=body):
                file.write_text(source("py.test.run", body), encoding="utf-8")
                self.discover(file)
                error = self.request(self.execution("bad"))[-1]
                self.assertEqual(error["code"], "python_output_invalid")
                self.assertIn(message, error["message"])

    def test_function_failure_traceback_and_readonly_mmap_cleanup(self):
        file = self.root / "node.py"
        array_file = self.root / "input.bin"
        closed, terminals = [], []
        original_close, original_emit = DataCodec.close, self.worker.emit

        def close(codec, success=False):
            maps, outputs = list(codec.maps), list(codec.outputs)
            original_close(codec, success=success)
            closed.append((len(maps), len(outputs), all(mapping._mmap.closed for mapping in maps), all(not path.exists() for path in outputs)))

        def emit(identifier, event):
            if event["event"] in {"result", "error", "cancelled"} and identifier == "fail":
                terminals.append(list(closed))
            original_emit(identifier, event)

        cases = [
            ("只读输入", "kwargs['cloud'].xyz[0,0] = 9", "{'out':'Record'}", "python_exception", "read-only"),
            ("SystemExit", "raise SystemExit('node exit')", "{'out':'Record'}", "python_exception", "SystemExit: node exit"),
            ("KeyboardInterrupt", "raise KeyboardInterrupt('node interrupt')", "{'out':'Record'}", "python_exception", "KeyboardInterrupt: node interrupt"),
            ("缺失依赖", "import lyflow_missing_runtime_dependency_123", "{'out':'Record'}", "python_dependency_missing", "ModuleNotFoundError"),
            ("部分输出后失败", "return {'image': Image(kwargs['cloud'].xyz), 'out': Measurement(1)}", "{'image':'Image','out':'Record'}", "python_output_invalid", "output port out"),
        ]
        with patch.object(DataCodec, "close", close), patch.object(self.worker, "emit", emit):
            for name, body, outputs, code, diagnostic in cases:
                with self.subTest(name=name):
                    closed.clear()
                    terminals.clear()
                    np.zeros((2, 3), np.float32).tofile(array_file)
                    file.write_text(source("py.test.run", body, inputs="{'cloud':'PointCloud'}", outputs=outputs), encoding="utf-8")
                    self.assertEqual(len(self.discover(file)["operators"]), 1)
                    event = self.request(self.execution("fail", inputs={"cloud": {"type": "PointCloud", "xyz": {"file": str(array_file), "shape": [2, 3], "dtype": "f32"}}}))[-1]
                    self.assertEqual(event["code"], code)
                    if code != "python_output_invalid":
                        self.assertIn(str(file).casefold(), event["traceback"].casefold())
                    self.assertIn(diagnostic, event["traceback"])
                    self.assertEqual(terminals, [[(1, int(code == "python_output_invalid"), True, True)]], "错误帧发送前关闭全部映射并删除失败输出")
                    array_file.unlink()
                    self.assertEqual(list(self.root.glob("out-*.bin")), [])
            file.write_text(source("py.test.run"), encoding="utf-8")
            self.assertEqual(len(self.discover(file)["operators"]), 1)
            self.assertEqual(self.request(self.execution("recovered"))[-1]["event"], "result")

    def test_shipped_dict_signature_examples_execute_z_offset_and_image_metrics(self):
        examples = Path(__file__).resolve().parents[1] / "examples"
        discovered = self.discover(examples / "offset.py", examples / "image_metrics")
        self.assertEqual([op["id"] for op in discovered["operators"]], ["py.example.image_metrics", "py.example.offset"])
        self.assertEqual(discovered["problems"], [])
        xyz = np.array([[1, 2, 3], [4, 5, 6]], dtype=np.float32)
        cloud_file = self.root / "example-cloud.bin"
        xyz.tofile(cloud_file)
        cloud_result = self.request(self.execution("offset", operatorId="py.example.offset", params={"offset": 2.5},
            inputs={"cloud": {"type": "PointCloud", "xyz": {"file": str(cloud_file), "shape": [2, 3], "dtype": "f32"}}}))[-1]
        self.assertEqual(cloud_result["event"], "result", cloud_result)
        changed = np.fromfile(cloud_result["outputs"]["cloud"]["xyz"]["file"], dtype="<f4").reshape(2, 3)
        expected = xyz.copy()
        expected[:, 2] += 2.5
        np.testing.assert_array_equal(changed, expected)
        np.testing.assert_array_equal(np.fromfile(cloud_file, dtype="<f4").reshape(2, 3), xyz)

        image = np.arange(18, dtype=np.uint8).reshape(2, 3, 3)
        image_file = self.root / "example-image.bin"
        image.tofile(image_file)
        image_result = self.request(self.execution("image", operatorId="py.example.image_metrics", params={"gain": 2},
            inputs={"image": {"type": "Image", "pixels": {"file": str(image_file), "shape": [2, 3, 3], "dtype": "u8"}}}))[-1]
        self.assertEqual(image_result["event"], "result", image_result)
        outputs = image_result["outputs"]
        self.assertEqual(outputs["mean"]["value"], float(image.mean() * 2))
        self.assertEqual(outputs["info"]["recordType"], "py.image.metrics")
        np.testing.assert_array_equal(np.fromfile(outputs["image"]["pixels"]["file"], dtype="<f4").reshape(2, 3, 3), image.astype(np.float32) * 2)

    def test_cancel_is_read_while_function_runs_and_following_task_survives(self):
        file = self.root / "node.py"
        array_file = self.root / "cancel-input.bin"
        np.zeros((2, 3), np.float32).tofile(array_file)
        file.write_text(source("py.test.run", "import time\nctx.log('started')\nwhile True:\n    ctx.check_cancelled()\n    time.sleep(0.005)", inputs="{'cloud':'PointCloud'}"), encoding="utf-8")
        self.discover(file)
        write_frame(self.client, self.execution("long", inputs={"cloud": {"type": "PointCloud", "xyz": {"file": str(array_file), "shape": [2, 3], "dtype": "f32"}}}))
        self.assertEqual(read_frame(self.client)["message"], "started")
        write_frame(self.client, self.execution("missing", operatorId="py.test.missing"))
        write_frame(self.client, {"id": "long", "command": "cancel"})
        self.assertEqual(self.until_terminal("long")[-1]["event"], "cancelled")
        array_file.unlink()
        self.assertEqual(self.until_terminal("missing")[-1]["code"], "python_operator_not_found")


if __name__ == "__main__":
    unittest.main()
