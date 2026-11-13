from __future__ import annotations

import copy
import hashlib
import importlib.util
import platform
import sys
import traceback
from pathlib import Path

import numpy as np

from .api import NodeCancelled, NodeError


class Discovery:
    def __init__(self):
        self.operators = {}

    def discover(self, package_paths, check_cancelled=lambda: None):
        if not isinstance(package_paths, list) or not all(isinstance(path, str) for path in package_paths):
            raise NodeError("packagePaths must be a list of paths")
        found, problems, seen = {}, [], set()

        def problem(path, error):
            problems.append({"path": str(path), "message": str(error), "traceback": "".join(traceback.format_exception(error))})

        def candidates(path):
            if path.is_file():
                if path.suffix != ".py":
                    raise NodeError("Python node file must end in .py")
                yield path
            elif path.is_dir():
                if not (path / "__init__.py").is_file():
                    raise NodeError("Python package directory requires __init__.py; list individual .py files or package directories explicitly")
                yield path / "__init__.py"
            else:
                raise NodeError("Python package path does not exist")

        for package_path in package_paths:
            try:
                paths = list(candidates(Path(package_path).resolve()))
            except Exception as error:
                problem(package_path, error)
                continue
            for path in paths:
                check_cancelled()
                identity = str(path.resolve())
                if identity in seen:
                    continue
                seen.add(identity)
                prefix = "_lyflow_node_" + hashlib.sha256(identity.encode()).hexdigest()[:24]
                for name in list(sys.modules):
                    if name == prefix or name.startswith(prefix + "."):
                        del sys.modules[name]
                original_path = sys.path[:]
                try:
                    is_package = path.name == "__init__.py"
                    spec = importlib.util.spec_from_file_location(prefix, path, submodule_search_locations=[str(path.parent)] if is_package else None)
                    if spec is None or spec.loader is None:
                        raise NodeError("cannot load Python package")
                    module = importlib.util.module_from_spec(spec)
                    sys.modules[prefix] = module
                    sys.path.insert(0, str(path.parent.parent if is_package else path.parent))
                    spec.loader.exec_module(module)
                    functions = set()
                    for function in vars(module).values():
                        manifest = getattr(function, "__lyflow_node__", None)
                        if not callable(function) or not isinstance(manifest, dict) or id(function) in functions:
                            continue
                        functions.add(id(function))
                        found.setdefault(manifest["id"], []).append((function, copy.deepcopy(manifest), identity))
                except BaseException as error:
                    if isinstance(error, NodeCancelled):
                        raise
                    problem(path, error)
                    for name in list(sys.modules):
                        if name == prefix or name.startswith(prefix + "."):
                            del sys.modules[name]
                finally:
                    sys.path[:] = original_path
        enabled = {}
        for identifier, entries in found.items():
            if len(entries) != 1:
                for _, _, path in entries:
                    problem(path, NodeError(f"duplicate Python operator id disabled: {identifier}"))
            else:
                enabled[identifier] = entries[0]
        check_cancelled()
        self.operators = enabled
        return {"operators": [enabled[key][1] for key in sorted(enabled)], "problems": problems,
                "pythonVersion": platform.python_version(), "numpyVersion": np.__version__}
