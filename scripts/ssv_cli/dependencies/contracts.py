"""依赖准备结果与构建快照契约。"""

from __future__ import annotations

import os
import shlex
import tempfile
from dataclasses import dataclass
from pathlib import Path

from .policy import _error

SNAPSHOT_KEYS = (
    "SSV_DEPS_SIGNATURE",
    "SSV_DEPS_PROFILE",
    "SSV_DEPS_PKG_CONFIG_PATH",
    "SSV_DEPS_RUNTIME_PATH",
    "SSV_DEPS_OPENCV_MODE",
    "SSV_DEPS_TENSORRT_MODE",
    "SSV_DEPS_ONNXRUNTIME_VERSION",
    "SSV_DEPS_ONNXRUNTIME_PCDIR",
    "SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS",
    "SSV_DEPS_ONNXRUNTIME_PROVIDERS",
    "SSV_DEPS_ONNXRUNTIME_PROVIDER_LIBRARIES",
    "SSV_DEPS_OPENCV_PCDIR",
    "SSV_DEPS_OPENCV_RUNTIME_DIRS",
    "SSV_DEPS_TENSORRT_PCDIR",
    "SSV_DEPS_TENSORRT_RUNTIME_DIRS",
)

@dataclass(frozen=True)
class DependencyResult:
    source: str
    version: str
    pc_dir: str = ""
    runtime_dirs: str = ""
    providers: str = ""
    provider_libraries: str = ""
    include_dir: str = ""
    lib_dir: str = ""


@dataclass(frozen=True)
class DependencySnapshot:
    signature: str
    profile: str
    pkg_config_path: str
    runtime_path: str
    opencv_mode: str
    tensorrt_mode: str
    onnxruntime_version: str
    onnxruntime_pcdir: str
    onnxruntime_runtime_dirs: str
    onnxruntime_providers: str
    onnxruntime_provider_libraries: str
    opencv_pcdir: str
    opencv_runtime_dirs: str
    tensorrt_pcdir: str
    tensorrt_runtime_dirs: str

    def values(self) -> dict[str, str]:
        return {
            "SSV_DEPS_SIGNATURE": self.signature,
            "SSV_DEPS_PROFILE": self.profile,
            "SSV_DEPS_PKG_CONFIG_PATH": self.pkg_config_path,
            "SSV_DEPS_RUNTIME_PATH": self.runtime_path,
            "SSV_DEPS_OPENCV_MODE": self.opencv_mode,
            "SSV_DEPS_TENSORRT_MODE": self.tensorrt_mode,
            "SSV_DEPS_ONNXRUNTIME_VERSION": self.onnxruntime_version,
            "SSV_DEPS_ONNXRUNTIME_PCDIR": self.onnxruntime_pcdir,
            "SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS": self.onnxruntime_runtime_dirs,
            "SSV_DEPS_ONNXRUNTIME_PROVIDERS": self.onnxruntime_providers,
            "SSV_DEPS_ONNXRUNTIME_PROVIDER_LIBRARIES": self.onnxruntime_provider_libraries,
            "SSV_DEPS_OPENCV_PCDIR": self.opencv_pcdir,
            "SSV_DEPS_OPENCV_RUNTIME_DIRS": self.opencv_runtime_dirs,
            "SSV_DEPS_TENSORRT_PCDIR": self.tensorrt_pcdir,
            "SSV_DEPS_TENSORRT_RUNTIME_DIRS": self.tensorrt_runtime_dirs,
        }


def write_snapshot(path: Path, snapshot: DependencySnapshot) -> None:
    values = snapshot.values()
    missing = [key for key in SNAPSHOT_KEYS if key not in values]
    if missing:
        raise _error(f"dependency snapshot is missing {missing[0]}")
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            for key in SNAPSHOT_KEYS:
                output.write(f"{key}={shlex.quote(values[key])}\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
