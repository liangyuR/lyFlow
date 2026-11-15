from __future__ import annotations

import json
import math
import numbers
from collections.abc import Mapping

from .api import NodeError


MAX_ITEMS = 4096
MAX_BYTES = 1024 * 1024
MAX_STRING_BYTES = 65536
MAX_DEPTH = 16
MAX_INTEGER = 2 ** 53
POSE_TYPES = {"vision.Pose2D.v1": 2, "vision.Poses2D.v1": 2,
              "vision.Pose3D.v1": 3, "vision.Poses3D.v1": 3}


def _number(value):
    return isinstance(value, numbers.Real) and not isinstance(value, bool) and math.isfinite(value) and (not isinstance(value, numbers.Integral) or abs(int(value)) <= MAX_INTEGER)


def _sequence(value):
    return isinstance(value, (tuple, list))


def _vector(value, length, positive=False):
    return _sequence(value) and len(value) == length and all(_number(v) and (not positive or v > 0) for v in value)


def _text(value, required=True):
    return isinstance(value, str) and (bool(value) or not required)


def _plain(value):
    if isinstance(value, Mapping):
        return {key: _plain(item) for key, item in value.items()}
    if _sequence(value):
        return [_plain(item) for item in value]
    if isinstance(value, numbers.Integral) and not isinstance(value, bool):
        return int(value)
    if isinstance(value, numbers.Real) and not isinstance(value, bool):
        return float(value)
    return value


def _bounded(data, allow_null=True):
    pending = [(data, 0)]
    count = 0
    while pending:
        value, depth = pending.pop()
        count += 1
        if depth > MAX_DEPTH or count > MAX_BYTES:
            raise NodeError("Record nesting or item budget exceeded")
        if value is None:
            if not allow_null:
                raise NodeError("This standard record does not allow null")
        elif isinstance(value, bool):
            pass
        elif isinstance(value, str):
            if len(value.encode("utf-8")) > MAX_STRING_BYTES:
                raise NodeError("Record string exceeds 64 KiB")
        elif isinstance(value, numbers.Real):
            if not _number(value):
                raise NodeError("Record numbers must be finite and integers within [-2^53,2^53]; use strings for larger IDs")
        elif isinstance(value, Mapping):
            if len(value) > MAX_ITEMS or not all(isinstance(key, str) for key in value):
                raise NodeError("Record objects require string keys and at most 4096 members")
            for key, child in value.items():
                if len(key.encode("utf-8")) > MAX_STRING_BYTES:
                    raise NodeError("Record key exceeds 64 KiB")
                pending.append((child, depth + 1))
        elif _sequence(value):
            if len(value) > MAX_ITEMS:
                raise NodeError("Record arrays exceed 4096 items; use native Tensor for large arrays")
            pending.extend((child, depth + 1) for child in value)
        else:
            raise NodeError(f"Record contains unsupported value {type(value).__name__}")
    try:
        payload = json.dumps(_plain(data), ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
    except (TypeError, ValueError, UnicodeError) as error:
        raise NodeError("Record must contain valid UTF-8 JSON values") from error
    if len(payload) > MAX_BYTES:
        raise NodeError("Record exceeds 1 MiB")


def _pose_item(item, dimension):
    required = {"id", "frame", "unit", "position", "size", "angle" if dimension == 2 else "rotation"}
    optional = {"name", "classId", "classLabel", "confidence", "pickable", "attributes"}
    if not isinstance(item, Mapping) or not required <= item.keys() or set(item) - required - optional:
        raise NodeError("Pose has missing, unknown or dimension-incompatible fields")
    if not all(_text(item[key]) for key in ("id", "frame", "unit")):
        raise NodeError("Pose id/frame/unit must be nonempty strings")
    if item["unit"] not in (("m", "px") if dimension == 2 else ("m",)):
        raise NodeError("2D poses use m/px and 3D poses use m")
    if not _vector(item["position"], dimension) or not _vector(item["size"], dimension, True):
        raise NodeError("Pose requires finite position and positive physical dimensions")
    if dimension == 2:
        if not _number(item["angle"]):
            raise NodeError("Pose2D angle must be finite radians")
    elif not _vector(item["rotation"], 4) or abs(math.hypot(*item["rotation"]) - 1) > 1e-6:
        raise NodeError("Pose3D rotation must be a normalized xyzw quaternion")
    for key in ("name", "classId", "classLabel"):
        if key in item and not _text(item[key], False):
            raise NodeError(f"Pose {key} must be a string")
    if "confidence" in item and (not _number(item["confidence"]) or not 0 <= item["confidence"] <= 1):
        raise NodeError("Pose confidence must be within [0,1]")
    if "pickable" in item and not isinstance(item["pickable"], bool):
        raise NodeError("Pose pickable must be boolean")
    if "attributes" in item and not isinstance(item["attributes"], Mapping):
        raise NodeError("Pose attributes must be an object")


def _kind(kind, value):
    if kind == "number":
        return _number(value)
    if kind == "boolean":
        return isinstance(value, bool)
    if kind == "string":
        return isinstance(value, str)
    return kind == "record" and isinstance(value, Mapping) and set(value) == {"type", "data"} and _text(value["type"]) and isinstance(value["data"], Mapping)


def validate_record(record_type, data):
    if not isinstance(record_type, str) or len(record_type.encode("utf-8")) > MAX_STRING_BYTES:
        raise NodeError("Record type must be a UTF-8 string of at most 64 KiB")
    _bounded(data)
    prefixes = ("std.Value.", "std.List.", "vision.Pose2D.", "vision.Pose3D.", "vision.Poses2D.", "vision.Poses3D.", "geometry.Features3D.")
    if record_type.startswith(prefixes) and record_type not in (*POSE_TYPES, "std.Value.v1", "std.List.v1", "geometry.Features3D.v1"):
        raise NodeError(f"Unsupported standard Record version: {record_type}")
    if record_type in ("std.Value.v1", "std.List.v1"):
        content = "items" if record_type == "std.List.v1" else "value"
        if not isinstance(data, Mapping) or set(data) != {"kind", "unit", content} or data["kind"] not in {"number", "boolean", "string", "record"} or not isinstance(data["unit"], str):
            raise NodeError("Standard values require exactly kind, unit and value/items")
        if data["kind"] != "number" and data["unit"]:
            raise NodeError("Only numeric standard values can declare a unit")
        items = data[content] if content == "items" else [data[content]]
        if not _sequence(items) or not all(_kind(data["kind"], item) for item in items):
            raise NodeError("Standard value contents do not match kind")
        if data["kind"] == "record" and len({item["type"] for item in items}) > 1:
            raise NodeError("Standard record-list item types must match")
        _bounded(data, False)
    elif record_type in POSE_TYPES:
        dimension = POSE_TYPES[record_type]
        if record_type.startswith("vision.Poses"):
            if not isinstance(data, Mapping) or set(data) != {"frame", "unit", "items"} or not _text(data["frame"]) or not _sequence(data["items"]):
                raise NodeError("Pose collections require exactly frame, unit and items")
            if data["unit"] not in (("m", "px") if dimension == 2 else ("m",)):
                raise NodeError("Pose collection unit is invalid")
            identifiers = set()
            for item in data["items"]:
                _pose_item(item, dimension)
                if item["frame"] != data["frame"] or item["unit"] != data["unit"] or item["id"] in identifiers:
                    raise NodeError("Pose collection items require matching frame/unit and unique IDs")
                identifiers.add(item["id"])
        else:
            _pose_item(data, dimension)
    elif record_type == "geometry.Features3D.v1":
        if not isinstance(data, Mapping) or not {"frame", "unit", "items"} <= set(data) or set(data) - {"frame", "unit", "items", "sourceFrame"} or not _text(data["frame"]) or data["unit"] != "m" or not _sequence(data["items"]) or ("sourceFrame" in data and not _text(data["sourceFrame"])):
            raise NodeError("Features3D requires frame, unit=m and items")
        identifiers = set()
        for item in data["items"]:
            if not isinstance(item, Mapping) or item.get("type") not in {"point", "line", "circle", "plane", "sphere"}:
                raise NodeError("Features3D supports point, line, circle, plane and sphere")
            kind = item["type"]
            if ("frame" in item and item["frame"] != data["frame"]) or ("unit" in item and item["unit"] != "m"):
                raise NodeError("Features3D item frame and unit must match the collection")
            if "id" in item:
                if not _text(item["id"]) or item["id"] in identifiers:
                    raise NodeError("Features3D item IDs must be nonempty and unique")
                identifiers.add(item["id"])
            if kind != "plane" and not _vector(item.get("position"), 3):
                raise NodeError("Features3D position must contain three finite meters")
            if kind in {"line", "circle", "plane"}:
                key = "direction" if kind == "line" else "normal"
                if not _vector(item.get(key), 3) or abs(math.hypot(*item[key]) - 1) > 1e-6:
                    raise NodeError(f"Features3D {key} must be normalized")
            if kind in {"circle", "sphere"} and (not _number(item.get("radius")) or item["radius"] <= 0):
                raise NodeError("Features3D radius must be positive finite meters")
            if kind == "plane" and not _number(item.get("d")):
                raise NodeError("Features3D plane d must be finite meters")
            if kind == "line":
                if "hasSegment" in item and not isinstance(item["hasSegment"], bool):
                    raise NodeError("Features3D hasSegment must be boolean")
                if item.get("hasSegment", False):
                    if not _vector(item.get("start"), 3) or not _vector(item.get("end"), 3) or list(item["start"]) == list(item["end"]):
                        raise NodeError("Features3D line segment endpoints must be finite and different")


def scalar(value, unit=""):
    from .data import Record
    kind = "boolean" if isinstance(value, bool) else "number" if _number(value) else "string" if isinstance(value, str) else None
    if kind is None:
        raise NodeError("scalar requires a finite number, boolean or string")
    return Record({"kind": kind, "unit": unit, "value": value}, "std.Value.v1")


def value_list(items, kind="number", unit=""):
    from .data import Record
    return Record({"kind": kind, "unit": unit, "items": list(items)}, "std.List.v1")


def pose2d(identifier, position, size, *, angle=0, frame="image", unit="px", **metadata):
    from .data import Record
    if not _number(angle):
        raise NodeError("Pose2D angle must be finite radians")
    wrapped = math.remainder(angle, 2 * math.pi)
    return Record({"id": identifier, "frame": frame, "unit": unit, "position": list(position),
                   "size": list(size), "angle": wrapped - 2 * math.pi if wrapped >= math.pi else wrapped,
                   **metadata}, "vision.Pose2D.v1")


def pose3d(identifier, position, size, *, rotation=(0, 0, 0, 1), frame="world", **metadata):
    from .data import Record
    if not _vector(rotation, 4):
        raise NodeError("Pose3D rotation must contain four finite xyzw values")
    scale = max(abs(v) for v in rotation)
    if not scale:
        raise NodeError("Pose3D quaternion cannot be zero")
    normalized = [v / scale for v in rotation]
    norm = math.hypot(*normalized)
    normalized = [v / norm for v in normalized]
    flip = normalized[3] < 0 or (normalized[3] == 0 and next((v for v in normalized[:3] if v), 1) < 0)
    if flip:
        normalized = [-v for v in normalized]
    return Record({"id": identifier, "frame": frame, "unit": "m", "position": list(position),
                   "size": list(size), "rotation": normalized, **metadata}, "vision.Pose3D.v1")


def poses(items, dimension, *, frame=None, unit=None):
    from .data import Record
    if dimension not in (2, 3):
        raise NodeError("Pose dimension must be 2 or 3")
    values = [_plain(item.data) if isinstance(item, Record) else _plain(item) for item in items]
    frame = frame if frame is not None else values[0]["frame"] if values else "image" if dimension == 2 else "world"
    unit = unit if unit is not None else values[0]["unit"] if values else "px" if dimension == 2 else "m"
    return Record({"frame": frame, "unit": unit, "items": values}, f"vision.Poses{dimension}D.v1")


def geometry3d(items, *, frame="world"):
    from .data import Record
    return Record({"frame": frame, "unit": "m", "items": [dict(items)] if isinstance(items, Mapping) else list(items)}, "geometry.Features3D.v1")
