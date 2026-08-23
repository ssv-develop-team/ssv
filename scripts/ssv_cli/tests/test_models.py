"""Model service tests around the direct Python integration boundary."""

from __future__ import annotations

import tempfile
import unittest
import types
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

    def test_manifest_passes_raw_model_path_to_writer(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_name:
            root = Path(temporary_name)
            captured: dict[str, Path] = {}
            fake_module = types.SimpleNamespace(
                parse_args=lambda _arguments: Namespace(
                    model=Path("source.onnx"),
                    engine=Path("model.engine"),
                    output=Path("model.engine.json"),
                    force=False,
                ),
                build_manifest=lambda options: captured.update(
                    model=options.model,
                    engine=options.engine,
                    output=options.output,
                ) or {"engine": {"sha256": "a" * 64}},
                write_manifest=lambda *_args: None,
                format_log_value=lambda value: value,
            )
            with patch(
                "scripts.ssv_cli.services.models._import_optional", return_value=fake_module
            ), patch("builtins.print") as output:
                result = ModelService(self.make_context(root)).write_manifest([])
            self.assertEqual(result, 0)
            self.assertEqual(captured["model"], root / "source.onnx")
            self.assertEqual(captured["engine"], root / "model.engine")
            printed = " ".join(str(item) for call in output.call_args_list for item in call.args)
            self.assertIn("engine_sha256=" + "a" * 64, printed)

    def test_verify_help_does_not_require_ultralytics(self) -> None:
        with patch(
            "scripts.ssv_cli.services.models._require_optional",
            side_effect=AssertionError("help must not preflight optional dependencies"),
        ):
            self.assertEqual(ModelService(self.make_context(Path("/tmp"))).verify(["--help"]), 0)
