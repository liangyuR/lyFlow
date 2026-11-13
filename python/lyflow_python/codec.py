from __future__ import annotations

import json
import math
import sys
import uuid
from pathlib import Path

import numpy as np

from .api import NodeError
from .data import Image, Measurement, PointCloud, Record, _plain


DTYPES = {"f32": np.dtype("<f4"), "u8": np.dtype("u1"), "u16": np.dtype("<u2")}
TYPES = {"PointCloud": PointCloud, "Image": Image, "Measurement": Measurement, "Record": Record}


class DataCodec:
    def __init__(self, request_dir, check_cancelled=lambda: None):
        if not isinstance(request_dir, str) or not Path(request_dir).is_absolute():
            raise NodeError("requestDir must be an absolute directory")
        self.root = Path(request_dir).resolve(strict=True)
        if not self.root.is_dir():
            raise NodeError("requestDir must be a directory")
        self.maps, self.outputs = [], []
        self.check_cancelled = check_cancelled

    def close(self, success=False):
        for mapping in reversed(self.maps):
            mapping._mmap.close()
        self.maps.clear()
        if not success:
            for path in self.outputs:
                path.unlink(missing_ok=True)
        self.outputs.clear()

    def read_array(self, descriptor):
        self.check_cancelled()
        if not isinstance(descriptor, dict) or set(descriptor) != {"file", "shape", "dtype"}:
            raise NodeError("array DTO must contain file, shape and dtype")
        file = descriptor["file"]
        if not isinstance(file, str) or not Path(file).is_absolute():
            raise NodeError("array file must be absolute within requestDir")
        path = Path(file).resolve(strict=True)
        if not path.is_relative_to(self.root) or not path.is_file():
            raise NodeError("array file escapes requestDir or is not a regular file")
        shape, dtype = descriptor["shape"], descriptor["dtype"]
        if not isinstance(dtype, str) or dtype not in DTYPES:
            raise NodeError(f"unsupported array dtype: {dtype!r}")
        if not isinstance(shape, list) or not shape or len(shape) > 3 or any(isinstance(n, bool) or not isinstance(n, int) or n < 0 for n in shape):
            raise NodeError("array shape must have one to three nonnegative integer dimensions")
        size = math.prod(shape) * DTYPES[dtype].itemsize
        if size > sys.maxsize or path.stat().st_size != size:
            raise NodeError("array file byte size does not match shape and dtype")
        if size == 0:
            array = np.empty(shape, dtype=DTYPES[dtype])
            array.flags.writeable = False
            return array
        array = np.memmap(path, mode="r", dtype=DTYPES[dtype], shape=tuple(shape), order="C")
        self.maps.append(array)
        return array

    def decode(self, dto, expected_type):
        if not isinstance(dto, dict) or dto.get("type") != expected_type:
            raise NodeError(f"expected {expected_type} DTO, got {dto.get('type') if isinstance(dto, dict) else type(dto).__name__}")
        if expected_type == "PointCloud":
            if "xyz" not in dto or set(dto) - {"type", "xyz", "intensity", "normals", "rgb"}:
                raise NodeError("PointCloud DTO requires xyz and only declared channels")
            result = PointCloud(**{name: self.read_array(value) for name, value in dto.items() if name != "type"})
        elif expected_type == "Image":
            if set(dto) != {"type", "pixels"}:
                raise NodeError("Image DTO requires pixels")
            result = Image(self.read_array(dto["pixels"]))
        elif expected_type == "Measurement":
            allowed = {"type", "value", "unit", "ok", "message", "verdict", "nominal", "lower", "upper"}
            if "value" not in dto or set(dto) - allowed:
                raise NodeError("invalid Measurement DTO fields")
            result = Measurement(**{key: value for key, value in dto.items() if key != "type"})
        elif expected_type == "Record":
            if set(dto) != {"type", "recordType", "data"}:
                raise NodeError("Record DTO requires recordType and data")
            json.dumps(dto["data"], allow_nan=False)
            result = Record(dto["data"], dto["recordType"])
        else:
            raise NodeError(f"unsupported port type: {expected_type}")
        return result._freeze()

    def write_array(self, array):
        self.check_cancelled()
        dtype = next((name for name, value in DTYPES.items() if array.dtype == value), None)
        if dtype is None:
            raise NodeError(f"unsupported output dtype: {array.dtype}")
        path = self.root / ("out-" + uuid.uuid4().hex + ".bin")
        self.outputs.append(path)
        if array.size == 0:
            path.touch(exist_ok=False)
        else:
            mapping = np.memmap(path, mode="w+", dtype=DTYPES[dtype], shape=array.shape, order="C")
            try:
                stride = max(1, 4 * 1024 * 1024 // max(1, math.prod(array.shape[1:]) * array.dtype.itemsize))
                for start in range(0, len(array), stride):
                    self.check_cancelled()
                    mapping[start:start + stride] = array[start:start + stride]
                mapping.flush()
            finally:
                mapping._mmap.close()
        return {"file": str(path), "shape": list(array.shape), "dtype": dtype}

    def encode(self, value, expected_type):
        self.check_cancelled()
        if expected_type not in TYPES or not isinstance(value, TYPES[expected_type]):
            raise NodeError(f"output requires {expected_type}, got {type(value).__name__}")
        if expected_type == "PointCloud":
            PointCloud(value.xyz, value.intensity, value.normals, value.rgb)
            return {"type": expected_type, **{name: self.write_array(getattr(value, name)) for name in ("xyz", "intensity", "normals", "rgb") if getattr(value, name) is not None}}
        if expected_type == "Image":
            Image(value.pixels)
            return {"type": expected_type, "pixels": self.write_array(value.pixels)}
        if expected_type == "Measurement":
            fields = {key: item for key, item in vars(value).items() if key != "_readonly" and item is not None}
            if value.value is None:
                fields["value"] = None
            Measurement(**fields)
            return {"type": expected_type, **fields}
        data = _plain(value.data)
        if not isinstance(value.record_type, str):
            raise NodeError("Record.record_type must be a string")
        json.dumps(data, allow_nan=False)
        return {"type": expected_type, "recordType": value.record_type, "data": data}
