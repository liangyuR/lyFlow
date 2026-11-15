import json

import numpy as np

from lyflow_python import NodeError, Record, Tensor, node, poses, scalar


def _poses(record, dimension=None):
    kinds = {"vision.Poses2D.v1": 2, "vision.Poses3D.v1": 3}
    actual = kinds.get(record.record_type)
    if actual is None or (dimension is not None and actual != dimension):
        raise NodeError("expected a standard vision.Poses2D.v1 or vision.Poses3D.v1 collection")
    return actual


@node(id="py.p6_adapters.offset_poses3d", version="1.0.0", label="三维工件位姿偏移",
      inputs={"poses": "Record"}, outputs={"poses": "Record"},
      params={"offset": {"type": "vec3f", "default": [0, 0, 0], "unit": "m"},
              "local": {"type": "bool", "default": False}},
      keywords=["位姿", "偏移", "pose", "offset", "local", "world"])
def offset_poses3d(inputs, params, ctx):
    _poses(inputs["poses"], 3)
    result = inputs["poses"].copy()
    offset = np.asarray(params["offset"], dtype=np.float64)
    for item in result.data["items"]:
        ctx.check_cancelled()
        delta = offset
        if params["local"]:
            x, y, z, w = item["rotation"]
            vector = np.asarray([x, y, z])
            delta = offset + 2 * np.cross(vector, np.cross(vector, offset) + w * offset)
        item["position"] = (np.asarray(item["position"]) + delta).tolist()
    ctx.log(f"offset {len(result.data['items'])} poses in meters")
    return {"poses": result}


@node(id="py.p6_adapters.filter_pickable", version="1.0.0", label="可抓工件筛选",
      inputs={"poses": "Record"}, outputs={"poses": "Record"},
      keywords=["筛选", "可抓", "pickable", "filter", "pose"])
def filter_pickable(inputs, params, ctx):
    dimension = _poses(inputs["poses"])
    accepted = []
    for item in inputs["poses"].copy().data["items"]:
        ctx.check_cancelled()
        if item.get("pickable") is True:
            accepted.append(item)
    source = inputs["poses"].data
    return {"poses": poses(accepted, dimension, frame=source["frame"], unit=source["unit"])}


@node(id="py.p6_adapters.poses_to_json", version="1.0.0", label="工件位姿 JSON 编码",
      inputs={"poses": "Record"}, outputs={"text": "Record"},
      keywords=["工件", "序列化", "pose", "serialize", "json"])
def poses_to_json(inputs, params, ctx):
    source = inputs["poses"]
    _poses(source)
    ctx.check_cancelled()
    document = {"type": source.record_type, "data": source.copy().data}
    return {"text": scalar(json.dumps(document, ensure_ascii=False, separators=(",", ":"), allow_nan=False))}


@node(id="py.p6_adapters.poses_from_json", version="1.0.0", label="工件位姿 JSON 解码",
      inputs={"text": "Record"}, outputs={"poses": "Record"},
      keywords=["工件", "反序列化", "pose", "deserialize", "json"])
def poses_from_json(inputs, params, ctx):
    text = inputs["text"]
    if text.record_type != "std.Value.v1" or text.data["kind"] != "string":
        raise NodeError("expected a std.Value.v1 string")
    ctx.check_cancelled()
    document = json.loads(text.data["value"])
    if not isinstance(document, dict) or set(document) != {"type", "data"}:
        raise NodeError("expected a typed pose JSON envelope")
    result = Record(document["data"], document["type"])
    _poses(result)
    return {"poses": result}


@node(id="py.p6_adapters.tensor_affine", version="1.0.0", label="张量线性变换",
      inputs={"tensor": "Tensor"}, outputs={"tensor": "Tensor"},
      params={"gain": {"type": "float", "default": 1}, "bias": {"type": "float", "default": 0}},
      keywords=["张量", "tensor", "affine", "gain", "bias"])
def tensor_affine(inputs, params, ctx):
    result = inputs["tensor"].copy()
    flat = result.values.reshape(-1)
    for start in range(0, flat.size, 262144):
        ctx.check_cancelled()
        block = flat[start:start + 262144]
        block[:] = block * params["gain"] + params["bias"]
    return {"tensor": result}
