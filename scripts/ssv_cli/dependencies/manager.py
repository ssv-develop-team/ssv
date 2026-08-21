"""依赖准备总编排器。

外部调用方只需要构造并调用 DependencyManager.prepare；各个 SDK provider
和系统工具属于本 package 的内部实现。
"""

from __future__ import annotations

import hashlib
import re
from pathlib import Path

from ..context import ProjectContext
from .contracts import DependencyResult, DependencySnapshot, write_snapshot
from .onnxruntime import OnnxRuntimeProvider
from .opencv import OpenCvProvider
from .policy import DependencyConfig, DependencyError, _error, join_unique
from .tensorrt import TensorRtProvider
from .tooling import DependencyTooling


class DependencyManager(
    DependencyTooling,
    OnnxRuntimeProvider,
    OpenCvProvider,
    TensorRtProvider,
):
    """拥有一次项目构建的依赖准备事务。"""

    def __init__(self, context: ProjectContext, config: DependencyConfig) -> None:
        self.context = context
        self.config = config
        self.root = context.root
        self.build_dir = context.build_dir
        self.environment = context.child_environment()
        self.base_pkg_config_path = join_unique(
            config.extra_pkg_config_path,
            self.environment.get("PKG_CONFIG_PATH", ""),
        )
        self.pkg_config_dirs: list[str] = []
        self.ort = DependencyResult(source=config.onnx_source, version="")
        self.opencv = DependencyResult(source=config.opencv_source, version="")
        self.tensorrt = DependencyResult(source=config.tensorrt_source, version="")
        self._onnx_config_version = config.onnx_version

    # ------------------------------------------------------------------
    # Snapshot and public transaction

    def _signature_roots(self) -> list[Path]:
        roots: list[Path] = []
        if self.ort.pc_dir:
            lib_dir = self._pkg_variable("onnxruntime", "libdir")
            if lib_dir:
                link = Path(lib_dir) / "libonnxruntime.so"
                if not link.is_file():
                    raise _error(f"ONNX Runtime signature root is missing: {link}")
                roots.append(link.resolve())
        roots.extend(Path(item).resolve() for item in self.ort.provider_libraries.split(":") if item)
        return roots

    def _linked_library_paths(self) -> list[Path]:
        seen: set[Path] = set()
        search_path = join_unique(
            self.ort.runtime_dirs,
            self.tensorrt.runtime_dirs,
            self.environment.get("LD_LIBRARY_PATH", ""),
        )
        environment = dict(self.environment)
        environment["LD_LIBRARY_PATH"] = search_path
        for root in self._signature_roots():
            if not root.is_file():
                raise _error(f"dependency signature root is missing: {root}")
            result = self._command(["ldd", root], env=environment)
            if result.returncode != 0:
                raise _error(f"failed to inspect linked libraries for dependency signature: {root}")
            output = (result.stdout or "") + (result.stderr or "")
            if "not found" in output:
                raise _error(f"dependency signature has an unresolved library for {root}")
            seen.add(root.resolve())
            for dependency in self._dependency_paths(output):
                if dependency.is_file():
                    seen.add(dependency)
        return sorted(seen, key=str)

    def _file_identity(self, path: Path) -> str:
        build_id = ""
        try:
            output = self._readelf("-n", path)
            match = re.search(r"Build ID:\s*([0-9a-fA-F]+)", output)
            build_id = match.group(1) if match else ""
        except DependencyError:
            pass
        if build_id:
            return f"{path}|build-id={build_id}"
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        return f"{path}|sha256={digest}"

    def _compute_signature(self) -> str:
        dependency_identity = "\n".join(self._file_identity(path) for path in self._linked_library_paths())
        onnx_identity = (
            f"{self.config.profile}|{self.config.onnx_source}|{self._onnx_config_version}|"
            f"{self.ort.version}|{self.ort.pc_dir}|{self.ort.runtime_dirs}|"
            f"{self.ort.providers}|{self.ort.provider_libraries}"
        )
        opencv_identity = (
            f"{self.config.opencv_source}|{self.config.opencv_mode}|{self.opencv.version}|"
            f"{self.opencv.pc_dir}|{self.opencv.runtime_dirs}|{self.opencv.include_dir}|"
            f"{self.opencv.lib_dir}"
        )
        tensorrt_identity = (
            f"{self.config.tensorrt_source}|{self.config.tensorrt_mode}|"
            f"{'disabled' if self.tensorrt.version == 'unavailable' else 'enabled'}|"
            f"{self.tensorrt.version}|{self.tensorrt.pc_dir}|{self.tensorrt.runtime_dirs}"
        )
        payload = f"{onnx_identity}\n{opencv_identity}\n{tensorrt_identity}\n{dependency_identity}"
        return hashlib.sha256(payload.encode()).hexdigest()

    def _snapshot(self) -> DependencySnapshot:
        opencv_mode = "disabled" if self.opencv.version == "disabled" else self.config.opencv_mode
        trt_mode = "disabled" if self.tensorrt.version in {"disabled", "unavailable"} else "enabled"
        runtime_path = join_unique(
            self.ort.runtime_dirs,
            self.opencv.runtime_dirs,
            self.tensorrt.runtime_dirs,
        )
        return DependencySnapshot(
            signature=self._compute_signature(),
            profile=self.config.profile,
            pkg_config_path=join_unique(
                ":".join(self._active_pkg_config_dirs()), self.base_pkg_config_path
            ),
            runtime_path=runtime_path,
            opencv_mode=opencv_mode,
            tensorrt_mode=trt_mode,
            onnxruntime_version=self.ort.version,
            onnxruntime_pcdir=self.ort.pc_dir,
            onnxruntime_runtime_dirs=self.ort.runtime_dirs,
            onnxruntime_providers=self.ort.providers,
            onnxruntime_provider_libraries=self.ort.provider_libraries,
            opencv_pcdir=self.opencv.pc_dir,
            opencv_runtime_dirs=self.opencv.runtime_dirs,
            tensorrt_pcdir=self.tensorrt.pc_dir,
            tensorrt_runtime_dirs=self.tensorrt.runtime_dirs,
        )

    def prepare(self, requested_profile: str, *, pending_path: Path | None = None) -> DependencySnapshot:
        """解析并准备全部依赖，然后发布 pending 快照。"""

        self.build_dir.mkdir(parents=True, exist_ok=True)
        self.ort = DependencyResult(source=self.config.onnx_source, version="")
        self.opencv = DependencyResult(source=self.config.opencv_source, version="")
        self.tensorrt = DependencyResult(source=self.config.tensorrt_source, version="")
        if self.config.profile == "nvidia":
            self.tensorrt = self._prepare_tensorrt()
            if self.tensorrt.runtime_dirs:
                self._prepend_runtime(self.tensorrt.runtime_dirs)
        self.ort = self._prepare_onnxruntime()
        self.opencv = self._prepare_opencv()
        if self.config.profile != "nvidia":
            self.tensorrt = self._prepare_tensorrt()
        if self.tensorrt.runtime_dirs:
            self._prepend_runtime(self.tensorrt.runtime_dirs)
        snapshot = self._snapshot()
        target = pending_path or self.build_dir / "ssv-deps.env.pending"
        write_snapshot(target, snapshot)
        self._print_summary()
        return snapshot

    def _print_summary(self) -> None:
        onnx_version = self._onnx_config_version if self.ort.source == "managed" else self.ort.version
        onnx_where = (
            f" root={self.context.display_path(self.config.onnx_root)}"
            if self.ort.source == "managed"
            else f" pcdir={self.ort.pc_dir}"
        )
        print(
            f"ONNX Runtime  profile={self.config.profile} source={self.ort.source}"
            f" version={onnx_version} providers={self.ort.providers}{onnx_where}"
        )
        if self.opencv.version == "disabled":
            print("OpenCV        mode=disabled")
        else:
            where = (
                f" root={self.context.display_path(self.config.opencv_root)}"
                if self.opencv.source == "managed"
                else f" pcdir={self.opencv.pc_dir}"
            )
            print(
                f"OpenCV        source={self.opencv.source} mode={self.config.opencv_mode}"
                f" version={self.opencv.version}{where}"
            )
        if self.tensorrt.version in {"disabled", "unavailable"}:
            print(f"TensorRT      source={self.tensorrt.source} mode=disabled status=stub")
        else:
            where = (
                f" root={self.context.display_path(self.config.tensorrt_root)}"
                if self.tensorrt.source == "managed"
                else f" pcdir={self.tensorrt.pc_dir}"
            )
            print(
                f"TensorRT      source={self.tensorrt.source} mode=enabled"
                f" version={self.tensorrt.version}{where}"
            )


def load_dependency_manager(context: ProjectContext, requested_profile: str) -> DependencyManager:
    config = DependencyConfig.from_context(context, requested_profile)
    return DependencyManager(context, config)
