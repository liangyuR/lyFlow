import argparse
import ctypes as C
import hashlib
import importlib.util
import json
from pathlib import Path
import socket
import subprocess
import threading
import uuid


ROOT = Path(__file__).resolve().parents[2]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def execute(cli, *arguments, success=True):
    result = subprocess.run([str(cli), *map(str, arguments)], cwd=ROOT, capture_output=True, text=True,
                            encoding="utf-8", errors="strict", timeout=120)
    require((result.returncode == 0) == success, f"{arguments}: exit={result.returncode}: {result.stderr}")
    return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]


class Cloud(C.Structure):
    _fields_ = [("count", C.c_uint32), ("total", C.c_uint32), ("flags", C.c_uint32), ("reserved", C.c_uint32),
                ("bounds", C.c_float * 6), ("xyz", C.c_void_p), ("intensity", C.c_void_p),
                ("normals", C.c_void_p), ("rgb", C.c_void_p), ("handle", C.c_void_p)]


class Tensor(C.Structure):
    _fields_ = [("rank", C.c_uint32), ("count", C.c_uint32), ("offset", C.c_uint64), ("total", C.c_uint64),
                ("shape", C.POINTER(C.c_int64)), ("data", C.c_void_p), ("handle", C.c_void_p)]


class Indices(C.Structure):
    _fields_ = [("count", C.c_uint32), ("total", C.c_uint32), ("source", C.c_uint64),
                ("data", C.c_void_p), ("handle", C.c_void_p)]


class Image(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ("width", "height", "channels", "depth", "level", "full_width",
                "full_height", "row_offset", "row_count", "row_bytes")] + [("data", C.c_void_p), ("handle", C.c_void_p)]


def digest(pointer, size):
    require(pointer or not size, "nonempty native array has null pointer")
    return hashlib.sha256(C.string_at(pointer, size)).hexdigest()


def payloads(core, path):
    lib = core.lib
    frame = C.c_void_p()
    require(core.text(lib.lyflow_frame_load(str(path).encode(), 1024**3, C.byref(frame))) == "", "load failed")
    try:
        lib.lyflow_frame_run_id.argtypes = [C.c_void_p]
        lib.lyflow_frame_run_id.restype = C.c_char_p
        run = lib.lyflow_frame_run_id(frame)
        lib.lyflow_output_info.argtypes = [C.c_char_p, C.c_char_p]
        lib.lyflow_output_info.restype = C.c_void_p
        result = {}
        for name, node, port, view_type, extra in (
            ("cloud", "source", "cloud", Cloud, (0,)),
            ("filtered", "filtered", "cloud", Cloud, (0,)),
            ("indices", "filtered", "indices", Indices, (0, 0)),
            ("image", "image", "image", Image, (0, 0, 0)),
            ("tensor", "tensor", "tensor", Tensor, (0, 0)),
        ):
            kind = "cloud" if view_type is Cloud else name
            getter = getattr(lib, f"lyflow_output_{kind}")
            getter.argtypes = [C.c_char_p] * 3 + [C.c_uint64 if kind in ("tensor", "indices") and i == 0 else C.c_uint32 for i in range(len(extra))] + [C.POINTER(view_type)]
            getter.restype = C.c_int
            free = getattr(lib, f"lyflow_{kind}_view_free")
            free.argtypes = [C.POINTER(view_type)]
            free.restype = None
            view = view_type()
            require(getter(run, node.encode(), port.encode(), *extra, C.byref(view)) == 0, f"missing real native {name}")
            try:
                if view_type is Cloud:
                    result[name] = {"count": view.count, "flags": view.flags, "xyz": digest(view.xyz, view.count * 12),
                                    "intensity": digest(view.intensity, view.count * 4) if view.flags & 1 else None}
                elif view_type is Tensor:
                    result[name] = {"shape": list(view.shape[:view.rank]), "data": digest(view.data, view.count * 4)}
                elif view_type is Image:
                    result[name] = {"shape": [view.width, view.height, view.channels, view.depth],
                                    "data": digest(view.data, view.row_count * view.row_bytes)}
                else:
                    result[name] = {"count": view.count, "data": digest(view.data, view.count * 4)}
                    require(view.source != 0, "indices native cloud reference lost")
            finally:
                free(C.byref(view))
        for node in ("message", "pose2d", "pose3d"):
            info = json.loads(core.text(lib.lyflow_output_info(run, node.encode())))
            require(len(info) == 1 and "value" in info[0], f"missing actual Record {node}")
            result[node] = info[0]["value"]
        require(result["cloud"]["count"] == 100000 and result["cloud"]["flags"] & 1, "real large cloud/channel missing")
        require(result["tensor"]["shape"] and result["image"]["shape"][0] > 0, "real image/tensor missing")
        return result
    finally:
        lib.lyflow_frame_free(frame)


def queued_recording(core, recording, destination):
    lib = core.lib
    for name, arguments in {
        "lyflow_session_create": [C.c_char_p, C.POINTER(C.c_void_p)],
        "lyflow_session_push": [C.c_void_p, C.c_void_p, C.c_int32],
        "lyflow_session_front": [C.c_void_p, C.c_int32, C.POINTER(C.c_void_p)],
        "lyflow_session_reset": [C.c_void_p], "lyflow_session_info": [C.c_void_p],
        "lyflow_frame_save": [C.c_void_p, C.c_char_p, C.c_int32],
    }.items():
        function = getattr(lib, name)
        function.argtypes = arguments
        function.restype = C.c_void_p
    lib.lyflow_session_free.argtypes = [C.c_void_p]
    lib.lyflow_session_free.restype = None
    session = C.c_void_p()
    frames = []
    front = C.c_void_p()
    original = json.loads((recording / "index.json").read_text(encoding="utf-8"))
    try:
        options = {"id": "p6-delivery", "capacity": 1, "maxBytes": 64 * 1024**2, "overflow": "drop_oldest",
                   "timestampOrder": "nondecreasing", "schemaChange": "reset"}
        require(core.text(lib.lyflow_session_create(json.dumps(options).encode(), C.byref(session))) == "", "session create failed")
        for row in original["frames"]:
            frame = C.c_void_p()
            require(core.text(lib.lyflow_frame_load(str(recording / row["file"]).encode(), 1024**3, C.byref(frame))) == "", "session frame load failed")
            frames.append(frame)
        first = json.loads(core.text(lib.lyflow_session_push(session, frames[0], 0)))
        second = json.loads(core.text(lib.lyflow_session_push(session, frames[1], 0)))
        require(first.get("accepted") and second.get("accepted"), f"session push rejected: {first}, {second}")
        require(int(second["sequence"]) > int(first["sequence"]), "session sequence is not monotonic")
        require(core.text(lib.lyflow_session_front(session, 0, C.byref(front))) == "" and front.value, "session front missing")
        metadata = json.loads(core.text(lib.lyflow_frame_metadata(front)))
        require(metadata["frameId"] == original["frames"][1]["metadata"]["frameId"], "bounded FIFO retained wrong frame")
        require(metadata["session"]["id"] == "p6-delivery" and metadata["session"]["sequence"] == second["sequence"], "queued frame lost session identity")
        original_metadata = json.loads(core.text(lib.lyflow_frame_metadata(frames[1])))
        require(original_metadata.get("session") is None, "session push mutated caller frame")
        epoch = metadata["session"]["epoch"]
        require(core.text(lib.lyflow_session_reset(session)) == "", "session reset failed")
        empty = C.c_void_p()
        require(core.text(lib.lyflow_session_front(session, 0, C.byref(empty))) == "" and not empty.value, "reset did not clear queue")
        reset_push = json.loads(core.text(lib.lyflow_session_push(session, frames[1], 0)))
        require(reset_push.get("accepted"), "session push after reset failed")
        reset_front = C.c_void_p()
        require(core.text(lib.lyflow_session_front(session, 1, C.byref(reset_front))) == "" and reset_front.value, "pop after reset failed")
        try:
            reset_metadata = json.loads(core.text(lib.lyflow_frame_metadata(reset_front)))
            require(reset_metadata["session"]["epoch"] != epoch, "reset reused prior session epoch")
        finally:
            lib.lyflow_frame_free(reset_front)
        destination.mkdir()
        archive = destination / "queued.lyframe"
        require(core.text(lib.lyflow_frame_save(front, str(archive).encode(), 0)) == "", "retained queued frame did not survive reset")
        require(core.text(lib.lyflow_frame_save(front, str(archive).encode(), 0)) != "", "archive save silently overwrote existing file")
        data = archive.read_bytes()
        index = {"type": "lyflow.Recording.v1", "schemaVersion": 1, "complete": True, "frames": [{"sequence": "0", "file": archive.name,
                 "bytes": str(len(data)), "sha256": hashlib.sha256(data).hexdigest(), "metadata": metadata}]}
        (destination / "index.json").write_text(json.dumps(index), encoding="utf-8")
        return metadata
    finally:
        if front.value:
            lib.lyflow_frame_free(front)
        for frame in frames:
            lib.lyflow_frame_free(frame)
        if session.value:
            lib.lyflow_session_free(session)


def bundle_source_contract(cli, examples, output):
    profile = (examples / "p5-scene.pcd").as_posix()
    graph = {"schemaVersion": 1, "id": "p6-bundle-source", "name": "P6 Bundle source replay", "nodes": [
        {"id": "scan", "op": "gap.read_scan", "opVersion": "2.0.0", "params": {
         "source": "files", "primaryFile": profile, "secondaryFile": profile, "removeOutliers": False,
         "unit": "m", "layout": "sensor", "fileFrame": "p6.fixture.file", "sensorFrame": "p6.fixture.sensor",
         "measurementFrame": "p6.measurement", "handedness": "right"}}], "edges": [], "outputs": {
        "scan": {"node": "scan", "port": "scan"},
        **{name: {"node": "scan", "port": f"scan.{name}"} for name in (
            "sensorPrimary", "sensorSecondary", "primary", "secondary", "merged")}}}
    path = output / "bundle-source.lyflow.json"
    path.write_text(json.dumps(graph), encoding="utf-8")
    recording = output / "bundle-recording"
    execute(cli, "record", path, "--recording", recording)
    original = execute(cli, "replay", recording)[0]
    restored = execute(cli, "replay", recording, "--recompute", "--no-cache")[0]
    require(restored["status"] == "ok", "Bundle source recompute injected virtual fields as target ports")
    require(original["outputs"]["scan"]["type"] == restored["outputs"]["scan"]["type"] == "Bundle<gap.ScanPair.v2>",
            "real versioned native Bundle source was not recorded")
    fields = ("sensorPrimary", "sensorSecondary", "primary", "secondary", "merged")
    for name in fields:
        before, after = original["outputs"][name], restored["outputs"][name]
        require(before["type"] == after["type"] == "Profile", "Bundle field native type changed")
        require(before["elementCount"] == after["elementCount"] > 0, "Bundle field payload lost")
        require(before["value"]["descriptor"] == after["value"]["descriptor"], "Profile declaration changed during replay")
        require(before["value"]["validCount"] == after["value"]["validCount"] > 0, "Profile validity was lost")
        descriptor = before["value"]["descriptor"]
        require(descriptor["unit"] == "m" and descriptor["sourceCount"] >= before["elementCount"],
                "Profile lost physical units or source slot count")
        require(descriptor["plane"] == ("XZ" if name.startswith("sensor") else "XY") and
                descriptor["frame"] == ("p6.fixture.sensor" if name.startswith("sensor") else "p6.measurement"),
                "Profile lost its explicit sensor/measurement frame or analysis plane")
    virtual = [entry for entry in original["metadata"]["entries"] if entry.get("virtual") is True]
    require({entry["port"] for entry in virtual} ==
            {f"scan.{name}" for name in (*fields, "mergedProvenance", "info")}, "Bundle virtual-port identity missing")
    target = {"schemaVersion": 1, "id": "p6-bundle-field-binding", "name": "P6 explicit native Bundle source", "nodes": [
        {"id": "source", "op": "profile.input", "opVersion": "1.0.0", "params": {"source": "injected"}},
        {"id": "cloud", "op": "profile.to_cloud", "opVersion": "1.0.0", "params": {"unit": "m"}},
        {"id": "stats", "op": "cloud.statistics", "params": {"frame": "p6.measurement"}}],
        "edges": [
            {"id": "profile-cloud", "from": {"node": "source", "port": "profile"}, "to": {"node": "cloud", "port": "profile"}},
            {"id": "cloud-stats", "from": {"node": "cloud", "port": "cloud"}, "to": {"node": "stats", "port": "cloud"}}],
        "outputs": {"statistics": {"node": "stats", "port": "statistics"},
                    "profile": {"node": "source", "port": "profile"},
                    "cloud": {"node": "cloud", "port": "cloud"}}}
    target_path = output / "bundle-target.lyflow.json"
    target_path.write_text(json.dumps(target), encoding="utf-8")
    explicit = execute(cli, "replay", recording, "--graph", target_path, "--bind", "scan.scan.merged=source.profile", "--no-cache")[0]
    require(explicit["status"] == "ok", "explicit native Bundle field source binding rejected")
    require(explicit["outputs"]["profile"]["type"] == "Profile" and explicit["outputs"]["cloud"]["type"] == "PointCloud",
            "explicit Profile-to-PointCloud boundary was not preserved")
    require(explicit["outputs"]["profile"]["value"]["descriptor"] == original["outputs"]["merged"]["value"]["descriptor"],
            "explicit replay discarded the actual Profile descriptor")
    require(explicit["outputs"]["statistics"]["value"]["data"]["count"] == original["outputs"]["merged"]["elementCount"],
            "explicit Bundle field binding did not feed the real target port")
    wrong = json.loads(json.dumps(target))
    wrong["nodes"][0] = {"id": "source", "op": "gen.synthetic", "params": {"pointCount": 1}}
    wrong["nodes"] = [node for node in wrong["nodes"] if node["id"] != "cloud"]
    wrong["edges"] = [{"id": "wrong-cloud-stats", "from": {"node": "source", "port": "cloud"},
                       "to": {"node": "stats", "port": "cloud"}}]
    wrong["outputs"] = {"statistics": {"node": "stats", "port": "statistics"}}
    wrong_path = output / "bundle-wrong-target.lyflow.json"
    wrong_path.write_text(json.dumps(wrong), encoding="utf-8")
    execute(cli, "replay", recording, "--graph", wrong_path, "--bind", "scan.scan.merged=source.cloud", "--no-cache", success=False)
    return {"bundleType": "Bundle<gap.ScanPair.v2>", "nativeFieldType": "Profile", "profileFields": list(fields),
            "mergedSlots": original["outputs"]["merged"]["elementCount"], "recomputePassed": True,
            "explicitProfileToCloudPassed": True, "mismatchedBindingRejected": True}



def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--examples", type=Path, default=ROOT / "examples")
    args = parser.parse_args()
    examples = args.examples.resolve(strict=True)
    require(examples.is_dir(), "examples must be a directory")
    output = args.output_dir.resolve() / f"p6-delivery-{uuid.uuid4().hex}"
    output.mkdir(parents=True)
    recording = output / "recording"
    fixture = examples / "p6-delivery_demo.lyflow.json"
    execute(args.cli, "record", fixture, "--recording", recording, "--count", 2, "--interval-ms", 10,
            "--frame", "world", "--calibration-version", "fixture-v1")
    index = json.loads((recording / "index.json").read_text(encoding="utf-8"))
    require(index["complete"] and len(index["frames"]) == 2, "recording did not complete")
    require(all(row["metadata"]["context"]["timestampKind"] == "host_capture" for row in index["frames"]), "clock kind lost")
    readonly = execute(args.cli, "replay", recording)
    require(len(readonly) == 2 and all(row["mode"] == "recorded" for row in readonly), "readonly replay ran algorithm")
    recomputed = execute(args.cli, "replay", recording, "--recompute", "--no-cache")
    require(len(recomputed) == 2 and all(row["status"] == "ok" for row in recomputed), "source frame injection failed")
    selected = execute(args.cli, "replay", recording, "--from-timestamp-ns", index["frames"][1]["metadata"]["timestampNs"])
    require(len(selected) == 1 and selected[0]["sequence"] == "1", "timestamp selection failed")
    execute(args.cli, "replay", recording, "--graph", fixture, success=False)

    spec = importlib.util.spec_from_file_location("receiver", examples / "p6-delivery_receiver.py")
    receiver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(receiver)
    core = receiver.Core(args.core)
    expected = payloads(core, recording / index["frames"][0]["file"])
    queued = output / "queued-recording"
    queued_metadata = queued_recording(core, recording, queued)
    destination = output / "received"
    destination.mkdir()
    errors = []
    received = []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(2)
        listener.settimeout(10)
        def serve():
            try:
                for _ in range(3):
                    connection, _ = listener.accept()
                    with connection:
                        received.append(receiver.receive(connection, destination, core, 1024**3, 10, "test/results"))
            except BaseException as error:
                errors.append(error)
        server = threading.Thread(target=serve)
        server.start()
        try:
            receipts = execute(args.cli, "publish", recording, "--endpoint", f"tcp://127.0.0.1:{listener.getsockname()[1]}",
                               "--topic", "test/results", "--timeout-ms", 10000)
            queue_receipts = execute(args.cli, "publish", queued, "--endpoint", f"tcp://127.0.0.1:{listener.getsockname()[1]}",
                                     "--topic", "test/results", "--timeout-ms", 10000)
        finally:
            server.join(15)
        require(not server.is_alive() and not errors, f"receiver failed: {errors}")
    require(len(receipts) == 2 and all(row["acknowledged"] for row in receipts), "publication missing ACK")
    require(len(received) == 3 and payloads(core, Path(received[0]["file"])) == expected, "native arrays/records changed in delivery")
    require(queue_receipts[0]["session"] == queued_metadata["session"], "endpoint ACK lost actual FIFO epoch/sequence")
    require(not list(destination.glob("*.partial")), "receiver leaked temporary files")

    for kind, node in (("string", "message.value"), ("poses2d", "pose2d.pose"), ("poses3d", "pose3d.pose")):
        path = output / f"{kind}.jsonl"
        command = ("log", recording, "--node", node, "--output", path, "--log-kind", kind, "--log-archive", "none")
        execute(args.cli, *command)
        content = path.read_bytes()
        execute(args.cli, *command, success=False)
        require(path.read_bytes() == content, "log conflict overwrote an existing file")
        execute(args.cli, *command, "--conflict", "append")
        require(path.read_bytes() == content * 2 and len(content.splitlines()) == 2, "append/log boundaries failed")
        values = [json.loads(line) for line in content.splitlines()]
        require(all(row["calibrationVersion"] == "fixture-v1" and row["frame"] == "world" for row in values), "log metadata lost")
    execute(args.cli, "log", recording, "--node", "pose3d.pose", "--output", output / "euler.log", "--log-kind", "poses3d",
            "--length-unit", "mm", "--pose-format", "ZYX", "--angle-unit", "deg")
    require(len(list(output.glob("euler.log.day-*"))) == 1, "daily UTC log archive missing")
    injected_graph = json.loads(fixture.read_text(encoding="utf-8"))
    injected_graph["nodes"].append({"id": "concat", "op": "cloud.group_concat", "params": {"frame": "world"}})
    injected_graph["nodes"].append({"id": "stats", "op": "cloud.statistics", "params": {"frame": "world"}})
    injected_graph["edges"].extend([
        {"id": "source-concat", "from": {"node": "source", "port": "cloud"}, "to": {"node": "concat", "port": "a"}},
        {"id": "concat-stats", "from": {"node": "concat", "port": "cloud"}, "to": {"node": "stats", "port": "cloud"}}])
    injected_graph["outputs"].update({"statistics": {"node": "stats", "port": "statistics"}, "height": {"node": "stats", "port": "height"}, "boxTransform": {"node": "stats", "port": "pose"}})
    graph_path = output / "input-injection.lyflow.json"
    graph_path.write_text(json.dumps(injected_graph), encoding="utf-8")
    injected_cloud = output / "injected.pcd"
    injected_cloud.write_text("VERSION .7\nFIELDS x y z intensity\nSIZE 4 4 4 4\nTYPE F F F F\nCOUNT 1 1 1 1\nWIDTH 3\nHEIGHT 1\nPOINTS 3\nDATA ascii\n0 0 .25 1\n.1 0 .25 2\n0 .1 .25 3\n", encoding="ascii")
    input_recording = output / "injected-recording"
    execute(args.cli, "record", graph_path, "--recording", input_recording, "--base-dir", examples,
            "--input", f"concat.b={injected_cloud}")
    original_result = execute(args.cli, "replay", input_recording)[0]
    replayed_result = execute(args.cli, "replay", input_recording, "--recompute", "--no-cache")[0]
    for row in (original_result, replayed_result):
        require(row["outputs"]["sourceCloud"]["elementCount"] == 100000, "native source changed")
        require(row["outputs"]["statistics"]["value"]["data"]["count"] == 100003, "non-source injected input was lost during replay")
        require(row["outputs"]["height"]["type"] == "Measurement" and row["outputs"]["boxTransform"]["type"] == "Transform", "small native types lost during archive load")
    require(original_result["metadata"]["frame"] == "" and original_result["metadata"]["calibrationVersion"] is None, "unknown coordinate/calibration fabricated")
    matching = json.loads((examples / "p2-matching-fast.lyflow.json").read_text(encoding="utf-8"))
    matching["nodes"].extend([
        {"id": "visibleStats", "op": "cloud.statistics", "params": {"frame": "camera"}},
        {"id": "declaredId", "op": "value.field", "params": {"pointer": "/cloudId"}},
        {"id": "actualId", "op": "value.field", "params": {"pointer": "/sourceCloudId"}},
        {"id": "identity", "op": "value.compare", "params": {"operation": "equal"}}])
    for source_node, source_port, target_node, target_port in [
        ("visible", "cloud", "visibleStats", "cloud"), ("visible", "quality", "declaredId", "record"),
        ("visibleStats", "statistics", "actualId", "record"), ("declaredId", "value", "identity", "a"),
        ("actualId", "value", "identity", "b")]:
        matching["edges"].append({"id": f"{source_node}-{target_node}", "from": {"node": source_node, "port": source_port}, "to": {"node": target_node, "port": target_port}})
    matching["outputs"]["nativeIdentity"] = {"node": "identity", "port": "value"}
    matching_graph = output / "matching-identity.lyflow.json"
    matching_graph.write_text(json.dumps(matching), encoding="utf-8")
    matching_recording = output / "matching-recording"
    execute(args.cli, "record", matching_graph, "--recording", matching_recording, "--base-dir", examples)
    maps = []
    for node, ports in (("cad", ("cloud", "metadata")), ("scene", ("cloud", "metadata")), ("template", ("model", "template")),
                        ("visible", ("cloud", "indices", "groups", "cloudIndices", "cloudGroups", "quality"))):
        for port in ports:
            maps.extend(("--bind", f"{node}.{port}={node}.{port}"))
    restored_match = execute(args.cli, "replay", matching_recording, "--recompute", "--no-cache", *maps)[0]
    require(restored_match["status"] == "ok", "loaded Template3D/model rejected by real global matching consumer")
    require(restored_match["outputs"]["nativeIdentity"]["value"]["data"]["value"] is True,
            "loaded ViewSampling.cloudId disagrees with actual downstream cloud.statistics/value.field/value.compare")
    bundle_contract = bundle_source_contract(args.cli, examples, output)
    execute(args.cli, "run", fixture)
    print(json.dumps({"ok": True, "type": "lyflow.P6DeliveryAcceptance.v1", "output": str(output),
                      "frames": 2, "nativePayloads": list(expected), "receipts": len(receipts),
                      "bundleSource": bundle_contract, "noSkipped": True}))


if __name__ == "__main__":
    main()
