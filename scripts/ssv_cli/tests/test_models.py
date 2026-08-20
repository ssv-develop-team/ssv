"""Model service tests around the direct Python integration boundary."""

from __future__ import annotations

import tempfile
import types
import unittest
from argparse import Namespace
from pathlib import Path
from unittest.mock import patch

from scripts.ssv_cli.context import ProjectContext
from scripts.ssv_cli.output import CliError
from scripts.ssv_cli.services.models import ModelService


class ModelServiceTest(unittest.TestCase):
    def make_context(self, root: Path) -> ProjectContext:
        return ProjectContext(
            root=root,
            environment={},
            config_path=None,
            build_dir=root / "build",
            compose_file=root / "compose.yaml",
        )

    def test_export_is_idempotent_without_importing_exporter(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            root = Path(temporary_name)
            target = root / "models/yolov8n.onnx"
            target.parent.mkdir()
            target.write_bytes(b"existing")
            with patch("scripts.ssv_cli.services.models._import_optional") as importer:
                self.assertEqual(ModelService(self.make_context(root)).export_default(), 0)
            importer.assert_not_called()

    def test_prepare_calls_wrapper_module_directly_and_resolves_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            root = Path(temporary_name)
            fake_module = types.SimpleNamespace(
                WRAPPER_CONTRACT="rgba_u8_nhwc_v1",
                PrepareModelError=RuntimeError,
                parse_args=lambda _arguments: Namespace(
                    input=Path("source.onnx"), output=Path("wrapper.onnx"), force=False
                ),
                prepare_wrapper=lambda options: ("created", "a" * 64),
                format_log_value=lambda value: value,
                format_fatal_error=lambda error: str(error),
            )
            with patch(
                "scripts.ssv_cli.services.models._import_optional", return_value=fake_module
            ), patch("builtins.print") as output:
                result = ModelService(self.make_context(root)).prepare(["--direct"])
            self.assertEqual(result, 0)
            printed = " ".join(str(item) for call in output.call_args_list for item in call.args)
            self.assertIn("source_sha256=" + "a" * 64, printed)

    def test_prepare_reports_missing_optional_dependency_without_uv_or_pip(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            context = self.make_context(Path(temporary_name))
            missing = ModuleNotFoundError("No module named 'onnx'")
            missing.name = "onnx"
            with patch(
                "scripts.ssv_cli.services.models.importlib.import_module",
                side_effect=missing,
            ), self.assertRaisesRegex(CliError, r"\[model\]"):
                ModelService(context).prepare([])

    def test_prepare_reports_any_missing_dependency_from_model_extra(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            context = self.make_context(Path(temporary_name))
            missing = ModuleNotFoundError("No module named 'onnxruntime'")
            missing.name = "onnxruntime"
            with patch(
                "scripts.ssv_cli.services.models.importlib.import_module",
                side_effect=missing,
            ), self.assertRaisesRegex(CliError, "onnxruntime"):
                ModelService(context).prepare([])

    def test_prepare_reports_internal_import_failure_without_install_hint(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            context = self.make_context(Path(temporary_name))
            missing = ModuleNotFoundError("No module named 'broken_internal_dependency'")
            missing.name = "broken_internal_dependency"
            with patch(
                "scripts.ssv_cli.services.models.importlib.import_module",
                side_effect=missing,
            ), self.assertRaisesRegex(CliError, "内部依赖 broken_internal_dependency"):
                ModelService(context).prepare([])

    def test_verify_help_does_not_require_ultralytics(self) -> None:
        with patch(
            "scripts.ssv_cli.services.models._require_optional",
            side_effect=AssertionError("help must not preflight optional dependencies"),
        ):
            self.assertEqual(ModelService(self.make_context(Path("/tmp"))).verify(["--help"]), 0)
