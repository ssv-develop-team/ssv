"""依赖 profile 策略、默认值和配置校验。"""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from ..context import ProjectContext
from ..output import CliError

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
