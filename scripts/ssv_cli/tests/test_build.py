"""Native build orchestration tests with fake dependency and Meson boundaries."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts.ssv_cli.context import ProjectContext
from scripts.ssv_cli.dependencies import DependencySnapshot, write_snapshot
from scripts.ssv_cli.output import CliError
from scripts.ssv_cli.services.native_build import NativeBuildService


def snapshot(signature: str) -> DependencySnapshot:
    return DependencySnapshot(
        signature=signature,
        profile="cpu",
        pkg_config_path="",
        runtime_path="",
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


class FakeDependencyManager:
    def __init__(self, context: ProjectContext, dependency_snapshot: DependencySnapshot) -> None:
        self.context = context
        self.dependency_snapshot = dependency_snapshot
        self.build_environment = context.child_environment()

    def check_base_dependencies(self) -> None:
        return None

    def prepare(self, _profile: str, *, pending_path: Path) -> DependencySnapshot:
        write_snapshot(pending_path, self.dependency_snapshot)
        return self.dependency_snapshot


class NativeBuildServiceTest(unittest.TestCase):
    def make_context(self, root: Path) -> ProjectContext:
        return ProjectContext(
            root=root,
            environment={},
            config_path=None,
            build_dir=root / "build",
            compose_file=root / "compose.yaml",
        )

    def test_signature_change_uses_clearcache_and_publishes_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            root = Path(temporary_name)
            context = self.make_context(root)
            context.build_dir.mkdir()
            (context.build_dir / "build.ninja").write_text("old", encoding="utf-8")
            (context.build_dir / "ssv-deps.env").write_text(
                "SSV_DEPS_SIGNATURE='old'\n", encoding="utf-8"
            )
            calls: list[list[str]] = []

            def fake_run(_context, argv, **kwargs):
                command = [str(item) for item in argv]
                calls.append(command)
                if command[:3] == ["meson", "setup", "--help"]:
                    return type("Result", (), {"returncode": 0, "stdout": "--clearcache", "stderr": ""})()
                if command[0:2] == ["meson", "setup"]:
                    context.build_dir.mkdir(parents=True, exist_ok=True)
                    (context.build_dir / "build.ninja").write_text("new", encoding="utf-8")
                if command[:2] == ["meson", "compile"]:
                    for directory, name in (
                        ("gst/ssv-template", "libgstssvtemplate.so"),
                        ("gst/ssv-infer", "libgstssvinfer.so"),
                        ("gst/ssv-track", "libgstssvtrack.so"),
                        ("gst/ssv-pub", "libgstssvpub.so"),
                        ("gst/ssv-overlay", "libgstssvoverlay.so"),
                    ):
                        target = context.build_dir / directory / name
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(b"plugin")
                return type("Result", (), {"returncode": 0, "stdout": "", "stderr": ""})()

            manager = FakeDependencyManager(context, snapshot("new"))
            service = NativeBuildService(context)
            with (
                patch("scripts.ssv_cli.services.native_build.load_dependency_manager", return_value=manager),
                patch("scripts.ssv_cli.services.native_build.require_command", return_value="meson"),
                patch("scripts.ssv_cli.services.native_build.run_command", side_effect=fake_run),
            ):
                self.assertEqual(service.build("cpu"), 0)

            self.assertTrue((context.build_dir / "ssv-deps.env").is_file())
            self.assertFalse((context.build_dir / "ssv-deps.env.pending").exists())
            self.assertTrue(any("--clearcache" in call for call in calls))

    def test_missing_plugin_does_not_publish_pending_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            root = Path(temporary_name)
            context = self.make_context(root)
            calls: list[list[str]] = []

            def fake_run(_context, argv, **kwargs):
                command = [str(item) for item in argv]
                calls.append(command)
                if command[:2] == ["meson", "compile"]:
                    return type("Result", (), {"returncode": 0, "stdout": "", "stderr": ""})()
                return type("Result", (), {"returncode": 0, "stdout": "", "stderr": ""})()

            manager = FakeDependencyManager(context, snapshot("same"))
            service = NativeBuildService(context)
            with (
                patch("scripts.ssv_cli.services.native_build.load_dependency_manager", return_value=manager),
                patch("scripts.ssv_cli.services.native_build.require_command", return_value="meson"),
                patch("scripts.ssv_cli.services.native_build.run_command", side_effect=fake_run),
                self.assertRaisesRegex(Exception, "部分插件未生成"),
            ):
                service.build("cpu")
            self.assertFalse((context.build_dir / "ssv-deps.env.pending").exists())

    def test_invalid_project_root_build_directory_is_not_deleted(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            root = Path(temporary_name)
            keep = root / "keep.txt"
            keep.write_text("preserve", encoding="utf-8")
            context = ProjectContext(
                root=root,
                environment={},
                config_path=None,
                build_dir=root,
                compose_file=root / "compose.yaml",
            )

            with self.assertRaisesRegex(CliError, "拒绝删除项目根目录"):
                NativeBuildService(context).build("cpu")

            self.assertEqual(keep.read_text(encoding="utf-8"), "preserve")
