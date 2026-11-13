from __future__ import annotations

import copy
import inspect
import json
import math
import re
import threading
from pathlib import Path


class NodeError(ValueError):
    pass


class NodeCancelled(Exception):
    pass


class Context:
    def __init__(self, *, base_dir="", thread_budget=1, cancel_event=None, emit=None):
        if isinstance(thread_budget, bool) or not isinstance(thread_budget, int) or thread_budget < 1:
            raise NodeError("threadBudget must be a positive integer")
        self.base_dir = Path(base_dir) if base_dir else Path.cwd()
        self.thread_budget = thread_budget
        self._cancel_event = cancel_event or threading.Event()
        self._emit = emit or (lambda event: None)

    @property
    def cancelled(self):
        return self._cancel_event.is_set()

    def check_cancelled(self):
        if self.cancelled:
            raise NodeCancelled("Python node was cancelled")

    def log(self, message, level="info"):
        self.check_cancelled()
        if level == "warning":
            level = "warn"
        if level not in {"debug", "info", "warn", "error"}:
            raise NodeError(f"invalid log level: {level}")
        self._emit({"event": "log", "level": level, "message": str(message)})

    def progress(self, ratio, message=""):
        self.check_cancelled()
        if isinstance(ratio, bool) or not isinstance(ratio, (int, float)) or not math.isfinite(ratio) or not 0 <= ratio <= 1:
            raise NodeError("progress ratio must be finite and within [0, 1]")
        self._emit({"event": "progress", "ratio": float(ratio), "message": str(message)})


PORT_TYPES = {"PointCloud", "Image", "Measurement", "Record"}
PARAM_TYPES = {"bool", "int", "float", "vec2f", "vec3f", "vec4f", "enum", "flags", "string", "text", "path", "color", "transform", "curve"}
PORT_FIELDS = {"type", "label", "doc", "required", "contract", "example"}
PARAM_FIELDS = {"type", "label", "doc", "default", "group", "advanced", "min", "max", "softMin", "softMax", "step", "unit", "componentLabels", "options", "placeholder", "pattern", "rows", "filters", "mode", "alpha", "visibleWhen", "enabledWhen", "semantic", "roiBackdrop", "tuningRole", "unitSource"}


def _enum_string(value, name):
    if isinstance(value, str):
        return value
    if (isinstance(value, int) and not isinstance(value, bool)) or (isinstance(value, float) and math.isfinite(value)):
        return json.dumps(value, allow_nan=False)
    raise NodeError(f"{name}: enum values must be strings or finite numbers")


def resolve_params(specs, supplied):
    if not isinstance(supplied, dict) or set(supplied) - {p["name"] for p in specs}:
        raise NodeError("params must be a dictionary containing only declared parameters")
    result = {}
    for spec in specs:
        name, kind = spec["name"], spec["type"]
        value = copy.deepcopy(supplied.get(name, spec["default"]))
        number = lambda v: isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)
        valid = True
        if kind == "bool":
            valid = isinstance(value, bool)
        elif kind in {"int", "flags"}:
            valid = isinstance(value, int) and not isinstance(value, bool) and (kind != "flags" or value >= 0)
        elif kind == "float":
            valid = number(value)
        elif kind in {"string", "text", "path"}:
            valid = isinstance(value, str)
        elif kind in {"vec2f", "vec3f", "vec4f", "color", "transform"}:
            length = {"vec2f": 2, "vec3f": 3, "vec4f": 4, "color": 4 if spec.get("alpha") else 3, "transform": 16}[kind]
            valid = isinstance(value, list) and len(value) == length and all(number(v) for v in value)
        elif kind == "enum":
            valid = isinstance(value, str) and any(value == item["value"] for item in spec.get("options", []))
        elif kind == "curve":
            valid = isinstance(value, dict) and isinstance(value.get("points"), list) and len(value["points"]) >= 2
        if not valid:
            raise NodeError(f"parameter {name}: invalid {kind} value {value!r}")
        values = value if isinstance(value, list) else [value]
        for item in values:
            if number(item) and (("min" in spec and item < spec["min"]) or ("max" in spec and item > spec["max"])):
                raise NodeError(f"parameter {name}: value is outside declared min/max")
        json.dumps(value, allow_nan=False)
        result[name] = value
    return result


def _named_specs(specs, ports):
    if not isinstance(specs, dict):
        raise NodeError("inputs, outputs and params must be dictionaries")
    result = []
    for name, spec in specs.items():
        if not isinstance(name, str) or not name.isidentifier() or name == "ctx":
            raise NodeError(f"invalid or reserved port/parameter name: {name!r}")
        if ports and isinstance(spec, str):
            spec = {"type": spec}
        if not isinstance(spec, dict):
            raise NodeError(f"{name}: expected a type specification")
        allowed = PORT_FIELDS if ports else PARAM_FIELDS
        extra = set(spec) - allowed
        if extra:
            raise NodeError(f"{name}: unsupported specification fields: {sorted(extra)}")
        if spec.get("type") not in (PORT_TYPES if ports else PARAM_TYPES):
            raise NodeError(f"{name}: unsupported type {spec.get('type')!r}")
        if not ports and "default" not in spec:
            raise NodeError(f"{name}: parameter default is required")
        if ports and "required" in spec and not isinstance(spec["required"], bool):
            raise NodeError(f"{name}: required must be boolean")
        normalized = {"name": name, **copy.deepcopy(spec)}
        if not ports and normalized["type"] == "enum":
            options = normalized.get("options")
            if not isinstance(options, list) or not options:
                raise NodeError(f"{name}: enum options must be a nonempty list")
            for option in options:
                if not isinstance(option, dict) or "value" not in option or set(option) - {"value", "label", "doc"}:
                    raise NodeError(f"{name}: enum options must contain value and optional label/doc")
                option["value"] = _enum_string(option["value"], name)
                option.setdefault("label", option["value"])
                if not isinstance(option["label"], str) or ("doc" in option and not isinstance(option["doc"], str)):
                    raise NodeError(f"{name}: enum option label/doc must be strings")
            normalized["default"] = _enum_string(normalized["default"], name)
        result.append(normalized)
    return result


def node(*, id, version, label, inputs=None, outputs=None, params=None, category="Python", doc="", keywords=None):
    if not isinstance(id, str) or not re.fullmatch(r"py\.[A-Za-z0-9_]+(?:[.-][A-Za-z0-9_]+)*", id):
        raise NodeError("Python operator id must start with 'py.' and contain named segments")
    if not isinstance(version, str) or not re.fullmatch(r"\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?", version):
        raise NodeError("version must be a semantic version such as 1.0.0")
    if not isinstance(label, str) or not label.strip() or not isinstance(category, str) or not category.strip():
        raise NodeError("label and category must be nonempty strings")
    manifest = {
        "id": id, "version": version, "label": label, "category": category,
        "inputs": _named_specs(inputs or {}, True), "outputs": _named_specs(outputs or {}, True),
        "params": _named_specs(params or {}, False),
        "capabilities": {"deterministic": False, "cancellable": True, "previewable": False},
    }
    if not manifest["outputs"]:
        raise NodeError("at least one output port is required")
    if doc:
        manifest["doc"] = doc
    if keywords is not None:
        manifest["keywords"] = list(keywords)
    json.dumps(manifest, allow_nan=False)
    resolve_params(manifest["params"], {})
    names = [entry["name"] for entry in manifest["inputs"] + manifest["params"]]
    if len(names) != len(set(names)):
        raise NodeError("input and parameter names must not overlap")

    def decorate(function):
        if not inspect.isfunction(function) or inspect.iscoroutinefunction(function):
            raise NodeError("@node requires a synchronous Python function")
        signature = inspect.signature(function)
        dictionary_call = {"inputs", "params", "ctx"}.issubset(signature.parameters)
        arguments = {"inputs": {}, "params": {}, "ctx": None} if dictionary_call else {name: None for name in names}
        if not dictionary_call and ("ctx" in signature.parameters or any(p.kind == p.VAR_KEYWORD for p in signature.parameters.values())):
            arguments["ctx"] = None
        try:
            signature.bind(**arguments)
        except TypeError as error:
            raise NodeError(f"{id}: function signature does not match its ports/params: {error}") from error
        function.__lyflow_node__ = copy.deepcopy(manifest)
        function.__lyflow_call_style__ = "dict" if dictionary_call else "kwargs"
        return function

    return decorate
