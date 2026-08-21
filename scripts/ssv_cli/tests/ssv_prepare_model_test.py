#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path

import onnx
from onnx import TensorProto, helper

ROOT = Path(__file__).resolve().parents[3]


def make_identity_model(path: Path, *, height: int = 2, width: int = 3) -> None:
    make_model(path, shape=[1, 3, height, width])


def make_model(
    path: Path,
    *,
    shape: list[int | str],
    element_type: int = TensorProto.FLOAT,
    extra_input: bool = False,
) -> None:
    inputs = [helper.make_tensor_value_info("images", element_type, shape)]
    if extra_input:
        inputs.append(
            helper.make_tensor_value_info("threshold", TensorProto.FLOAT, [1])
        )
    graph = helper.make_graph(
        [helper.make_node("Identity", ["images"], ["output"])],
        "identity-detector",
        inputs,
        [helper.make_tensor_value_info("output", element_type, shape)],
    )
    model = helper.make_model(
        graph,
        producer_name="ssv-test",
        opset_imports=[helper.make_opsetid("", 18)],
    )
    model.ir_version = 10
    onnx.save_model(model, path)


def make_unsupported_runtime_model(path: Path) -> None:
    shape = [1, 3, 2, 3]
    graph = helper.make_graph(
        [
            helper.make_node(
                "UnavailableAtRuntime",
                ["images"],
                ["output"],
                domain="ssv.test",
            )
        ],
        "unsupported-runtime-model",
        [helper.make_tensor_value_info("images", TensorProto.FLOAT, shape)],
        [helper.make_tensor_value_info("output", TensorProto.FLOAT, shape)],
    )
    model = helper.make_model(
        graph,
        producer_name="ssv-test",
        opset_imports=[
            helper.make_opsetid("", 18),
            helper.make_opsetid("ssv.test", 1),
        ],
    )
    model.ir_version = 10
    onnx.save_model(model, path)


def rewrite_as_legacy_wrapper(path: Path) -> None:
    model = onnx.load_model(path)
    cast, gather, divide, transpose, *original_nodes = model.graph.node
    wrapper_input = model.graph.input[0].name
    gather.input[0] = wrapper_input
    cast.input[0] = gather.output[0]
    divide.input[0] = cast.output[0]
    del model.graph.node[:]
    model.graph.node.extend([gather, cast, divide, transpose, *original_nodes])
    del model.graph.value_info[:]
    onnx.save_model(model, path)


class PrepareModelTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.work = Path(self.temporary_directory.name)

    def run_prepare(
        self,
        source: Path,
        output: Path,
        *extra: str,
        env: dict[str, str] | None = None,
        output_format: str = "yolov8",
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            self.prepare_command(source, output, *extra, output_format=output_format),
            cwd=ROOT,
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )

    def prepare_command(
        self,
        source: Path,
        output: Path,
        *extra: str,
        output_format: str = "yolov8",
    ) -> list[str]:
        return [
            str(ROOT / "ssv"),
            "model",
            "prepare",
            "--input",
            str(source),
            "--output",
            str(output),
            "--family",
            "yolo",
            "--output-format",
            output_format,
            *extra,
        ]

    def restricted_command_path(self, name: str) -> Path:
        path = self.work / name
        path.mkdir()
        for command in ("bash", "dirname", "mktemp", "rm", "python3"):
            executable = shutil.which(command)
            self.assertIsNotNone(executable)
            (path / command).symlink_to(executable)
        return path

    def test_prepares_static_float_nchw_model_as_rgba_uint8(self) -> None:
        source = self.work / "source.onnx"
        output = self.work / "wrapper.onnx"
        second_output = self.work / "wrapper-copy.onnx"
        make_identity_model(source)
        source_before = source.read_bytes()

        poison_import = self.work / "poison-import"
        poison_import.mkdir()
        (poison_import / "cv2.py").write_text(
            "raise RuntimeError('OpenCV must not be imported')\n", encoding="utf-8"
        )
        environment = os.environ.copy()
        environment["PYTHONPATH"] = str(poison_import)

        result = self.run_prepare(source, output, env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("event=model_prepared", result.stdout)
        self.assertEqual(source.read_bytes(), source_before)

        wrapper = onnx.load_model(output)
        self.assertEqual(len(wrapper.graph.input), 1)
        wrapper_input = wrapper.graph.input[0]
        self.assertEqual(wrapper_input.type.tensor_type.elem_type, TensorProto.UINT8)
        dimensions = [
            dimension.dim_value
            for dimension in wrapper_input.type.tensor_type.shape.dim
        ]
        self.assertEqual(dimensions, [1, 2, 3, 4])

        operation_types = [node.op_type for node in wrapper.graph.node]
        self.assertEqual(operation_types[:4], ["Cast", "Gather", "Div", "Transpose"])
        cast, gather, divide, transpose = wrapper.graph.node[:4]
        self.assertEqual(cast.input[0], wrapper_input.name)
        self.assertEqual(gather.input[0], cast.output[0])
        self.assertEqual(divide.input[0], gather.output[0])
        self.assertEqual(transpose.input[0], divide.output[0])
        self.assertNotIn("Resize", operation_types)
        self.assertNotIn("Pad", operation_types)

        metadata = {item.key: item.value for item in wrapper.metadata_props}
        self.assertEqual(
            metadata,
            {
                "ssv.wrapper.channel_rule": "drop_alpha_keep_rgb",
                "ssv.wrapper.contract": "rgba_u8_nhwc_v1",
                "ssv.wrapper.dtype": "uint8",
                "ssv.wrapper.height": "2",
                "ssv.wrapper.layout": "NHWC",
                "ssv.wrapper.model_family": "yolo",
                "ssv.wrapper.normalization": "divide_by_255",
                "ssv.wrapper.output_format": "yolov8",
                "ssv.wrapper.source_sha256": hashlib.sha256(source_before).hexdigest(),
                "ssv.wrapper.tool": "ssv.prepare_wrapper",
                "ssv.wrapper.tool_version": "1.1.0",
                "ssv.wrapper.width": "3",
            },
        )

        repeated = self.run_prepare(source, second_output)
        self.assertEqual(repeated.returncode, 0, repeated.stderr)
        self.assertEqual(output.read_bytes(), second_output.read_bytes())

    def test_accepts_yolo_nx6_output_contract_and_safe_replacement(self) -> None:
        source = self.work / "source.onnx"
        output = self.work / "wrapper.onnx"
        make_identity_model(source)

        yolov8 = self.run_prepare(source, output)
        self.assertEqual(yolov8.returncode, 0, yolov8.stderr)

        yolo_nx6 = self.run_prepare(
            source,
            output,
            "--force",
            output_format="yolo_nx6",
        )
        self.assertEqual(yolo_nx6.returncode, 0, yolo_nx6.stderr)
        self.assertIn("status=replaced", yolo_nx6.stdout)

        metadata = {
            item.key: item.value for item in onnx.load_model(output).metadata_props
        }
        self.assertEqual(metadata["ssv.wrapper.output_format"], "yolo_nx6")

        unchanged = self.run_prepare(
            source,
            output,
            output_format="yolo_nx6",
        )
        self.assertEqual(unchanged.returncode, 0, unchanged.stderr)
        self.assertIn("status=unchanged", unchanged.stdout)

    def test_rejects_duplicate_cli_options_before_creating_output(self) -> None:
        first_source = self.work / "first.onnx"
        second_source = self.work / "second.onnx"
        output = self.work / "wrapper.onnx"
        make_identity_model(first_source)
        make_identity_model(second_source)

        result = subprocess.run(
            [
                str(ROOT / "ssv"),
                "model",
                "prepare",
                "--input",
                str(first_source),
                "--input",
                str(second_source),
                "--output",
                str(output),
                "--family",
                "yolo",
                "--output-format",
                "yolov8",
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )

        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(output.exists())
        self.assertEqual(result.stderr.count("event=fatal_error"), 1)
        self.assertIn("exit_code=2", result.stderr)
        self.assertIn("stage=cli", result.stderr)
        self.assertIn("source_id=model-prepare", result.stderr)
        self.assertIn("may be specified only once", result.stderr)

    def test_existing_output_is_idempotent_and_force_is_provenance_safe(self) -> None:
        first_source = self.work / "first.onnx"
        second_source = self.work / "second.onnx"
        output = self.work / "wrapper.onnx"
        make_identity_model(first_source, width=3)
        make_identity_model(second_source, width=4)

        created = self.run_prepare(first_source, output)
        self.assertEqual(created.returncode, 0, created.stderr)
        original_output = output.read_bytes()
        original_mtime = output.stat().st_mtime_ns

        unchanged = self.run_prepare(first_source, output)
        self.assertEqual(unchanged.returncode, 0, unchanged.stderr)
        self.assertIn("status=unchanged", unchanged.stdout)
        self.assertEqual(output.read_bytes(), original_output)
        self.assertEqual(output.stat().st_mtime_ns, original_mtime)

        rewrite_as_legacy_wrapper(output)
        legacy_output = output.read_bytes()
        self.assertNotEqual(legacy_output, original_output)

        refused = self.run_prepare(second_source, output)
        self.assertEqual(refused.returncode, 4, refused.stderr)
        self.assertIn("event=fatal_error", refused.stderr)
        self.assertIn("exit_code=4", refused.stderr)
        self.assertEqual(output.read_bytes(), legacy_output)

        replaced = self.run_prepare(second_source, output, "--force")
        self.assertEqual(replaced.returncode, 0, replaced.stderr)
        self.assertIn("status=replaced", replaced.stdout)
        self.assertNotEqual(output.read_bytes(), original_output)
        dimensions = [
            dimension.dim_value
            for dimension in onnx.load_model(output)
            .graph.input[0]
            .type.tensor_type.shape.dim
        ]
        self.assertEqual(dimensions, [1, 2, 4, 4])

        untrusted_output = self.work / "untrusted.onnx"
        untrusted_output.write_bytes(b"user-owned content\n")
        untrusted_before = untrusted_output.read_bytes()
        rejected_force = self.run_prepare(first_source, untrusted_output, "--force")
        self.assertEqual(rejected_force.returncode, 4, rejected_force.stderr)
        self.assertIn("refusing --force", rejected_force.stderr)
        self.assertEqual(untrusted_output.read_bytes(), untrusted_before)

        forged_output = self.work / "forged-wrapper.onnx"
        forged = onnx.load_model(output)
        for item in forged.metadata_props:
            if item.key == "ssv.wrapper.tool":
                item.value = "another.tool"
        onnx.save_model(forged, forged_output)
        forged_before = forged_output.read_bytes()
        rejected_forgery = self.run_prepare(first_source, forged_output, "--force")
        self.assertEqual(rejected_forgery.returncode, 4, rejected_forgery.stderr)
        self.assertIn("refusing --force", rejected_forgery.stderr)
        self.assertIn("ssv.wrapper.tool", rejected_forgery.stderr)
        self.assertEqual(forged_output.read_bytes(), forged_before)

        self.assertEqual(list(self.work.glob(".*.tmp")), [])

    def test_rejects_models_outside_the_source_input_contract(self) -> None:
        cases: list[tuple[str, str, Callable[[Path], object]]] = [
            (
                "multiple-inputs",
                "exactly one graph input",
                lambda path: make_model(path, shape=[1, 3, 2, 3], extra_input=True),
            ),
            (
                "float64",
                "must use float32",
                lambda path: make_model(
                    path, shape=[1, 3, 2, 3], element_type=TensorProto.DOUBLE
                ),
            ),
            (
                "dynamic-height",
                "must be static [1,3,H,W]",
                lambda path: make_model(path, shape=[1, 3, "height", 3]),
            ),
            (
                "batch-two",
                "must be static [1,3,H,W]",
                lambda path: make_model(path, shape=[2, 3, 2, 3]),
            ),
            (
                "four-channels",
                "must be static [1,3,H,W]",
                lambda path: make_model(path, shape=[1, 4, 2, 3]),
            ),
            (
                "invalid-onnx",
                "invalid input ONNX model",
                lambda path: path.write_bytes(b"not an ONNX model\n"),
            ),
        ]

        for name, expected_error, create_source in cases:
            with self.subTest(name=name):
                source = self.work / f"{name}.onnx"
                output = self.work / f"{name}-wrapper.onnx"
                create_source(source)

                result = self.run_prepare(source, output)

                self.assertEqual(result.returncode, 4, result.stderr)
                self.assertEqual(result.stderr.count("event=fatal_error"), 1)
                self.assertIn("exit_code=4", result.stderr)
                self.assertIn("stage=model_prepare", result.stderr)
                self.assertIn("source_id=model-prepare", result.stderr)
                self.assertIn(expected_error, result.stderr)
                self.assertFalse(output.exists())
                self.assertEqual(list(self.work.glob(f".{output.name}.*.tmp")), [])

    def test_cli_errors_are_side_effect_free_and_use_fixed_exit_codes(self) -> None:
        source = self.work / "source.onnx"
        output = self.work / "wrapper.onnx"
        make_identity_model(source)
        valid = [
            "--input",
            str(source),
            "--output",
            str(output),
            "--family",
            "yolo",
            "--output-format",
            "yolov8",
        ]
        cases = [
            ([], "the following arguments are required"),
            (["--input"], "expected one argument"),
            ([*valid, "--family", "yolo"], "may be specified only once"),
            ([*valid, "--force", "--force"], "may be specified only once"),
            (
                [*valid[:5], "other", *valid[6:]],
                "invalid choice: 'other'",
            ),
            ([*valid, "--resize", "640"], "unrecognized arguments"),
        ]

        for arguments, expected_error in cases:
            with self.subTest(arguments=arguments):
                output.unlink(missing_ok=True)
                result = subprocess.run(
                    [str(ROOT / "ssv"), "model", "prepare", *arguments],
                    cwd=ROOT,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stderr.count("event=fatal_error"), 1)
                self.assertIn("exit_code=2", result.stderr)
                self.assertIn("stage=cli", result.stderr)
                self.assertIn("source_id=model-prepare", result.stderr)
                self.assertIn(expected_error, result.stderr)
                self.assertFalse(output.exists())

        help_result = subprocess.run(
            [str(ROOT / "ssv"), "model", "prepare", "--help"],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertIn("usage: ./ssv model prepare", help_result.stdout)
        self.assertNotIn("event=fatal_error", help_result.stderr)

        restricted_path = self.restricted_command_path("no-uv-bin")
        no_uv = subprocess.run(
            [sys.executable, str(ROOT / "ssv"), "model", "prepare", *valid],
            cwd=ROOT,
            env=os.environ | {"PATH": str(restricted_path)},
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(no_uv.returncode, 0, no_uv.stderr)
        self.assertIn("event=model_prepared", no_uv.stdout)
        self.assertTrue(output.exists())

    def test_cpu_smoke_failure_does_not_publish_any_output(self) -> None:
        source = self.work / "unsupported.onnx"
        output = self.work / "new-directory" / "wrapper.onnx"
        make_unsupported_runtime_model(source)

        result = self.run_prepare(source, output)

        self.assertEqual(result.returncode, 4, result.stderr)
        self.assertIn("event=fatal_error", result.stderr)
        self.assertIn("wrapper validation failed", result.stderr)
        self.assertIn("not a registered function/op", result.stderr)
        self.assertFalse(output.parent.exists())
        self.assertEqual(list(self.work.rglob(".*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
