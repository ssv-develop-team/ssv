"""Python 接管的依赖配置与 provider 编排。

编译器、Meson、pkg-config 和 ELF 工具仍属于系统工具；本模块负责配置、探针、缓存、
产物发布，以及 native runner 使用的运行时快照。
"""

from __future__ import annotations

import hashlib
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from ..context import ProjectContext
from ..output import CliError, info, warn

DEFAULT_ONNXRUNTIME_VERSION = "1.25.1"
DEFAULT_OPENCV_VERSION = "4.10.0"
TENSORRT_MANAGED_VERSION = "10.16.1"
TENSORRT_PACKAGE_REVISION = "10.16.1.11-1+cuda13.2"
TENSORRT_CUDNN_REVISION = "9.25.0.15-1"
TENSORRT_CUDA_MAJOR = "13"
TENSORRT_REPOSITORY = "https://developer.download.nvidia.com/compute/cuda/repos/debian12/x86_64"
OPENVINO_VERSION = "2025.4.1"
OPENVINO_ARCHIVE = "openvino_toolkit_ubuntu22_2025.4.1.20426.82bbf0292c5_x86_64.tgz"
OPENVINO_URL = (
    "https://storage.openvinotoolkit.org/repositories/openvino/packages/2025.4.1/linux/"
    + OPENVINO_ARCHIVE
)
ORT_INTEL_ARCHIVE = "onnxruntime-v1.25.1.tar.gz"
ORT_INTEL_URL = "https://github.com/microsoft/onnxruntime/archive/refs/tags/v1.25.1.tar.gz"
OPENCV_URL = "https://github.com/opencv/opencv/archive/refs/tags/{version}.tar.gz"

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

_PROFILES = frozenset({"auto", "cpu", "nvidia", "intel", "amd"})
_SOURCES = frozenset({"managed", "local", "system"})
_PROVIDER_LIBRARY_NAMES = {
    "cpu": (),
    "nvidia": (
        "libonnxruntime_providers_shared.so",
        "libonnxruntime_providers_tensorrt.so",
        "libonnxruntime_providers_cuda.so",
    ),
    "intel": (
        "libonnxruntime_providers_shared.so",
        "libonnxruntime_providers_openvino.so",
    ),
    "amd": (
        "libonnxruntime_providers_shared.so",
        "libonnxruntime_providers_migraphx.so",
    ),
}
_EXPECTED_PROVIDERS = {
    "cpu": ("CPUExecutionProvider",),
    "nvidia": (
        "TensorrtExecutionProvider",
        "CUDAExecutionProvider",
        "CPUExecutionProvider",
    ),
    "intel": ("OpenVINOExecutionProvider", "CPUExecutionProvider"),
    "amd": ("MIGraphXExecutionProvider", "CPUExecutionProvider"),
}
_PROVIDER_PROFILE = {
    "TensorrtExecutionProvider": "nvidia",
    "CUDAExecutionProvider": "nvidia",
    "OpenVINOExecutionProvider": "intel",
    "MIGraphXExecutionProvider": "amd",
}
_SNAPSHOT_KEY_SET = frozenset(SNAPSHOT_KEYS)
_ORT_PROBE_SOURCE = r"""#include <algorithm>
#include <iostream>
#include <onnxruntime_cxx_api.h>
int main() {
    auto providers = Ort::GetAvailableProviders();
    std::sort(providers.begin(), providers.end());
    std::cout << "version=" << OrtGetApiBase()->GetVersionString() << "\nproviders=";
    for (std::size_t i = 0; i < providers.size(); ++i) {
        if (i != 0) std::cout << ",";
        std::cout << providers[i];
    }
    std::cout << "\n";
    return 0;
}
"""


class DependencyError(CliError):
    """依赖策略或产物校验失败。"""


def _error(message: str) -> DependencyError:
    return DependencyError(message)


def validate_profile(profile: str) -> str:
    if profile not in _PROFILES:
        raise _error(f"runtime profile must be auto, cpu, nvidia, intel, or amd: {profile}")
    return profile


def detect_gpu_vendors(sysfs_root: Path | str = "/sys") -> tuple[str, ...]:
    """返回受支持 DRM 设备归一化且去重后的厂商 ID。"""

    root = Path(sysfs_root)
    seen: set[str] = set()
    vendors: list[str] = []
    for vendor_file in sorted(root.glob("class/drm/card*/device/vendor")):
        try:
            value = vendor_file.read_text(encoding="ascii").strip().lower()
        except OSError:
            continue
        value = value.removeprefix("0x")
        if value not in {"10de", "8086", "1002"} or value in seen:
            continue
        seen.add(value)
        vendors.append(value)
    return tuple(vendors)


def resolve_profile(requested: str, vendors: Iterable[str] = ()) -> str:
    validate_profile(requested)
    if requested != "auto":
        return requested
    normalized = {str(value).lower().removeprefix("0x") for value in vendors}
    if "10de" in normalized:
        return "nvidia"
    if "8086" in normalized:
        return "intel"
    if "1002" in normalized:
        return "amd"
    return "cpu"


def expected_providers(profile: str) -> tuple[str, ...]:
    try:
        return _EXPECTED_PROVIDERS[profile]
    except KeyError as exc:
        raise _error(f"resolved runtime profile required for Provider selection: {profile}") from exc


def validate_provider_set(profile: str, providers: str) -> tuple[str, ...]:
    """在保留既有回退策略的前提下校验 provider 集合。"""

    if not providers:
        raise _error(f"{profile} profile returned no ONNX Runtime Providers")
    values = tuple(item.strip() for item in providers.split(","))
    seen: set[str] = set()
    for provider in values:
        if not provider:
            raise _error(f"{profile} profile returned an empty ONNX Runtime Provider name")
        if provider in seen:
            raise _error(f"{profile} profile returned duplicate Provider: {provider}")
        seen.add(provider)
        owner = _PROVIDER_PROFILE.get(provider)
        if owner is not None and owner != profile:
            raise _error(f"{profile} profile must not activate Provider: {provider}")
        if owner is None and provider not in {"CPUExecutionProvider", "AzureExecutionProvider"}:
            raise _error(f"{profile} profile returned an unsupported Provider: {provider}")
    missing = [provider for provider in expected_providers(profile) if provider not in seen]
    if missing:
        raise _error(f"{profile} profile is missing required Provider: {missing[0]}")
    return values


def join_unique(*values: str) -> str:
    result: list[str] = []
    seen: set[str] = set()
    for value in values:
        for item in value.split(":"):
            if item and item not in seen:
                seen.add(item)
                result.append(item)
    return ":".join(result)


def _validate_scalar(name: str, value: str) -> None:
    if "\n" in value or "\r" in value:
        raise _error(f"{name} must not contain a newline")
    if ":" in value:
        raise _error(f"{name} must not contain ':'")


def _validate_path_list(name: str, value: str) -> None:
    if "\n" in value or "\r" in value:
        raise _error(f"{name} must not contain a newline")
    for item in value.split(":"):
        if ":" in item:
            raise _error(f"{name} contains an invalid path: {item}")


def _resolve_path(root: Path, value: str) -> Path:
    path = Path(value).expanduser()
    return (path if path.is_absolute() else root / path).resolve(strict=False)


def _validate_root(project_root: Path, name: str, value: str) -> Path:
    if not value:
        raise _error(f"{name} must not be empty")
    _validate_scalar(name, value)
    path = _resolve_path(project_root, value)
    forbidden = {Path("/"), project_root.resolve(), (project_root / ".deps").resolve()}
    if path in forbidden or (Path.home() != Path("/") and path == Path.home().resolve()):
        raise _error(f"{name} points at a broad or unsafe directory: {value}")
    return path


@dataclass(frozen=True)
class DependencyConfig:
    profile: str
    onnx_source: str
    onnx_version: str
    onnx_root: Path
    opencv_source: str
    opencv_mode: str
    opencv_root: Path
    opencv_include_dir: Path | None
    opencv_lib_dir: Path | None
    tensorrt_source: str
    tensorrt_mode: str
    tensorrt_root: Path
    tensorrt_archive: Path | None
    tensorrt_url: str | None
    cuda_home: Path | None
    extra_pkg_config_path: str

    @classmethod
    def from_context(
        cls,
        context: ProjectContext,
        requested_profile: str,
        *,
        sysfs_root: Path | str = "/sys",
    ) -> DependencyConfig:
        validate_profile(requested_profile)
        vendors = detect_gpu_vendors(sysfs_root) if requested_profile == "auto" else ()
        profile = resolve_profile(requested_profile, vendors)
        environment = context.environment
        explicit = set(environment)

        if "SSV_ONNXRUNTIME_VERSION" in explicit:
            raise _error(
                "SSV_ONNXRUNTIME_VERSION is no longer configurable; select the runtime artifact with --profile"
            )
        default_onnx_source = "system" if profile == "amd" else "managed"
        onnx_source = environment.get("SSV_ONNXRUNTIME_SOURCE", default_onnx_source)
        onnx_version = DEFAULT_ONNXRUNTIME_VERSION + ("-gpu" if profile == "nvidia" else "")
        onnx_root_default = ".deps/onnxruntime-openvino" if profile == "intel" else ".deps/onnxruntime"
        onnx_root_value = environment.get("SSV_ONNXRUNTIME_ROOT", onnx_root_default)

        opencv_source = environment.get("SSV_OPENCV_SOURCE", "managed")
        opencv_mode = environment.get("SSV_OPENCV_MODE", "enabled")
        opencv_root_value = environment.get("SSV_OPENCV_ROOT", ".deps/opencv")
        include_value = environment.get("SSV_OPENCV_INCLUDE_DIR", "")
        lib_value = environment.get("SSV_OPENCV_LIB_DIR", "")

        tensorrt_source = environment.get("SSV_TENSORRT_SOURCE", "managed")
        default_trt_mode = "enabled" if profile == "nvidia" else "auto"
        tensorrt_mode = environment.get("SSV_TENSORRT_MODE", default_trt_mode)
        tensorrt_root_value = environment.get("SSV_TENSORRT_ROOT", ".deps/tensorrt")
        archive_value = environment.get("SSV_TENSORRT_ARCHIVE", "")
        url_value = environment.get("SSV_TENSORRT_URL", "")
        cuda_value = environment.get("CUDA_HOME", "")

        if onnx_source not in _SOURCES:
            raise _error("SSV_ONNXRUNTIME_SOURCE must be managed, local, or system")
        if opencv_source not in _SOURCES:
            raise _error("SSV_OPENCV_SOURCE must be managed, local, or system")
        if opencv_mode not in {"enabled", "disabled"}:
            raise _error("SSV_OPENCV_MODE must be enabled or disabled")
        if tensorrt_source not in {"managed", "system"}:
            raise _error("SSV_TENSORRT_SOURCE must be managed or system")
        if tensorrt_mode not in {"auto", "enabled", "disabled"}:
            raise _error("SSV_TENSORRT_MODE must be auto, enabled, or disabled")
        if archive_value and url_value:
            raise _error("SSV_TENSORRT_ARCHIVE and SSV_TENSORRT_URL cannot both be set")

        if onnx_source == "system" and "SSV_ONNXRUNTIME_ROOT" in explicit:
            raise _error("system ONNX Runtime must not set managed ROOT")
        if onnx_source == "managed":
            if profile == "cpu" and onnx_version.endswith("-gpu"):
                raise _error("CPU profile requires a managed ONNX Runtime CPU package")
            if profile == "nvidia" and not onnx_version.endswith("-gpu"):
                raise _error("NVIDIA profile requires a managed ONNX Runtime GPU package")
            if profile == "intel" and onnx_version.endswith("-gpu"):
                raise _error("Intel profile requires a managed ONNX Runtime CPU source build")
            if profile not in {"cpu", "nvidia", "intel"}:
                raise _error(f"{profile} profile requires system or local ONNX Runtime")

        if opencv_mode == "disabled":
            forbidden = {
                "SSV_OPENCV_SOURCE",
                "SSV_OPENCV_ROOT",
                "SSV_OPENCV_INCLUDE_DIR",
                "SSV_OPENCV_LIB_DIR",
            }
            if explicit & forbidden:
                raise _error("disabled OpenCV must not set SOURCE, ROOT, INCLUDE_DIR, or LIB_DIR")
        elif opencv_source == "system":
            if explicit & {"SSV_OPENCV_ROOT", "SSV_OPENCV_INCLUDE_DIR", "SSV_OPENCV_LIB_DIR"}:
                raise _error("system OpenCV must not set ROOT, INCLUDE_DIR, or LIB_DIR")
        elif opencv_source == "managed":
            if explicit & {"SSV_OPENCV_INCLUDE_DIR", "SSV_OPENCV_LIB_DIR"}:
                raise _error("managed OpenCV must not set INCLUDE_DIR or LIB_DIR")
        else:
            if not include_value:
                raise _error("local OpenCV requires SSV_OPENCV_INCLUDE_DIR")
            if not lib_value:
                raise _error("local OpenCV requires SSV_OPENCV_LIB_DIR")

        if tensorrt_mode == "disabled":
            forbidden = {
                "SSV_TENSORRT_SOURCE",
                "SSV_TENSORRT_ROOT",
                "SSV_TENSORRT_ARCHIVE",
                "SSV_TENSORRT_URL",
                "CUDA_HOME",
            }
            if explicit & forbidden:
                raise _error("disabled TensorRT must not set SOURCE, ROOT, ARCHIVE, URL, or CUDA_HOME")
        elif tensorrt_source == "system":
            forbidden = {"SSV_TENSORRT_ROOT", "SSV_TENSORRT_ARCHIVE", "SSV_TENSORRT_URL", "CUDA_HOME"}
            if explicit & forbidden:
                raise _error("system TensorRT must not set ROOT, ARCHIVE, URL, or CUDA_HOME")
        if profile == "nvidia" and tensorrt_mode != "enabled":
            raise _error("NVIDIA profile requires SSV_TENSORRT_MODE=enabled")

        opencv_include = _resolve_path(context.root, include_value) if include_value else None
        opencv_lib = _resolve_path(context.root, lib_value) if lib_value else None
        if opencv_source == "local":
            assert opencv_include is not None and opencv_lib is not None
            if not opencv_include.is_dir():
                raise _error(f"local OpenCV include directory does not exist: {opencv_include}")
            if not opencv_lib.is_dir():
                raise _error(f"local OpenCV library directory does not exist: {opencv_lib}")
            _validate_scalar("SSV_OPENCV_INCLUDE_DIR", include_value)
            _validate_scalar("SSV_OPENCV_LIB_DIR", lib_value)

        archive = _resolve_path(context.root, archive_value) if archive_value else None
        cuda_home = _resolve_path(context.root, cuda_value) if cuda_value else None
        return cls(
            profile=profile,
            onnx_source=onnx_source,
            onnx_version=onnx_version,
            onnx_root=_validate_root(context.root, "SSV_ONNXRUNTIME_ROOT", onnx_root_value),
            opencv_source=opencv_source,
            opencv_mode=opencv_mode,
            opencv_root=_validate_root(context.root, "SSV_OPENCV_ROOT", opencv_root_value),
            opencv_include_dir=opencv_include,
            opencv_lib_dir=opencv_lib,
            tensorrt_source=tensorrt_source,
            tensorrt_mode=tensorrt_mode,
            tensorrt_root=_validate_root(context.root, "SSV_TENSORRT_ROOT", tensorrt_root_value),
            tensorrt_archive=archive,
            tensorrt_url=url_value or None,
            cuda_home=cuda_home,
            extra_pkg_config_path=environment.get("SSV_EXTRA_PKG_CONFIG_PATH", ""),
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


class DependencyManager:
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

    def _command(
        self,
        argv: Sequence[str | Path],
        *,
        env: Mapping[str, str] | None = None,
        cwd: Path | None = None,
        capture_output: bool = True,
        check: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        command = [str(item) for item in argv]
        try:
            result = subprocess.run(
                command,
                cwd=str(cwd or self.root),
                env=dict(env or self.environment),
                text=True,
                capture_output=capture_output,
                check=False,
            )
        except FileNotFoundError as exc:
            raise _error(f"required command not found: {command[0]}") from exc
        if check and result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise _error(f"command failed ({result.returncode}): {' '.join(command)}{': ' + detail if detail else ''}")
        return result

    def _require_command(self, name: str) -> str:
        path = shutil.which(name)
        if path is None:
            raise _error(f"required command not found: {name}")
        return path

    def _pkg_env(self) -> dict[str, str]:
        self.pkg_config_dirs = self._active_pkg_config_dirs()
        environment = dict(self.environment)
        environment["PKG_CONFIG_PATH"] = join_unique(
            ":".join(self.pkg_config_dirs), self.base_pkg_config_path
        )
        return environment

    def _active_pkg_config_dirs(self) -> list[str]:
        """丢弃原子发布后已不存在的生成 pkg-config 目录。"""

        return [directory for directory in self.pkg_config_dirs if Path(directory).is_dir()]

    def _prepend_pkg_config(self, directory: str) -> None:
        self.pkg_config_dirs = self._active_pkg_config_dirs()
        if directory and directory not in self.pkg_config_dirs:
            self.pkg_config_dirs.insert(0, directory)
        self.environment["PKG_CONFIG_PATH"] = join_unique(
            ":".join(self.pkg_config_dirs), self.base_pkg_config_path
        )

    def _prepend_runtime(self, directories: str) -> None:
        self.environment["LD_LIBRARY_PATH"] = join_unique(
            directories, self.environment.get("LD_LIBRARY_PATH", "")
        )

    def _pkg(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return self._command(["pkg-config", *arguments], env=self._pkg_env())

    @property
    def build_environment(self) -> dict[str, str]:
        """返回传给 Meson 及其编译子进程的环境。"""

        return self._pkg_env()

    def check_base_dependencies(self) -> None:
        required = (
            "gstreamer-1.0",
            "gstreamer-base-1.0",
            "gstreamer-video-1.0",
            "yaml-cpp",
            "hiredis",
            "nlohmann_json",
            "blas",
            "lapack",
        )
        missing = [package for package in required if self._pkg("--exists", package).returncode != 0]
        if missing:
            raise _error(
                "missing C/C++ development dependencies: "
                + ", ".join(missing)
                + "; install GStreamer, yaml-cpp, hiredis, nlohmann-json, BLAS, and LAPACK development packages"
            )

    def _pkg_dir(self, package: str) -> str:
        result = self._pkg("--variable=pcfiledir", package)
        if result.returncode != 0 or not result.stdout.strip():
            raise _error(f"pkg-config package not found: {package}")
        path = Path(result.stdout.strip()).resolve(strict=False)
        if not path.is_dir():
            raise _error(f"pkg-config pcfiledir does not exist for {package}: {path}")
        return str(path)

    def _pkg_version(self, package: str) -> str:
        result = self._pkg("--modversion", package)
        if result.returncode != 0:
            raise _error(f"pkg-config package not found: {package}")
        return result.stdout.strip()

    def _pkg_at_least(self, package: str, minimum: str) -> None:
        result = self._pkg(f"--atleast-version={minimum}", package)
        if result.returncode != 0:
            raise _error(f"system {package} >= {minimum} is required")

    def _pkg_variable(self, package: str, variable: str) -> str:
        result = self._pkg(f"--variable={variable}", package)
        if result.returncode != 0:
            return ""
        return result.stdout.strip()

    def _pkg_lib_dirs(self, package: str) -> list[str]:
        values: list[str] = []
        lib_dir = self._pkg_variable(package, "libdir")
        if lib_dir:
            values.append(lib_dir)
        result = self._pkg("--libs", package)
        for flag in shlex.split(result.stdout) if result.returncode == 0 else ():
            if flag.startswith("-L") and len(flag) > 2:
                values.append(flag[2:])
        return values

    @staticmethod
    def _is_system_dir(directory: Path) -> bool:
        resolved = directory.resolve(strict=False)
        return resolved == Path("/lib") or resolved == Path("/usr/lib") or resolved == Path("/usr/lib64") or any(
            resolved.is_relative_to(parent)
            for parent in (Path("/lib"), Path("/usr/lib"), Path("/usr/lib64"))
        )

    def _runtime_dirs(self, *directories: str | Path) -> str:
        result: list[str] = []
        seen: set[str] = set()
        for item in directories:
            if not item:
                continue
            path = Path(item).resolve(strict=False)
            if not path.is_dir() or self._is_system_dir(path):
                continue
            value = str(path)
            if value not in seen:
                seen.add(value)
                result.append(value)
        return ":".join(result)

    def _write_pc(self, path: Path, lines: Sequence[str]) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    def _compile_probe(self, source: str, package: str, runtime_dirs: str) -> None:
        compiler_value = self.environment.get("CXX", "")
        compiler = compiler_value.split()[0] if compiler_value else "c++"
        self._require_command(compiler)
        with tempfile.TemporaryDirectory(prefix="ssv-deps-probe-") as temporary_name:
            temporary = Path(temporary_name)
            source_file = temporary / "probe.cpp"
            binary = temporary / "probe"
            source_file.write_text(source, encoding="utf-8")
            flags_result = self._pkg("--cflags", "--libs", "--static", package)
            if flags_result.returncode != 0:
                raise _error(f"pkg-config probe flags unavailable for {package}")
            flags = shlex.split(flags_result.stdout)
            compile_result = self._command(
                [compiler, "-std=c++17", source_file, "-o", binary, *flags],
                env=self._pkg_env(),
            )
            if compile_result.returncode != 0:
                detail = (compile_result.stderr or compile_result.stdout).strip()
                raise _error(f"{package} compile probe failed: {detail}")
            environment = dict(self.environment)
            environment["LD_LIBRARY_PATH"] = join_unique(
                runtime_dirs, environment.get("LD_LIBRARY_PATH", "")
            )
            run_result = self._command([binary], env=environment)
            if run_result.returncode != 0:
                detail = (run_result.stderr or run_result.stdout).strip()
                raise _error(f"{package} load probe failed: {detail}")

    def _readelf(self, *arguments: str | Path) -> str:
        self._require_command("readelf")
        result = self._command(["readelf", *arguments])
        if result.returncode != 0:
            raise _error((result.stderr or result.stdout).strip() or "readelf failed")
        return result.stdout

    def _ldd(self, library: Path, *, preload: Path | None = None, check: bool = True) -> str:
        self._require_command("ldd")
        environment = dict(self.environment)
        environment["LD_LIBRARY_PATH"] = join_unique(
            str(library.parent), environment.get("LD_LIBRARY_PATH", "")
        )
        if preload is not None:
            environment["LD_PRELOAD"] = join_unique(
                str(preload), environment.get("LD_PRELOAD", "")
            )
        result = self._command(["ldd", "-r", library], env=environment)
        output = (result.stdout or "") + (result.stderr or "")
        if check and result.returncode != 0:
            raise _error(f"runtime load check failed for {library.name}: {output.strip()}")
        if check:
            missing = re.findall(r"^\s*([^\s]+) => not found$", output, re.MULTILINE)
            symbols = re.findall(r"undefined symbol: ([^\s]+)", output)
            if missing:
                raise _error(f"runtime dependencies unresolved for {library.name}: {','.join(sorted(set(missing)))}")
            if symbols:
                raise _error(f"runtime symbols unresolved for {library.name}: {','.join(sorted(set(symbols)))}")
        return output

    @staticmethod
    def _dependency_paths(ldd_output: str) -> list[Path]:
        paths: list[Path] = []
        for line in ldd_output.splitlines():
            match = re.search(r"=>\s+(/[^\s]+)", line)
            if match is None:
                match = re.match(r"\s*(/[^\s]+)", line)
            if match:
                paths.append(Path(match.group(1)).resolve(strict=False))
        return paths

    def _find_unique(self, root: Path, pattern: str, *, description: str) -> Path:
        matches = [path for path in root.rglob(pattern) if path.is_file()]
        if len(matches) > 1:
            raise _error(f"multiple {description} candidates found under {root}")
        if not matches:
            raise _error(f"{description} not found under {root}")
        return matches[0]

    def _candidate(self, parent: Path, label: str) -> Path:
        parent.mkdir(parents=True, exist_ok=True)
        return Path(tempfile.mkdtemp(prefix=f".{label}.", dir=parent))

    def _atomic_replace(self, candidate: Path, destination: Path) -> None:
        destination.parent.mkdir(parents=True, exist_ok=True)
        backup = destination.with_name(f"{destination.name}.old.{os.getpid()}")
        if backup.exists() or backup.is_symlink():
            self._remove_path(backup)
        if destination.exists() or destination.is_symlink():
            os.replace(destination, backup)
        try:
            os.replace(candidate, destination)
        except BaseException:
            if backup.exists() or backup.is_symlink():
                os.replace(backup, destination)
            raise
        if backup.exists() or backup.is_symlink():
            self._remove_path(backup)

    @staticmethod
    def _remove_path(path: Path) -> None:
        if path.is_symlink() or path.is_file():
            path.unlink(missing_ok=True)
        elif path.is_dir():
            shutil.rmtree(path)

    @staticmethod
    def _is_empty_directory(path: Path) -> bool:
        return not path.exists() or (path.is_dir() and not any(path.iterdir()))

    def _require_replaceable(self, path: Path, validator, *, description: str) -> None:
        if self._is_empty_directory(path):
            return
        try:
            validator(path)
            return
        except DependencyError:
            pass
        if not path.is_dir():
            raise _error(f"refusing to replace non-directory {description}: {path}")
        raise _error(f"refusing to replace non-empty unrecognized {description}: {path}")

    def _download(self, url: str, destination: Path) -> Path:
        if destination.is_file() and destination.stat().st_size > 0:
            return destination
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(f"{destination.name}.tmp.{os.getpid()}")
        temporary.unlink(missing_ok=True)
        info(f"downloading {destination.name}")
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "ssv-cli/0.1"})
            with urllib.request.urlopen(request, timeout=120) as response, temporary.open("wb") as output:
                shutil.copyfileobj(response, output)
            if temporary.stat().st_size == 0:
                raise _error(f"downloaded file is empty: {url}")
            os.replace(temporary, destination)
        except (OSError, urllib.error.URLError) as exc:
            temporary.unlink(missing_ok=True)
            raise _error(f"download failed: {url}: {exc}") from exc
        return destination

    @staticmethod
    def _safe_archive_member(destination: Path, member_name: str) -> Path:
        target = (destination / member_name).resolve(strict=False)
        if not target.is_relative_to(destination.resolve()):
            raise _error(f"archive contains an unsafe path: {member_name}")
        return target

    def _extract_archive(self, archive: Path, destination: Path) -> None:
        self._remove_path(destination)
        destination.mkdir(parents=True, exist_ok=True)
        name = archive.name.lower()
        if name.endswith(".zip"):
            try:
                with zipfile.ZipFile(archive) as source:
                    for member in source.infolist():
                        self._safe_archive_member(destination, member.filename)
                    source.extractall(destination)
            except (OSError, zipfile.BadZipFile) as exc:
                raise _error(f"failed to extract archive: {archive}: {exc}") from exc
            return
        if name.endswith((".tar.gz", ".tgz", ".tar.xz", ".txz", ".tar.bz2", ".tbz2", ".tar")):
            try:
                with tarfile.open(archive, "r:*") as source:
                    members = source.getmembers()
                    for member in members:
                        self._safe_archive_member(destination, member.name)
                    source.extractall(destination)
            except (OSError, tarfile.TarError) as exc:
                raise _error(f"failed to extract archive: {archive}: {exc}") from exc
            return
        if name.endswith((".tar.zst", ".tzst")):
            self._require_command("tar")
            result = self._command(["tar", "--zstd", "-xf", archive, "-C", destination])
            if result.returncode != 0:
                detail = (result.stderr or result.stdout).strip()
                raise _error(f"failed to extract archive: {archive}: {detail}")
            return
        raise _error(f"unsupported archive format: {archive}")

    def _system_result(self, package: str, minimum: str) -> DependencyResult:
        self._pkg_at_least(package, minimum)
        pc_dir = self._pkg_dir(package)
        project_paths = {(self.root / ".deps").resolve(), self.build_dir.resolve()}
        if any(Path(pc_dir).is_relative_to(path) for path in project_paths):
            raise _error(f"system {package} resolved to a project-local artifact: {pc_dir}")
        runtime_dirs = self._runtime_dirs(*self._pkg_lib_dirs(package))
        if package == "opencv4":
            probe = '#include <opencv2/core.hpp>\nint main() { return CV_VERSION[0] == \'\\0\'; }\n'
        elif package == "nvinfer":
            probe = "#include <NvInfer.h>\n#include <cuda_runtime_api.h>\nint main() { return NV_TENSORRT_MAJOR < 0; }\n"
        else:
            raise _error(f"no system dependency probe is defined for {package}")
        self._compile_probe(probe, package, runtime_dirs)
        return DependencyResult(
            source="system",
            version=self._pkg_version(package),
            pc_dir=pc_dir,
            runtime_dirs=runtime_dirs,
        )

    def _validate_elf(self, path: Path, label: str) -> None:
        try:
            self._readelf("-h", path)
        except DependencyError as exc:
            raise _error(f"{label} is not a readable ELF: {path.name}: {exc}") from exc

    # ------------------------------------------------------------------
    # ONNX Runtime

    @staticmethod
    def _normalize_onnx_version(version: str) -> str:
        if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-gpu)?", version) is None:
            raise _error(f"SSV_ONNXRUNTIME_VERSION must be x.y.z or x.y.z-gpu: {version}")
        return version

    @staticmethod
    def _onnx_archive_info(version: str) -> tuple[str, str]:
        machine = platform.machine().lower()
        if machine in {"x86_64", "amd64"}:
            architecture = "x64"
        elif machine in {"aarch64", "arm64"}:
            architecture = "aarch64"
        else:
            raise _error(f"managed ONNX Runtime supports Linux x86_64 and aarch64, got {machine}")
        base = version.removesuffix("-gpu")
        if version.endswith("-gpu"):
            if architecture != "x64":
                raise _error("managed NVIDIA ONNX Runtime supports Linux x86_64 only")
            archive = f"onnxruntime-linux-x64-gpu_cuda13-{base}.tgz"
        else:
            archive = f"onnxruntime-linux-{architecture}-{base}.tgz"
        return archive, f"https://github.com/microsoft/onnxruntime/releases/download/v{base}/{archive}"

    @staticmethod
    def _onnx_libdir(root: Path) -> Path:
        matches: list[Path] = []
        for name in ("lib", "lib64"):
            directory = root / name
            if not directory.is_dir():
                continue
            if any(path.name == "libonnxruntime.so" or path.name.startswith("libonnxruntime.so.") for path in directory.iterdir()):
                matches.append(directory)
        if len(matches) > 1:
            raise _error(f"multiple ONNX Runtime library directories found under {root}")
        if not matches:
            raise _error(f"ONNX Runtime library not found under {root}")
        return matches[0].resolve()

    def _make_onnx_pc(self, root: Path, version: str, include_dir: Path, lib_dir: Path) -> Path:
        pc_dir = root / "lib" / "pkgconfig"
        self._write_pc(
            pc_dir / "onnxruntime.pc",
            (
                f"prefix={root}",
                "exec_prefix=${prefix}",
                f"libdir={lib_dir}",
                f"includedir={include_dir}",
                "",
                "Name: onnxruntime",
                "Description: ONNX Runtime C/C++ inference runtime",
                f"Version: {version.removesuffix('-gpu')}",
                "Libs: -L${libdir} -lonnxruntime",
                "Cflags: -I${includedir}",
            ),
        )
        return pc_dir.resolve()

    def _ort_provider_libraries(self, profile: str, lib_dir: Path) -> str:
        libraries: list[str] = []
        for name in _PROVIDER_LIBRARY_NAMES[profile]:
            path = lib_dir / name
            if not path.is_file():
                raise _error(f"{profile} profile is missing Provider library: {name}")
            self._validate_elf(path, f"{profile} profile Provider library")
            if name != "libonnxruntime_providers_shared.so":
                symbols = self._readelf("--dyn-syms", "--wide", path)
                found = any(
                    "GLOBAL" in line
                    and "DEFAULT" in line
                    and "UND" not in line
                    and re.search(r"\bGetProvider(?:@|\s|$)", line) is not None
                    for line in symbols.splitlines()
                )
                if not found:
                    raise _error(f"{profile} profile Provider library has no GetProvider entry point: {name}")
            value = str(path.resolve())
            if value not in libraries:
                libraries.append(value)
        return ":".join(libraries)

    def _ort_runtime_dirs(self, profile: str, lib_dir: Path, provider_libraries: str) -> str:
        libraries = [lib_dir / "libonnxruntime.so"]
        provider_paths = [Path(value) for value in provider_libraries.split(":") if value]
        libraries.extend(provider_paths)
        dependency_dirs: list[Path] = []
        shared = lib_dir / "libonnxruntime_providers_shared.so"
        for library in libraries:
            if not library.is_file():
                raise _error(f"ONNX Runtime link library not found: {library}")
            self._validate_elf(library, "ONNX Runtime library")
            preload = shared if library.name.startswith("libonnxruntime_providers_") and library != shared else None
            output = self._ldd(library, preload=preload)
            dependency_dirs.extend(path.parent for path in self._dependency_paths(output))
        return self._runtime_dirs(lib_dir, *dependency_dirs)

    def _ort_probe(self, runtime_dirs: str) -> tuple[str, str]:
        with tempfile.TemporaryDirectory(prefix="ssv-ort-probe-") as temporary_name:
            temporary = Path(temporary_name)
            source_file = temporary / "probe.cpp"
            source_file.write_text(_ORT_PROBE_SOURCE, encoding="utf-8")
            compiler_value = self.environment.get("CXX", "")
            compiler = compiler_value.split()[0] if compiler_value else "c++"
            flags_result = self._pkg("--cflags", "--libs", "--static", "onnxruntime")
            if flags_result.returncode != 0:
                raise _error("ONNX Runtime probe flags are unavailable")
            binary = temporary / "probe"
            compile_result = self._command(
                [compiler, "-std=c++17", source_file, "-o", binary, *shlex.split(flags_result.stdout)],
                env=self._pkg_env(),
            )
            if compile_result.returncode != 0:
                detail = (compile_result.stderr or compile_result.stdout).strip()
                raise _error(f"ONNX Runtime compile/load probe failed: {detail}")
            environment = dict(self.environment)
            environment["LD_LIBRARY_PATH"] = join_unique(runtime_dirs, environment.get("LD_LIBRARY_PATH", ""))
            result = self._command([binary], env=environment)
            if result.returncode != 0:
                detail = (result.stderr or result.stdout).strip()
                raise _error(f"ONNX Runtime compile/load probe failed: {detail}")
            values: dict[str, str] = {}
            for line in result.stdout.splitlines():
                key, separator, value = line.partition("=")
                if not separator or key not in {"version", "providers"} or key in values:
                    raise _error(f"ONNX Runtime probe returned an invalid line: {line}")
                values[key] = value
            if set(values) != {"version", "providers"}:
                raise _error("ONNX Runtime probe returned an incomplete result")
            return values["version"], values["providers"]

    def _validate_onnx_artifact(
        self,
        profile: str,
        expected_version: str,
        include_dir: Path,
        lib_dir: Path,
        expected_pc_dir: Path,
    ) -> DependencyResult:
        if not (include_dir / "onnxruntime_cxx_api.h").is_file():
            raise _error(f"ONNX Runtime header not found: {include_dir / 'onnxruntime_cxx_api.h'}")
        link_library = lib_dir / "libonnxruntime.so"
        if not link_library.is_file():
            raise _error(f"ONNX Runtime link library not found: {link_library}")
        actual_pc_dir = Path(self._pkg_dir("onnxruntime"))
        if actual_pc_dir != expected_pc_dir.resolve():
            raise _error(f"onnxruntime pkg-config source mismatch: expected {expected_pc_dir}, got {actual_pc_dir}")
        pc_version = self._pkg_version("onnxruntime")
        if pc_version != expected_version:
            raise _error(f"ONNX Runtime pkg-config ABI mismatch: expected {expected_version}, got {pc_version}")
        provider_libraries = self._ort_provider_libraries(profile, lib_dir)
        runtime_dirs = self._ort_runtime_dirs(profile, lib_dir, provider_libraries)
        runtime_version, providers = self._ort_probe(runtime_dirs)
        if runtime_version != expected_version:
            raise _error(f"ONNX Runtime ABI mismatch: expected {expected_version}, got {runtime_version or 'empty'}")
        validate_provider_set(profile, providers)
        return DependencyResult(
            source=self.config.onnx_source,
            version=runtime_version,
            pc_dir=str(actual_pc_dir),
            runtime_dirs=runtime_dirs,
            providers=providers,
            provider_libraries=provider_libraries,
            include_dir=str(include_dir.resolve()),
            lib_dir=str(lib_dir.resolve()),
        )

    def _validate_onnx_layout(self, root: Path, expected_version: str, profile: str) -> DependencyResult:
        include_dir = root / "include"
        lib_dir = self._onnx_libdir(root)
        version_file = root / "VERSION_NUMBER"
        if not version_file.is_file():
            raise _error(f"ONNX Runtime VERSION_NUMBER not found under {root}")
        actual_version = version_file.read_text(encoding="utf-8").strip()
        expected_base = expected_version.removesuffix("-gpu")
        if actual_version != expected_base:
            raise _error(f"ONNX Runtime version mismatch: expected {expected_base}, got {actual_version}")
        if profile == "cpu" and expected_version.endswith("-gpu"):
            raise _error("CPU profile cannot use an ONNX Runtime GPU archive")
        if profile == "nvidia" and not expected_version.endswith("-gpu"):
            raise _error("NVIDIA profile requires an ONNX Runtime GPU archive")
        if profile == "intel" and expected_version.endswith("-gpu"):
            raise _error("Intel profile cannot use an ONNX Runtime GPU archive")
        link_library = lib_dir / "libonnxruntime.so"
        if not link_library.exists():
            versioned = sorted(lib_dir.glob("libonnxruntime.so.*"))
            if not versioned:
                raise _error(f"ONNX Runtime link library not found under {lib_dir}")
            link_library.symlink_to(versioned[0].name)
        pc_dir = self._make_onnx_pc(root, expected_version, include_dir, lib_dir)
        self._prepend_pkg_config(str(pc_dir))
        return self._validate_onnx_artifact(profile, expected_base, include_dir, lib_dir, pc_dir)

    def _stage_onnx_archive(self, root: Path, archive: Path, expected_version: str, profile: str) -> Path:
        candidate = self._candidate(root.parent, f"onnxruntime-{expected_version}")
        try:
            self._extract_archive(archive, candidate)
            header = next(iter(candidate.rglob("onnxruntime_cxx_api.h")), None)
            if header is None:
                raise _error("ONNX Runtime archive has no expected header")
            extracted_root = header.parent.parent
            if extracted_root != candidate:
                normalized = self._candidate(root.parent, "onnxruntime-normalize")
                shutil.copytree(extracted_root, normalized, dirs_exist_ok=True)
                self._remove_path(candidate)
                candidate = normalized
            self._validate_onnx_layout(candidate, expected_version, profile)
            return candidate
        except BaseException:
            self._remove_path(candidate)
            raise

    def _prepare_local_onnx(self) -> DependencyResult:
        root = self.config.onnx_root
        expected = self.config.onnx_version.removesuffix("-gpu")
        if not root.is_dir():
            raise _error(f"local ONNX Runtime artifact not found: {root}")
        version_file = root / "VERSION_NUMBER"
        if not version_file.is_file():
            raise _error(f"local ONNX Runtime VERSION_NUMBER not found: {root}")
        actual = version_file.read_text(encoding="utf-8").strip()
        if actual != expected:
            raise _error(f"local ONNX Runtime version mismatch: expected {expected}, got {actual}")
        include_dir = root / "include"
        lib_dir = self._onnx_libdir(root)
        adapter_root = self.build_dir / "ssv-deps" / f"onnxruntime-{self.config.profile}"
        pc_dir = self._make_onnx_pc(adapter_root, expected, include_dir, lib_dir)
        self._prepend_pkg_config(str(pc_dir))
        result = self._validate_onnx_artifact(self.config.profile, expected, include_dir, lib_dir, pc_dir)
        return DependencyResult(
            source="local",
            version=result.version,
            pc_dir=result.pc_dir,
            runtime_dirs=result.runtime_dirs,
            providers=result.providers,
            provider_libraries=result.provider_libraries,
            include_dir=result.include_dir,
            lib_dir=result.lib_dir,
        )

    def _prepare_system_onnx(self) -> DependencyResult:
        profile = self.config.profile
        expected = self.config.onnx_version.removesuffix("-gpu")
        if self._pkg("--exists", "onnxruntime").returncode != 0:
            raise _error(f"system ONNX Runtime artifact is required for profile={profile}")
        pc_dir = Path(self._pkg_dir("onnxruntime"))
        project_paths = {(self.root / ".deps").resolve(), self.build_dir.resolve()}
        if any(pc_dir.is_relative_to(path) for path in project_paths):
            raise _error(f"system ONNX Runtime resolved to a project-local artifact: {pc_dir}")
        include_dir = Path(self._pkg_variable("onnxruntime", "includedir"))
        lib_dir = Path(self._pkg_variable("onnxruntime", "libdir"))
        if not include_dir.is_dir() or not lib_dir.is_dir():
            raise _error("system ONNX Runtime pkg-config must define existing includedir and libdir")
        return self._validate_onnx_artifact(profile, expected, include_dir, lib_dir, pc_dir)

    def _prepare_openvino(self, openvino_dir: Path) -> None:
        if (openvino_dir / "runtime/cmake/OpenVINOConfig.cmake").is_file() and (
            openvino_dir / "runtime/lib/intel64/libopenvino.so"
        ).is_file():
            return
        cache = self.root / ".deps" / "downloads" / "openvino" / OPENVINO_VERSION / OPENVINO_ARCHIVE
        self._download(OPENVINO_URL, cache)
        candidate = self._candidate(openvino_dir.parent, f"openvino-{OPENVINO_VERSION}")
        try:
            self._extract_archive(cache, candidate)
            config_file = next(iter(candidate.rglob("OpenVINOConfig.cmake")), None)
            if config_file is None:
                raise _error("OpenVINO archive has no OpenVINOConfig.cmake")
            extracted_root = config_file.parent.parent.parent
            if extracted_root != candidate:
                normalized = self._candidate(openvino_dir.parent, "openvino-normalize")
                shutil.copytree(extracted_root, normalized, dirs_exist_ok=True)
                self._remove_path(candidate)
                candidate = normalized
            if not (
                candidate / "runtime/cmake/OpenVINOConfig.cmake"
            ).is_file() or not (candidate / "runtime/lib/intel64/libopenvino.so").is_file():
                raise _error("OpenVINO archive layout is incomplete")
            self._atomic_replace(candidate, openvino_dir)
        except BaseException:
            self._remove_path(candidate)
            raise

    def _prepare_intel_onnx(self) -> DependencyResult:
        workspace = self.config.onnx_root
        source_parent = workspace / "source"
        source_dir = source_parent / f"onnxruntime-{DEFAULT_ONNXRUNTIME_VERSION}"
        build_dir = workspace / "build"
        install_dir = workspace / "install"
        openvino_dir = workspace / "openvino"
        if install_dir.is_dir():
            try:
                return self._validate_onnx_layout(install_dir, self.config.onnx_version, "intel")
            except DependencyError:
                pass
        self._require_replaceable(install_dir, lambda path: self._validate_onnx_layout(path, self.config.onnx_version, "intel"), description="ONNX Runtime install root")
        self._prepare_openvino(openvino_dir)
        self._require_command("cmake")
        self._require_command(sys.executable)
        compiler_value = self.environment.get("CXX", "")
        compiler = compiler_value.split()[0] if compiler_value else next((candidate for candidate in ("c++", "g++", "clang++") if shutil.which(candidate)), "")
        if not compiler:
            raise _error("ONNX Runtime source build requires a C++ compiler (c++, g++, or clang++)")
        self.environment["CXX"] = compiler
        if not (shutil.which("make") or shutil.which("ninja")):
            raise _error("ONNX Runtime source build requires make or ninja")
        if not source_dir.is_dir():
            cache = self.root / ".deps" / "downloads" / "onnxruntime" / DEFAULT_ONNXRUNTIME_VERSION / ORT_INTEL_ARCHIVE
            self._download(ORT_INTEL_URL, cache)
            candidate = self._candidate(source_parent, f"onnxruntime-source-{DEFAULT_ONNXRUNTIME_VERSION}")
            try:
                self._extract_archive(cache, candidate)
                extracted = candidate / f"onnxruntime-{DEFAULT_ONNXRUNTIME_VERSION}"
                if not (extracted / "tools/ci_build/build.py").is_file():
                    raise _error("ONNX Runtime source archive has no expected build.py")
                self._atomic_replace(extracted, source_dir)
                self._remove_path(candidate)
            except BaseException:
                self._remove_path(candidate)
                raise
        jobs = self._build_jobs("SSV_ONNXRUNTIME_BUILD_JOBS")
        build_candidate = self._candidate(build_dir.parent, f"onnxruntime-build-{DEFAULT_ONNXRUNTIME_VERSION}")
        install_candidate = self._candidate(install_dir.parent, f"onnxruntime-install-{DEFAULT_ONNXRUNTIME_VERSION}")
        command = [
            sys.executable,
            source_dir / "tools/ci_build/build.py",
            "--config", "Release",
            "--build_dir", build_candidate,
            "--use_openvino", "GPU",
            "--build_shared_lib",
            "--skip_tests",
            "--compile_no_warning_as_error",
            "--parallel", str(jobs),
            "--cmake_extra_defines",
            f"OpenVINO_DIR={openvino_dir / 'runtime/cmake'}",
            "FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER",
        ]
        if shutil.which("ninja"):
            command.extend(["--cmake_generator", "Ninja"])
        result = self._command(command)
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise _error(f"ONNX Runtime OpenVINO source build failed: {detail[-4000:]}")
        config_build = build_candidate / "Release"
        include_dir = install_candidate / "include"
        lib_dir = install_candidate / "lib"
        include_dir.mkdir(parents=True, exist_ok=True)
        lib_dir.mkdir(parents=True, exist_ok=True)
        source_include = source_dir / "include/onnxruntime/core/session"
        if not source_include.is_dir():
            raise _error(f"ONNX Runtime source headers not found: {source_include}")
        shutil.copytree(source_include, include_dir, dirs_exist_ok=True)
        provider_include = source_dir / "include/onnxruntime/core/providers"
        (include_dir / "core/providers").mkdir(parents=True, exist_ok=True)
        shutil.copytree(provider_include, include_dir / "core/providers", dirs_exist_ok=True)
        for name in (
            "libonnxruntime.so*",
            "libonnxruntime_providers_shared.so*",
            "libonnxruntime_providers_openvino.so*",
        ):
            matches = list(config_build.glob(name))
            if not matches:
                raise _error(f"ONNX Runtime build artifact not found: {config_build / name}")
            for match in matches:
                target = lib_dir / match.name
                if match.is_symlink():
                    target.symlink_to(os.readlink(match))
                else:
                    shutil.copy2(match, target)
        (install_candidate / "VERSION_NUMBER").write_text(DEFAULT_ONNXRUNTIME_VERSION + "\n", encoding="utf-8")
        try:
            result = self._validate_onnx_layout(install_candidate, self.config.onnx_version, "intel")
            self._atomic_replace(build_candidate, build_dir)
            self._atomic_replace(install_candidate, install_dir)
            return self._validate_onnx_layout(install_dir, self.config.onnx_version, "intel")
        except BaseException:
            self._remove_path(build_candidate)
            self._remove_path(install_candidate)
            raise

    def _prepare_onnxruntime(self) -> DependencyResult:
        profile = self.config.profile
        if self.config.onnx_source == "system":
            return self._prepare_system_onnx()
        if self.config.onnx_source == "local":
            return self._prepare_local_onnx()
        if profile == "intel":
            return self._prepare_intel_onnx()
        root = self.config.onnx_root
        expected = self.config.onnx_version
        try:
            if root.is_dir():
                return self._validate_onnx_layout(root, expected, profile)
        except DependencyError:
            pass
        self._require_replaceable(root, lambda path: self._validate_onnx_layout(path, expected, profile), description="ONNX Runtime root")
        archive_name, url = self._onnx_archive_info(expected)
        cache = self.root / ".deps" / "downloads" / "onnxruntime" / expected / archive_name
        self._download(url, cache)
        try:
            candidate = self._stage_onnx_archive(root, cache, expected, profile)
        except DependencyError:
            warn(f"cached ONNX Runtime archive is invalid; downloading it once more: {archive_name}")
            cache.unlink(missing_ok=True)
            self._download(url, cache)
            candidate = self._stage_onnx_archive(root, cache, expected, profile)
        self._atomic_replace(candidate, root)
        return self._validate_onnx_layout(root, expected, profile)

    # ------------------------------------------------------------------
    # OpenCV

    _OPENCV_MODULES = ("core", "imgproc", "video", "calib3d", "features2d", "flann", "dnn")

    @staticmethod
    def _opencv_include_dir(root: Path) -> Path:
        matches = list(root.rglob("opencv2/core.hpp"))
        if not matches:
            raise _error(f"OpenCV headers not found under {root}")
        return matches[0].parent.parent.resolve()

    @staticmethod
    def _opencv_lib_dir(root: Path) -> Path:
        matches = list(root.rglob("libopencv_core.so")) + list(root.rglob("libopencv_core.so.*"))
        if not matches:
            raise _error(f"OpenCV libraries not found under {root}")
        return matches[0].parent.resolve()

    @staticmethod
    def _opencv_library(lib_dir: Path, module: str) -> Path:
        matches = [path for path in (lib_dir / f"libopencv_{module}.so", *lib_dir.glob(f"libopencv_{module}.so.*")) if path.exists()]
        if not matches:
            raise _error(f"OpenCV library missing: libopencv_{module}.so")
        return matches[0]

    def _validate_opencv_libraries(self, lib_dir: Path) -> None:
        libraries = [self._opencv_library(lib_dir, module) for module in self._OPENCV_MODULES]
        for library in libraries:
            self._validate_elf(library, "OpenCV library")
            dynamic = self._readelf("-d", library)
            needed = re.findall(r"Shared library: \[(libopencv_[^]]+)\]", dynamic)
            for dependency in needed:
                stem = dependency.split(".so", 1)[0]
                if not any(lib_dir.glob(f"{stem}.so")) and not any(lib_dir.glob(f"{stem}.so.*")):
                    raise _error(f"OpenCV runtime closure is incomplete: {library.name} needs {dependency}")
            self._ldd(library)

    def _make_opencv_pc(self, root: Path, version: str, include_dir: Path, lib_dir: Path, pc_dir: Path | None = None) -> Path:
        math_result = self._pkg("--libs", "cblas", "lapack", "blas")
        if math_result.returncode != 0:
            math_result = self._pkg("--libs", "lapack", "blas")
        if math_result.returncode != 0:
            raise _error("OpenCV pkg-config requires cblas/lapack/blas")
        target_dir = pc_dir or (root / "lib/pkgconfig")
        self._write_pc(
            target_dir / "opencv4.pc",
            (
                f"prefix={root}",
                "exec_prefix=${prefix}",
                f"libdir={lib_dir}",
                f"includedir={include_dir}",
                "",
                "Name: opencv4",
                "Description: OpenCV runtime",
                f"Version: {version}",
                "Libs: -L${libdir} -Wl,-rpath-link,${libdir} -lopencv_calib3d -lopencv_video -lopencv_features2d -lopencv_flann -lopencv_imgproc -lopencv_core " + math_result.stdout.strip(),
                'Cflags: -I"${includedir}"',
            ),
        )
        return target_dir.resolve()

    def _probe_opencv(self, expected_version: str, lib_dir: Path, runtime_dirs: str) -> None:
        source = f'''#include <opencv2/core.hpp>
#include <string>
int main() {{
    return std::string(CV_VERSION) == "{expected_version}" &&
                   cv::getVersionString() == "{expected_version}" ? 0 : 1;
}}
'''
        self._compile_probe(source, "opencv4", runtime_dirs)

    def _validate_opencv_layout(self, root: Path, expected_version: str) -> DependencyResult:
        include_dir = self._opencv_include_dir(root)
        lib_dir = self._opencv_lib_dir(root)
        self._validate_opencv_libraries(lib_dir)
        pc_dir = self._make_opencv_pc(root, expected_version, include_dir, lib_dir)
        self._prepend_pkg_config(str(pc_dir))
        actual_pc_dir = Path(self._pkg_dir("opencv4"))
        if actual_pc_dir != pc_dir:
            raise _error(f"OpenCV pkg-config source mismatch: {actual_pc_dir}")
        runtime_dirs = self._runtime_dirs(lib_dir)
        self._probe_opencv(expected_version, lib_dir, runtime_dirs)
        actual_version = self._pkg_version("opencv4")
        return DependencyResult(
            source=self.config.opencv_source,
            version=actual_version,
            pc_dir=str(actual_pc_dir),
            runtime_dirs=runtime_dirs,
            include_dir=str(include_dir),
            lib_dir=str(lib_dir),
        )

    def _prepare_local_opencv(self) -> DependencyResult:
        assert self.config.opencv_include_dir is not None
        assert self.config.opencv_lib_dir is not None
        requested_include = self.config.opencv_include_dir
        include_dir = requested_include if (requested_include / "opencv2/core.hpp").is_file() else requested_include / "opencv4"
        if not (include_dir / "opencv2/core.hpp").is_file():
            raise _error(f"local OpenCV headers not found under: {requested_include}")
        lib_dir = self.config.opencv_lib_dir.resolve()
        self._validate_opencv_libraries(lib_dir)
        root = self.config.opencv_root / "local"
        pc_dir = self._make_opencv_pc(root, DEFAULT_OPENCV_VERSION, include_dir, lib_dir, root / "lib/pkgconfig")
        self._prepend_pkg_config(str(pc_dir))
        runtime_dirs = self._runtime_dirs(lib_dir)
        self._probe_opencv(DEFAULT_OPENCV_VERSION, lib_dir, runtime_dirs)
        return DependencyResult(
            source="local",
            version=self._pkg_version("opencv4"),
            pc_dir=str(Path(self._pkg_dir("opencv4"))),
            runtime_dirs=runtime_dirs,
            include_dir=str(include_dir.resolve()),
            lib_dir=str(lib_dir),
        )

    def _opencv_jobs(self) -> int:
        return self._build_jobs("SSV_OPENCV_BUILD_JOBS")

    def _build_jobs(self, variable: str) -> int:
        value = self.environment.get(variable, "")
        if not value:
            value = str(os.cpu_count() or 1)
        if not value.isdecimal() or int(value) <= 0:
            raise _error(f"{variable} must be a positive integer")
        return int(value)

    def _prepare_managed_opencv(self) -> DependencyResult:
        workspace = self.config.opencv_root
        source_parent = workspace / "source"
        source_dir = source_parent / f"opencv-{DEFAULT_OPENCV_VERSION}"
        build_dir = workspace / "build"
        install_dir = workspace / "install"
        if install_dir.is_dir():
            try:
                return self._validate_opencv_layout(install_dir, DEFAULT_OPENCV_VERSION)
            except DependencyError:
                pass
        self._require_replaceable(install_dir, lambda path: self._validate_opencv_layout(path, DEFAULT_OPENCV_VERSION), description="OpenCV install root")
        if not source_dir.is_dir():
            cache = self.root / ".deps" / "downloads" / "opencv" / DEFAULT_OPENCV_VERSION / f"opencv-{DEFAULT_OPENCV_VERSION}.tar.gz"
            self._download(OPENCV_URL.format(version=DEFAULT_OPENCV_VERSION), cache)
            candidate = self._candidate(source_parent, f"opencv-source-{DEFAULT_OPENCV_VERSION}")
            try:
                self._extract_archive(cache, candidate)
                extracted = candidate / f"opencv-{DEFAULT_OPENCV_VERSION}"
                if not (extracted / "CMakeLists.txt").is_file():
                    raise _error("OpenCV source archive has no expected CMakeLists.txt")
                self._atomic_replace(extracted, source_dir)
                self._remove_path(candidate)
            except BaseException:
                self._remove_path(candidate)
                raise
        self._require_command("cmake")
        if not (shutil.which("make") or shutil.which("ninja")):
            raise _error("OpenCV source build requires make or ninja")
        compiler_value = self.environment.get("CXX", "")
        compiler = compiler_value.split()[0] if compiler_value else next((candidate for candidate in ("c++", "g++", "clang++") if shutil.which(candidate)), "")
        if not compiler:
            raise _error("OpenCV source build requires a C++ compiler (c++, g++, or clang++)")
        self.environment["CXX"] = compiler
        build_candidate = self._candidate(build_dir.parent, f"opencv-build-{DEFAULT_OPENCV_VERSION}")
        install_candidate = self._candidate(install_dir.parent, f"opencv-install-{DEFAULT_OPENCV_VERSION}")
        generator = ["-G", "Ninja"] if not shutil.which("make") and shutil.which("ninja") else []
        cmake_options = [
            "-S", source_dir,
            "-B", build_candidate,
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_INSTALL_PREFIX={install_dir}",
            "-DCMAKE_INSTALL_LIBDIR=lib",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
            "-DBUILD_opencv_core=ON", "-DBUILD_opencv_imgproc=ON", "-DBUILD_opencv_video=ON",
            "-DBUILD_opencv_calib3d=ON", "-DBUILD_opencv_features2d=ON", "-DBUILD_opencv_flann=ON",
            "-DBUILD_opencv_dnn=ON", "-DBUILD_opencv_imgcodecs=OFF", "-DBUILD_opencv_videoio=OFF",
            "-DBUILD_opencv_apps=OFF", "-DBUILD_opencv_gapi=OFF", "-DBUILD_opencv_highgui=OFF",
            "-DBUILD_opencv_ml=OFF", "-DBUILD_opencv_objdetect=OFF", "-DBUILD_opencv_photo=OFF",
            "-DBUILD_opencv_stitching=OFF", "-DBUILD_opencv_ts=OFF", "-DBUILD_PROTOBUF=ON",
            "-DBUILD_TESTS=OFF", "-DBUILD_PERF_TESTS=OFF", "-DBUILD_EXAMPLES=OFF", "-DBUILD_DOCS=OFF",
            "-DBUILD_JAVA=OFF", "-DBUILD_OBJC=OFF", "-DWITH_EIGEN=OFF", "-DWITH_1394=OFF",
            "-DWITH_FFMPEG=OFF", "-DWITH_GSTREAMER=OFF", "-DWITH_GTK=OFF", "-DWITH_CUDA=OFF",
            "-DWITH_CUDNN=OFF", "-DWITH_IPP=OFF", "-DWITH_ITT=OFF", "-DWITH_JPEG=OFF",
            "-DWITH_PNG=OFF", "-DWITH_TIFF=OFF", "-DWITH_WEBP=OFF", "-DWITH_OPENEXR=OFF",
            "-DWITH_OPENCL=OFF", "-DWITH_OPENGL=OFF", "-DWITH_OPENMP=OFF", "-DWITH_V4L=OFF",
            "-DWITH_VULKAN=OFF", "-DOPENCV_GENERATE_PKGCONFIG=OFF", "-DCPU_BASELINE=DETECT", "-DCV_TRACE=OFF",
        ]
        configure = self._command(["cmake", *generator, *cmake_options])
        if configure.returncode != 0:
            detail = (configure.stderr or configure.stdout).strip()
            raise _error(f"OpenCV CMake configuration failed: {detail[-4000:]}")
        build = self._command(["cmake", "--build", build_candidate, "--parallel", str(self._opencv_jobs())])
        if build.returncode != 0:
            detail = (build.stderr or build.stdout).strip()
            raise _error(f"OpenCV compilation failed: {detail[-4000:]}")
        install = self._command(["cmake", "--install", build_candidate, "--prefix", install_candidate])
        if install.returncode != 0:
            detail = (install.stderr or install.stdout).strip()
            raise _error(f"OpenCV installation failed: {detail[-4000:]}")
        try:
            self._validate_opencv_layout(install_candidate, DEFAULT_OPENCV_VERSION)
            self._atomic_replace(build_candidate, build_dir)
            self._atomic_replace(install_candidate, install_dir)
            return self._validate_opencv_layout(install_dir, DEFAULT_OPENCV_VERSION)
        except BaseException:
            self._remove_path(build_candidate)
            self._remove_path(install_candidate)
            raise

    def _prepare_opencv(self) -> DependencyResult:
        if self.config.opencv_mode == "disabled":
            return DependencyResult(source="disabled", version="disabled")
        if self.config.opencv_source == "system":
            return self._system_result("opencv4", "4.5")
        if self.config.opencv_source == "local":
            return self._prepare_local_opencv()
        return self._prepare_managed_opencv()

    # ------------------------------------------------------------------
    # TensorRT and CUDA

    @staticmethod
    def _tensorrt_manifest() -> tuple[tuple[str, str], ...]:
        return (
            (
                f"libnvinfer-headers-dev_{TENSORRT_PACKAGE_REVISION}_amd64.deb",
                "4945a01b9be143091c89fd8366224159cd01648c25befabe7e1271ff6b9774ca",
            ),
            (
                f"libnvinfer10_{TENSORRT_PACKAGE_REVISION}_amd64.deb",
                "8232ccdc8be82815411879a589ba08df039b9f01a997effdb3f60d3b8a05e6bd",
            ),
            (
                f"libnvonnxparsers10_{TENSORRT_PACKAGE_REVISION}_amd64.deb",
                "1f70eb168ec0c5a2714585e2458464476d6529ef11fc54cad51fac2396a09e6f",
            ),
            (
                f"libcudnn9-cuda-13_{TENSORRT_CUDNN_REVISION}_amd64.deb",
                "5b2ef35d332c903fb81f000c883c36f41fd57de3972aa82e007f944478db0099",
            ),
        )

    def _tensorrt_find_library(self, root: Path, soname: str) -> Path:
        stem = soname.split(".so", 1)[0]
        matches: list[Path] = []
        for candidate in root.rglob(f"{stem}.so*"):
            if not candidate.is_file():
                continue
            try:
                dynamic = self._readelf("-d", candidate)
            except DependencyError:
                continue
            if re.search(rf"Library soname: \[{re.escape(soname)}\]", dynamic):
                resolved = candidate.resolve()
                if resolved not in matches:
                    matches.append(resolved)
        if len(matches) > 1:
            raise _error(f"multiple NVIDIA libraries provide {soname} under {root}")
        if not matches:
            raise _error(f"NVIDIA runtime {soname} not found under {root}")
        return matches[0]

    @staticmethod
    def _macro_value(header: Path, macro: str) -> str:
        values: dict[str, str] = {}
        for raw_line in header.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = raw_line.split(maxsplit=2)
            if len(parts) == 3 and parts[0] == "#define":
                values[parts[1]] = parts[2].strip()
        value = values.get(macro, "")
        for _ in range(5):
            if value.isdecimal():
                return value
            if not value or value not in values:
                return ""
            value = values[value]
        return ""

    def _tensorrt_version(self, header: Path) -> str:
        major = self._macro_value(header, "NV_TENSORRT_MAJOR")
        minor = self._macro_value(header, "NV_TENSORRT_MINOR")
        patch = self._macro_value(header, "NV_TENSORRT_PATCH")
        if major and minor and patch:
            return f"{major}.{minor}.{patch}"
        encoded = self._macro_value(header, "NV_TENSORRT_VERSION_INT")
        if encoded.isdecimal():
            number = int(encoded)
            return f"{number // 10000}.{(number // 100) % 100}.{number % 100}"
        raise _error(f"unable to read TensorRT version from {header}")

    def _cuda_version(self, header: Path) -> str:
        encoded = self._macro_value(header, "CUDART_VERSION")
        if not encoded.isdecimal():
            raise _error(f"unable to read CUDA Runtime version from {header}")
        number = int(encoded)
        return f"{number // 1000}.{(number % 1000) // 10}"

    def _locate_cuda(self, root: Path) -> tuple[Path, Path]:
        candidates: list[Path] = []
        if self.config.cuda_home is not None:
            candidates.append(self.config.cuda_home)
        candidates.extend([root, Path("/usr/local/cuda"), *sorted(Path("/usr/local").glob("cuda-*"))])
        for candidate in candidates:
            if not candidate.is_dir():
                continue
            header = next(iter(candidate.rglob("cuda_runtime_api.h")), None)
            library = next(iter(candidate.rglob("libcudart.so")), None)
            if header is not None and library is not None:
                return header.parent.resolve(), library.parent.resolve()
        raise _error("CUDA Runtime (cuda_runtime_api.h and libcudart.so) not found for TensorRT")

    def _make_tensorrt_pc(
        self,
        root: Path,
        version: str,
        include_dir: Path,
        lib_dir: Path,
        cuda_include: Path,
        cuda_lib: Path,
        pc_dir: Path | None = None,
    ) -> Path:
        target = pc_dir or root / "lib/pkgconfig"
        self._write_pc(
            target / "nvinfer.pc",
            (
                f"prefix={root}",
                "exec_prefix=${prefix}",
                f"libdir={lib_dir}",
                f"includedir={include_dir}",
                f"cudaincludedir={cuda_include}",
                f"cudalibdir={cuda_lib}",
                "",
                "Name: nvinfer",
                "Description: NVIDIA TensorRT and CUDA Runtime",
                f"Version: {version}",
                "Libs: -L${libdir} -l:libnvinfer.so.10 -L${cudalibdir} -lcudart",
                "Cflags: -I${includedir} -I${cudaincludedir}",
            ),
        )
        return target.resolve()

    def _validate_tensorrt_layout(self, root: Path) -> DependencyResult:
        self._require_command("readelf")
        self._require_command("ldd")
        header = self._find_unique(root, "NvInfer.h", description="TensorRT NvInfer.h")
        version_header = self._find_unique(root, "NvInferVersion.h", description="TensorRT NvInferVersion.h")
        version = self._tensorrt_version(version_header)
        if not version.startswith("10."):
            raise _error(f"NVIDIA runtime requires TensorRT ABI major 10, got {version}")
        nvinfer = self._tensorrt_find_library(root, "libnvinfer.so.10")
        nvonnxparser = self._tensorrt_find_library(root, "libnvonnxparser.so.10")
        cudnn = self._tensorrt_find_library(root, "libcudnn.so.9")
        cuda_include, cuda_lib = self._locate_cuda(root)
        cuda_version = self._cuda_version(cuda_include / "cuda_runtime_api.h")
        if not cuda_version.startswith(f"{TENSORRT_CUDA_MAJOR}."):
            raise _error(f"NVIDIA runtime requires CUDA ABI major {TENSORRT_CUDA_MAJOR}, got {cuda_version}")
        cudart = self._tensorrt_find_library(cuda_lib, f"libcudart.so.{TENSORRT_CUDA_MAJOR}")
        include_dir = header.parent.resolve()
        lib_dir = nvinfer.parent.resolve()
        pc_dir = self._make_tensorrt_pc(root, version, include_dir, lib_dir, cuda_include, cuda_lib)
        self._prepend_pkg_config(str(pc_dir))
        actual_pc_dir = Path(self._pkg_dir("nvinfer"))
        if actual_pc_dir != pc_dir:
            raise _error(f"TensorRT pkg-config source mismatch: {actual_pc_dir}")
        self._pkg_at_least("nvinfer", version)
        runtime_dirs = self._runtime_dirs(lib_dir, nvonnxparser.parent, cudnn.parent, cuda_lib)
        for library in (nvinfer, nvonnxparser, cudnn, cudart):
            environment = dict(self.environment)
            environment["LD_LIBRARY_PATH"] = join_unique(runtime_dirs, environment.get("LD_LIBRARY_PATH", ""))
            result = self._command(["ldd", "-r", library], env=environment)
            output = (result.stdout or "") + (result.stderr or "")
            if result.returncode != 0 or "not found" in output or "undefined symbol:" in output:
                raise _error(f"NVIDIA runtime closure is incomplete for {library.name}: {output.strip()}")
        self._compile_probe(
            """#include <NvInfer.h>
#include <NvInferVersion.h>
#include <cuda_runtime_api.h>
int main() {
    int cuda_version = 0;
    if (cudaRuntimeGetVersion(&cuda_version) != cudaSuccess)
        return 1;
    return getInferLibVersion() > 0 && cuda_version >= 13000 && cuda_version < 14000 ? 0 : 1;
}
""",
            "nvinfer",
            runtime_dirs,
        )
        return DependencyResult(
            source=self.config.tensorrt_source,
            version=version,
            pc_dir=str(actual_pc_dir),
            runtime_dirs=runtime_dirs,
        )

    def _install_managed_tensorrt(self, destination: Path) -> None:
        self._require_command("dpkg-deb")
        cache_dir = self.root / ".deps" / "downloads" / "tensorrt" / "10.16.1-cuda13.2-cudnn9"
        for package, expected_hash in self._tensorrt_manifest():
            cache_file = cache_dir / package
            url = f"{TENSORRT_REPOSITORY}/{package}"
            self._download(url, cache_file)
            digest = hashlib.sha256(cache_file.read_bytes()).hexdigest()
            if digest != expected_hash:
                cache_file.unlink(missing_ok=True)
                raise _error(f"NVIDIA package checksum mismatch: {package}")
            check = self._command(["dpkg-deb", "--info", cache_file])
            if check.returncode != 0:
                raise _error(f"NVIDIA package is not a readable Debian archive: {package}")
            result = self._command(["dpkg-deb", "--extract", cache_file, destination])
            if result.returncode != 0:
                raise _error(f"failed to extract NVIDIA package: {package}")

    def _prepare_managed_tensorrt(self) -> DependencyResult:
        root = self.config.tensorrt_root
        archive = self.config.tensorrt_archive
        url = self.config.tensorrt_url
        if archive is None and url is None and root.is_dir():
            try:
                result = self._validate_tensorrt_layout(root)
                if result.version != TENSORRT_MANAGED_VERSION:
                    raise _error(f"managed TensorRT version mismatch: expected {TENSORRT_MANAGED_VERSION}, got {result.version}")
                return result
            except DependencyError:
                pass
        self._require_replaceable(root, self._validate_tensorrt_layout, description="TensorRT root")
        candidate = self._candidate(root.parent, "tensorrt")
        try:
            if archive is None and url is None:
                self._install_managed_tensorrt(candidate)
            else:
                source_archive = archive
                if url is not None:
                    identity = hashlib.sha256(url.encode()).hexdigest()
                    filename = url.rsplit("/", 1)[-1].split("?", 1)[0]
                    source_archive = self.root / ".deps" / "downloads" / "tensorrt" / f"{identity}-{filename}"
                    self._download(url, source_archive)
                assert source_archive is not None
                if not source_archive.is_file():
                    raise _error(f"TensorRT archive does not exist: {source_archive}")
                self._extract_archive(source_archive, candidate)
            result = self._validate_tensorrt_layout(candidate)
            if result.version != TENSORRT_MANAGED_VERSION:
                raise _error(f"managed TensorRT version mismatch: expected {TENSORRT_MANAGED_VERSION}, got {result.version}")
            self._atomic_replace(candidate, root)
            return self._validate_tensorrt_layout(root)
        except BaseException:
            self._remove_path(candidate)
            raise

    def _prepare_tensorrt(self) -> DependencyResult:
        if self.config.tensorrt_mode == "disabled":
            return DependencyResult(source="disabled", version="disabled")
        if self.config.tensorrt_mode == "auto" and self.config.tensorrt_archive is None and self.config.tensorrt_url is None:
            if self.config.tensorrt_source == "managed":
                if not self.config.tensorrt_root.exists() or self._is_empty_directory(self.config.tensorrt_root):
                    return DependencyResult(source="managed", version="unavailable")
                self._require_replaceable(
                    self.config.tensorrt_root,
                    self._validate_tensorrt_layout,
                    description="TensorRT root",
                )
                try:
                    result = self._validate_tensorrt_layout(self.config.tensorrt_root)
                except DependencyError:
                    return DependencyResult(source="managed", version="unavailable")
                if not result.version.startswith("10."):
                    return DependencyResult(source="managed", version="unavailable")
            elif self._pkg("--exists", "nvinfer").returncode != 0:
                return DependencyResult(source="system", version="unavailable")
        try:
            if self.config.tensorrt_source == "managed":
                result = self._prepare_managed_tensorrt()
            else:
                result = self._system_result("nvinfer", "1.0")
        except DependencyError:
            if self.config.tensorrt_mode == "auto" and self.config.tensorrt_archive is None and self.config.tensorrt_url is None:
                return DependencyResult(source=self.config.tensorrt_source, version="unavailable")
            raise _error(
                f"TensorRT is unavailable for mode={self.config.tensorrt_mode} source={self.config.tensorrt_source}"
            )
        if self.config.tensorrt_mode == "auto" and not result.version.startswith("10."):
            return DependencyResult(source=self.config.tensorrt_source, version="unavailable")
        if self.config.tensorrt_mode == "enabled" and not result.version.startswith("10."):
            raise _error(f"NVIDIA runtime requires TensorRT ABI major 10, got {result.version}")
        return result

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
        onnx_where = f" root={self.context.display_path(self.config.onnx_root)}" if self.ort.source == "managed" else f" pcdir={self.ort.pc_dir}"
        print(
            f"ONNX Runtime  profile={self.config.profile} source={self.ort.source}"
            f" version={onnx_version} providers={self.ort.providers}{onnx_where}"
        )
        if self.opencv.version == "disabled":
            print("OpenCV        mode=disabled")
        else:
            where = f" root={self.context.display_path(self.config.opencv_root)}" if self.opencv.source == "managed" else f" pcdir={self.opencv.pc_dir}"
            print(
                f"OpenCV        source={self.opencv.source} mode={self.config.opencv_mode}"
                f" version={self.opencv.version}{where}"
            )
        if self.tensorrt.version in {"disabled", "unavailable"}:
            print(f"TensorRT      source={self.tensorrt.source} mode=disabled status=stub")
        else:
            where = f" root={self.context.display_path(self.config.tensorrt_root)}" if self.tensorrt.source == "managed" else f" pcdir={self.tensorrt.pc_dir}"
            print(
                f"TensorRT      source={self.tensorrt.source} mode=enabled"
                f" version={self.tensorrt.version}{where}"
            )


def load_dependency_manager(context: ProjectContext, requested_profile: str) -> DependencyManager:
    config = DependencyConfig.from_context(context, requested_profile)
    return DependencyManager(context, config)
