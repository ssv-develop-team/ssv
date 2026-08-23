#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

import onnx
from onnx import TensorProto, helper

ROOT = Path(__file__).resolve().parents[3]


def make_model(
    path: Path,
    *,
    input_type: int = TensorProto.FLOAT,
    input_shape: list[int] | None = None,
) -> None:
    shape = input_shape or [1, 3, 2, 3]
    input_value = helper.make_tensor_value_info(
        "images", input_type, shape
    )
    output_value = helper.make_tensor_value_info(
        "output0", TensorProto.FLOAT, [1, 2, 3, 6]
    )
    graph = helper.make_graph(
        [helper.make_node("Constant", [], ["output0"])],
        "manifest-test-wrapper",
        [input_value],
        [output_value],
    )
    model = helper.make_model(
        graph,
        producer_name="ssv-test",
        opset_imports=[helper.make_opsetid("", 18)],
    )
    model.ir_version = 10
    onnx.save_model(model, path)


class TensorRtManifestToolTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.work = Path(self.temporary_directory.name)

    def run_tool(
        self,
        model: Path,
        engine: Path,
        output: Path,
        *extra: str,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                str(ROOT / "ssv"),
                "model",
                "manifest",
                "--model",
                str(model),
                "--engine",
                str(engine),
                "--output",
                str(output),
                "--precision",
                "fp16",
                "--tensorrt-version",
                "11.1.0.106",
                "--cuda-runtime-version",
                "13020",
                "--compute-capability",
                "8.9",
                *extra,
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_writes_manifest_from_raw_model_and_engine_content(self) -> None:
        model = self.work / "model.onnx"
        engine = self.work / "model.engine"
        output = self.work / "model.engine.json"
        make_model(model)
        engine.write_bytes(b"serialized-engine")

        result = self.run_tool(model, engine, output)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("event=tensorrt_manifest_written", result.stdout)
        self.assertEqual(
            json.loads(output.read_text(encoding="utf-8")),
            {
                "schema": "ssv.tensorrt-engine-manifest",
                "schema_version": 2,
                "engine": {
                    "sha256": hashlib.sha256(engine.read_bytes()).hexdigest(),
                    "precision": "fp16",
                    "tensorrt_version": "11.1.0.106",
                    "cuda_runtime_version": 13020,
                    "compute_capability": {"major": 8, "minor": 9},
                },
                "source_model": {
                    "sha256": hashlib.sha256(model.read_bytes()).hexdigest(),
                    "input": {
                        "name": "images",
                        "dtype": "float32",
                        "layout": "NCHW",
                        "shape": [1, 3, 2, 3],
                    },
                },
            },
        )

        repeated = self.run_tool(model, engine, output)
        self.assertEqual(repeated.returncode, 0, repeated.stderr)

    def test_rejects_non_float_nchw_source_before_creating_manifest(self) -> None:
        model = self.work / "invalid.onnx"
        engine = self.work / "model.engine"
        output = self.work / "model.engine.json"
        make_model(model, input_type=TensorProto.UINT8, input_shape=[1, 2, 3, 4])
        engine.write_bytes(b"serialized-engine")

        result = self.run_tool(model, engine, output)

        self.assertEqual(result.returncode, 4, result.stderr)
        self.assertFalse(output.exists())
        self.assertEqual(result.stderr.count("event=fatal_error"), 1)
        self.assertIn("stage=engine_manifest", result.stderr)
        self.assertIn("source model input must use float32", result.stderr)

    def test_rejects_malformed_source_without_a_traceback(self) -> None:
        model = self.work / "malformed.onnx"
        engine = self.work / "model.engine"
        output = self.work / "model.engine.json"
        model.write_bytes(b"not-an-onnx-model")
        engine.write_bytes(b"serialized-engine")

        result = self.run_tool(model, engine, output)

        self.assertEqual(result.returncode, 4, result.stderr)
        self.assertFalse(output.exists())
        self.assertEqual(result.stderr.count("event=fatal_error"), 1)
        self.assertNotIn("Traceback", result.stderr)
        self.assertIn("cannot load source ONNX", result.stderr)

    def test_requires_force_to_replace_a_different_manifest(self) -> None:
        model = self.work / "model.onnx"
        engine = self.work / "model.engine"
        output = self.work / "model.engine.json"
        make_model(model)
        engine.write_bytes(b"first-engine")
        first = self.run_tool(model, engine, output)
        self.assertEqual(first.returncode, 0, first.stderr)
        first_contents = output.read_bytes()

        engine.write_bytes(b"second-engine")
        rejected = self.run_tool(model, engine, output)
        self.assertEqual(rejected.returncode, 4, rejected.stderr)
        self.assertEqual(output.read_bytes(), first_contents)

        replaced = self.run_tool(model, engine, output, "--force")
        self.assertEqual(replaced.returncode, 0, replaced.stderr)
        self.assertNotEqual(output.read_bytes(), first_contents)
        manifest = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(
            manifest["engine"]["sha256"],
            hashlib.sha256(b"second-engine").hexdigest(),
        )


if __name__ == "__main__":
    unittest.main()
