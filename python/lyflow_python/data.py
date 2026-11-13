from __future__ import annotations

import copy
import math
import numbers
from collections.abc import Mapping
from types import MappingProxyType

import numpy as np

from .api import NodeError


def _array(name, value, dtype, shape):
    if not isinstance(value, np.ndarray) or value.dtype != np.dtype(dtype):
        raise NodeError(f"{name}: expected NumPy {np.dtype(dtype).name} array")
    if value.ndim != len(shape) or any(want is not None and value.shape[i] != want for i, want in enumerate(shape)):
        raise NodeError(f"{name}: expected shape {shape}, got {value.shape}")
    return value


def _frozen(value):
    if isinstance(value, Mapping):
        return MappingProxyType({k: _frozen(v) for k, v in value.items()})
    if isinstance(value, (list, tuple)):
        return tuple(_frozen(v) for v in value)
    return value


def _plain(value):
    if isinstance(value, Mapping):
        return {k: _plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_plain(v) for v in value]
    return copy.deepcopy(value)


class _Value:
    _readonly = False

    def __setattr__(self, name, value):
        if self._readonly:
            raise NodeError("input data is read-only; call copy() before editing")
        object.__setattr__(self, name, value)

    def _freeze(self):
        for value in vars(self).values():
            if isinstance(value, np.ndarray):
                value.flags.writeable = False
        object.__setattr__(self, "_readonly", True)
        return self


class PointCloud(_Value):
    def __init__(self, xyz, intensity=None, normals=None, rgb=None):
        self.xyz = _array("xyz", xyz, np.float32, (None, 3))
        count = len(xyz)
        self.intensity = None if intensity is None else _array("intensity", intensity, np.float32, (count,))
        self.normals = None if normals is None else _array("normals", normals, np.float32, (count, 3))
        self.rgb = None if rgb is None else _array("rgb", rgb, np.uint8, (count, 3))

    def copy(self):
        return PointCloud(**{name: None if value is None else value.copy() for name, value in vars(self).items() if name != "_readonly"})


class Image(_Value):
    def __init__(self, pixels):
        if not isinstance(pixels, np.ndarray) or pixels.dtype not in (np.dtype("uint8"), np.dtype("uint16"), np.dtype("float32")):
            raise NodeError("pixels: expected NumPy uint8/uint16/float32 array")
        if pixels.ndim not in (2, 3) or any(d < 1 for d in pixels.shape[:2]) or (pixels.ndim == 3 and pixels.shape[2] not in (1, 3, 4)):
            raise NodeError(f"pixels: expected HxW or HxWx1/3/4, got {pixels.shape}")
        self.pixels = pixels

    def copy(self):
        return Image(self.pixels.copy())


class Measurement(_Value):
    def __init__(self, value, unit="", ok=None, message="", verdict="", nominal=None, lower=None, upper=None):
        if value is not None and (isinstance(value, bool) or not isinstance(value, numbers.Real) or not math.isfinite(value)):
            raise NodeError("Measurement.value must be finite or None")
        if ok is None:
            ok = value is not None
        if not isinstance(ok, bool) or (value is None and ok):
            raise NodeError("Measurement.ok must be boolean and false when value is None")
        if not all(isinstance(item, str) for item in (unit, message, verdict)):
            raise NodeError("Measurement unit, message and verdict must be strings")
        limits = (nominal, lower, upper)
        if any(item is not None for item in limits) and not all(isinstance(item, numbers.Real) and not isinstance(item, bool) and math.isfinite(item) for item in limits):
            raise NodeError("Measurement limits must supply finite nominal/lower/upper together")
        self.value, self.unit, self.ok = None if value is None else float(value), unit, ok
        self.message, self.verdict = message, verdict
        self.nominal, self.lower, self.upper = (None if item is None else float(item) for item in limits)

    def copy(self):
        return Measurement(**{name: value for name, value in vars(self).items() if name != "_readonly"})


class Record(_Value):
    def __init__(self, data, record_type=""):
        if not isinstance(record_type, str):
            raise NodeError("Record.record_type must be a string")
        self.data = _plain(data)
        self.record_type = record_type

    def _freeze(self):
        self.data = _frozen(self.data)
        return super()._freeze()

    def copy(self):
        return Record(_plain(self.data), self.record_type)
