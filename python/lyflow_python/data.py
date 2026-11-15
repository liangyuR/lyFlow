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
    if isinstance(value, numbers.Integral) and not isinstance(value, bool):
        return int(value)
    if isinstance(value, numbers.Real) and not isinstance(value, bool):
        return float(value)
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
    def __init__(self, pixels, scale=1):
        if not isinstance(pixels, np.ndarray) or pixels.dtype not in (np.dtype("uint8"), np.dtype("uint16"), np.dtype("float32")):
            raise NodeError("pixels: expected NumPy uint8/uint16/float32 array")
        if pixels.ndim not in (2, 3) or any(d < 1 for d in pixels.shape[:2]) or (pixels.ndim == 3 and pixels.shape[2] not in (1, 3, 4)):
            raise NodeError(f"pixels: expected HxW or HxWx1/3/4, got {pixels.shape}")
        self.pixels = pixels
        self.scale = _scale(scale)

    def copy(self):
        return Image(self.pixels.copy(), self.scale)


def _scale(value):
    if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 2147483647:
        raise NodeError("scale must be a positive int32 integer")
    return value


def _finite_vector(name, value, count):
    try:
        raw = np.asarray(value)
        if raw.dtype.kind not in "iuf" or any(isinstance(v, (bool, np.bool_)) for v in value):
            raise ValueError("coordinates must be numeric, not boolean or string")
        array = raw.astype(np.float64)
    except (TypeError, ValueError, OverflowError) as error:
        raise NodeError(f"{name}: expected {count} finite float32 coordinates") from error
    if array.shape != (count,) or not np.isfinite(array).all() or (np.abs(array) > np.finfo(np.float32).max).any():
        raise NodeError(f"{name}: expected {count} finite float32 coordinates")
    return array.astype(np.float32)


def _unit(value):
    if not isinstance(value, str) or value not in ("m", "px"):
        raise NodeError("2D geometry unit must be m or px")
    return value


class Tensor(_Value):
    def __init__(self, values, scale=1, shape=None):
        if not isinstance(values, np.ndarray) or values.dtype != np.dtype("float32"):
            raise NodeError("Tensor.values must be a float32 NumPy array")
        dimensions = tuple(values.shape) if shape is None else tuple(shape)
        if len(dimensions) > 8 or any(isinstance(n, bool) or not isinstance(n, int) or not 0 <= n <= 2147483647 for n in dimensions):
            raise NodeError("Tensor.shape must have at most eight nonnegative int32 dimensions")
        count = math.prod(dimensions) if dimensions else 0
        if count != values.size or values.nbytes > 1024 ** 3:
            raise NodeError("Tensor shape or 1 GiB byte budget does not match its values")
        self.values = values.reshape(dimensions) if dimensions else values.reshape(0)
        self.shape = dimensions
        self.scale = _scale(scale)

    def copy(self):
        return Tensor(self.values.copy(), self.scale, self.shape)


class Transform(_Value):
    def __init__(self, matrix):
        try:
            array = np.asarray(matrix, dtype=object)
        except (TypeError, ValueError) as error:
            raise NodeError("Transform.matrix must be a finite float32 4x4 matrix") from error
        if array.shape != (4, 4):
            raise NodeError("Transform.matrix must be a finite float32 4x4 matrix")
        values = list(array.reshape(-1))
        if any(isinstance(value, (bool, np.bool_)) or not isinstance(value, numbers.Real) for value in values):
            raise NodeError("Transform.matrix must contain numeric nonboolean coordinates")
        self.matrix = _finite_vector("Transform.matrix", values, 16).reshape(4, 4)

    def copy(self):
        return Transform(self.matrix.copy())


class Plane(_Value):
    def __init__(self, normal, d):
        self.normal = _finite_vector("Plane.normal", normal, 3)
        self.d = float(_finite_vector("Plane.d", [d], 1)[0])
        if abs(float(np.linalg.norm(self.normal.astype(np.float64))) - 1) > 1e-4:
            raise NodeError("Plane.normal must be normalized")

    def copy(self):
        return Plane(self.normal.copy(), self.d)


class Point2D(_Value):
    def __init__(self, position, unit="m"):
        self.position = _finite_vector("Point2D.position", position, 2)
        self.unit = _unit(unit)

    def copy(self):
        return Point2D(self.position.copy(), self.unit)


class Line2D(_Value):
    def __init__(self, point, direction, unit="m", start=None, end=None):
        self.point = _finite_vector("Line2D.point", point, 2)
        self.direction = _finite_vector("Line2D.direction", direction, 2)
        if abs(float(np.linalg.norm(self.direction.astype(np.float64))) - 1) > 1e-4:
            raise NodeError("Line2D.direction must be normalized")
        if (start is None) != (end is None):
            raise NodeError("Line2D segment requires both start and end")
        self.start = None if start is None else _finite_vector("Line2D.start", start, 2)
        self.end = None if end is None else _finite_vector("Line2D.end", end, 2)
        if self.start is not None and np.array_equal(self.start, self.end):
            raise NodeError("Line2D segment endpoints must differ")
        self.unit = _unit(unit)

    def copy(self):
        return Line2D(self.point.copy(), self.direction.copy(), self.unit,
                      None if self.start is None else self.start.copy(),
                      None if self.end is None else self.end.copy())


class Circle2D(_Value):
    def __init__(self, center, radius, unit="m"):
        self.center = _finite_vector("Circle2D.center", center, 2)
        self.radius = float(_finite_vector("Circle2D.radius", [radius], 1)[0])
        if self.radius <= 0:
            raise NodeError("Circle2D.radius must be positive")
        self.unit = _unit(unit)

    def copy(self):
        return Circle2D(self.center.copy(), self.radius, self.unit)


class Box2D(_Value):
    def __init__(self, minimum, maximum, unit="m"):
        self.minimum = _finite_vector("Box2D.minimum", minimum, 2)
        self.maximum = _finite_vector("Box2D.maximum", maximum, 2)
        if (self.minimum > self.maximum).any():
            raise NodeError("Box2D minimum must not exceed maximum")
        self.unit = _unit(unit)

    def copy(self):
        return Box2D(self.minimum.copy(), self.maximum.copy(), self.unit)


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
        from .records import validate_record
        validate_record(record_type, data)
        self.data = _plain(data)
        self.record_type = record_type

    def _freeze(self):
        self.data = _frozen(self.data)
        return super()._freeze()

    def copy(self):
        return Record(_plain(self.data), self.record_type)
