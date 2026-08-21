"""Dependency policy and snapshot tests that do not require native SDKs."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock

from scripts.ssv_cli.context import ProjectContext
from scripts.ssv_cli.dependencies import (
    DependencyConfig,
    DependencyManager,
    DependencySnapshot,
    detect_gpu_vendors,
    expected_providers,
    resolve_profile,
    validate_provider_set,
    write_snapshot,
)
from scripts.ssv_cli.dependencies.onnxruntime import _ORT_PROBE_SOURCE, OnnxRuntimeProvider
from scripts.ssv_cli.dependencies.tensorrt import TensorRtProvider
from scripts.ssv_cli.dependencies.tooling import DependencyTooling
from scripts.ssv_cli.output import CliError
from scripts.ssv_cli.services.runtime_env import load_dependency_snapshot


class _DownloadHarness(DependencyTooling):
    def __init__(self, root: Path) -> None:
        self.root = root
        self.environment: dict[str, str] = {}


class DependencyPolicyTest(unittest.TestCase):
    def make_context(self, root: Path, **environment: str) -> ProjectContext:
        return ProjectContext(
            root=root,
            environment=environment,
            config_path=None,
            build_dir=root / "build",
            compose_file=root / "compose.yaml",
        )

    def test_detect_gpu_vendors_normalizes_and_deduplicates(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            sysfs = Path(temporary_name)
            for card, vendor in (("card0", "0x8086\n"), ("card1", "10de"), ("card2", "0x10DE"), ("card3", "0x1022")):
                path = sysfs / "class/drm" / card / "device/vendor"
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(vendor, encoding="ascii")
            self.assertEqual(detect_gpu_vendors(sysfs), ("8086", "10de"))

    def test_auto_profile_uses_deterministic_vendor_priority(self) -> None:
        self.assertEqual(resolve_profile("auto", ("1002", "8086")), "intel")
        self.assertEqual(resolve_profile("auto", ("10de", "8086")), "nvidia")
        self.assertEqual(resolve_profile("auto", ()), "cpu")
        self.assertEqual(resolve_profile("intel"), "intel")

    def test_provider_contract_rejects_wrong_or_duplicate_accelerators(self) -> None:
        self.assertEqual(expected_providers("nvidia")[0], "TensorrtExecutionProvider")
        self.assertEqual(
            validate_provider_set("intel", "CPUExecutionProvider,OpenVINOExecutionProvider"),
            ("CPUExecutionProvider", "OpenVINOExecutionProvider"),
        )
        with self.assertRaises(CliError):
            validate_provider_set("intel", "CPUExecutionProvider,CUDAExecutionProvider")
        with self.assertRaises(CliError):
            validate_provider_set("cpu", "CPUExecutionProvider,CPUExecutionProvider")

    def test_config_defaults_follow_resolved_profile(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            root = Path(temporary_name)
            config = DependencyConfig.from_context(self.make_context(root), "cpu")
            self.assertEqual(config.profile, "cpu")
            self.assertEqual(config.onnx_source, "managed")
            self.assertEqual(config.onnx_version, "1.25.1")
            self.assertEqual(config.opencv_mode, "enabled")
            self.assertEqual(config.tensorrt_mode, "auto")

    def test_config_rejects_version_override_and_invalid_combinations(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            root = Path(temporary_name)
            with self.assertRaisesRegex(CliError, "no longer configurable"):
                DependencyConfig.from_context(
                    self.make_context(root, SSV_ONNXRUNTIME_VERSION="1.25.1"),
                    "cpu",
                )
            with self.assertRaisesRegex(CliError, "requires SSV_TENSORRT_MODE=enabled"):
                DependencyConfig.from_context(
                    self.make_context(root, SSV_TENSORRT_MODE="auto"),
                    "nvidia",
                )
            with self.assertRaisesRegex(CliError, "disabled OpenCV"):
                DependencyConfig.from_context(
                    self.make_context(root, SSV_OPENCV_MODE="disabled", SSV_OPENCV_ROOT=".deps/custom"),
                    "cpu",
                )

    def test_onnx_probe_source_uses_cxx_newline_escapes(self) -> None:
        self.assertIn(r'"\nproviders="', _ORT_PROBE_SOURCE)
        self.assertIn(r'std::cout << "\n";', _ORT_PROBE_SOURCE)
        self.assertNotIn('"\nproviders="', _ORT_PROBE_SOURCE)

    def test_build_environment_drops_replaced_pkg_config_directories(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            root = Path(temporary_name)
            active = root / "active" / "pkgconfig"
            active.mkdir(parents=True)
            manager = DependencyManager(
                self.make_context(root),
                DependencyConfig.from_context(self.make_context(root), "cpu"),
            )
            manager.pkg_config_dirs = [str(root / "removed" / "pkgconfig"), str(active)]

            environment = manager.build_environment

        self.assertEqual(environment["PKG_CONFIG_PATH"], str(active))
        self.assertEqual(manager.pkg_config_dirs, [str(active)])

    def test_download_copies_archive_into_cache(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            root = Path(temporary_name)
            source = root / "source.bin"
            destination = root / "cache" / "artifact.bin"
            source.write_bytes(b"artifact")

            result = _DownloadHarness(root)._download(source.as_uri(), destination)

            self.assertEqual(result, destination)
            self.assertEqual(destination.read_bytes(), b"artifact")

    def test_tensorrt_version_accepts_aliased_enterprise_macros(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            header = Path(temporary_name) / "NvInferVersion.h"
            header.write_text(
                "#define TRT_MAJOR_ENTERPRISE 10\n"
                "#define TRT_MINOR_ENTERPRISE 16\n"
                "#define TRT_PATCH_ENTERPRISE 1\n"
                "#define NV_TENSORRT_MAJOR TRT_MAJOR_ENTERPRISE //!< TensorRT major version.\n"
                "#define NV_TENSORRT_MINOR TRT_MINOR_ENTERPRISE //!< TensorRT minor version.\n"
                "#define NV_TENSORRT_PATCH TRT_PATCH_ENTERPRISE //!< TensorRT patch version.\n",
                encoding="utf-8",
            )

            version = TensorRtProvider()._tensorrt_version(header)

            self.assertEqual(version, "10.16.1")

    def test_onnx_provider_libraries_resolve_nvidia_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            lib_dir = Path(temporary_name)
            for name in (
                "libonnxruntime_providers_shared.so",
                "libonnxruntime_providers_tensorrt.so",
                "libonnxruntime_providers_cuda.so",
            ):
                (lib_dir / name).write_bytes(b"placeholder")

            provider = OnnxRuntimeProvider()
            provider._validate_elf = Mock()
            provider._readelf = Mock(return_value="GLOBAL DEFAULT GetProvider\n")

            libraries = provider._ort_provider_libraries("nvidia", lib_dir)

            self.assertEqual(
                libraries.split(":"),
                [str((lib_dir / name).resolve()) for name in (
                    "libonnxruntime_providers_shared.so",
                    "libonnxruntime_providers_tensorrt.so",
                    "libonnxruntime_providers_cuda.so",
                )],
            )


class DependencySnapshotTest(unittest.TestCase):
    def test_snapshot_is_written_in_stable_shell_word_format(self) -> None:
        snapshot = DependencySnapshot(
            signature="abc123",
            profile="cpu",
            pkg_config_path="/tmp/pc dir",
            runtime_path="/tmp/runtime",
            opencv_mode="disabled",
            tensorrt_mode="disabled",
            onnxruntime_version="1.25.1",
            onnxruntime_pcdir="",
            onnxruntime_runtime_dirs="",
            onnxruntime_providers="CPUExecutionProvider",
            onnxruntime_provider_libraries="",
            opencv_pcdir="",
            opencv_runtime_dirs="",
            tensorrt_pcdir="",
            tensorrt_runtime_dirs="",
        )
        with tempfile.TemporaryDirectory() as temporary_name:
            path = Path(temporary_name) / "build/ssv-deps.env"
            write_snapshot(path, snapshot)
            values = load_dependency_snapshot(path)
            self.assertEqual(values["SSV_DEPS_SIGNATURE"], "abc123")
            self.assertEqual(values["SSV_DEPS_PKG_CONFIG_PATH"], "/tmp/pc dir")
            self.assertEqual(len(values), 15)
