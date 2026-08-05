#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11,<3.12"
# dependencies = [
#   "onnx==1.18.0",
# ]
# ///

from __future__ import annotations

import hashlib
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

import onnx
from onnx import TensorProto, helper

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "scripts/model/write_tensorrt_manifest.py"


def make_wrapper(path: Path) -> None:
    input_value = helper.make_tensor_value_info(
        "images_rgba", TensorProto.UINT8, [1, 2, 3, 4]
    )
    output_value = helper.make_tensor_value_info(
        "output0", TensorProto.FLOAT, [1, 2, 3, 4]
    )
    graph = helper.make_graph(
        [helper.make_node("Cast", ["images_rgba"], ["output0"], to=TensorProto.FLOAT)],
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
    metadata = {
        "ssv.wrapper.channel_rule": "drop_alpha_keep_rgb",
        "ssv.wrapper.contract": "rgba_u8_nhwc_v1",
        "ssv.wrapper.dtype": "uint8",
        "ssv.wrapper.height": "2",
        "ssv.wrapper.layout": "NHWC",
        "ssv.wrapper.model_family": "yolo",
        "ssv.wrapper.normalization": "divide_by_255",
        "ssv.wrapper.output_format": "yolov8",
        "ssv.wrapper.source_sha256": "a" * 64,
        "ssv.wrapper.tool": "ssv.prepare_wrapper",
        "ssv.wrapper.tool_version": "1.0.0",
        "ssv.wrapper.width": "3",
    }
    for key, value in sorted(metadata.items()):
        property_value = model.metadata_props.add()
        property_value.key = key
        property_value.value = value
    onnx.save_model(model, path)


class TensorRtManifestToolTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.work = Path(self.temporary_directory.name)

    def run_tool(
        self,
        wrapper: Path,
        engine: Path,
        output: Path,
        *extra: str,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "uv",
                "run",
                "--isolated",
                "--script",
                str(TOOL),
                "--wrapper",
                str(wrapper),
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

    def test_writes_manifest_from_wrapper_and_engine_content(self) -> None:
        wrapper = self.work / "model.onnx"
        engine = self.work / "model.engine"
        output = self.work / "model.engine.json"
        make_wrapper(wrapper)
        engine.write_bytes(b"serialized-engine")

        result = self.run_tool(wrapper, engine, output)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("event=tensorrt_manifest_written", result.stdout)
        self.assertEqual(
            json.loads(output.read_text(encoding="utf-8")),
            {
                "schema": "ssv.tensorrt-engine-manifest",
                "schema_version": 1,
                "engine": {
                    "sha256": hashlib.sha256(engine.read_bytes()).hexdigest(),
                    "precision": "fp16",
                    "tensorrt_version": "11.1.0.106",
                    "cuda_runtime_version": 13020,
                    "compute_capability": {"major": 8, "minor": 9},
                },
                "wrapper": {
                    "sha256": hashlib.sha256(wrapper.read_bytes()).hexdigest(),
                    "contract": "rgba_u8_nhwc_v1",
                    "source_sha256": "a" * 64,
                    "tool_version": "1.0.0",
                    "model_family": "yolo",
                    "output_format": "yolov8",
                    "input": {
                        "name": "images_rgba",
                        "dtype": "uint8",
                        "layout": "NHWC",
                        "shape": [1, 2, 3, 4],
                    },
                },
            },
        )

        repeated = self.run_tool(wrapper, engine, output)
        self.assertEqual(repeated.returncode, 0, repeated.stderr)

    def test_rejects_invalid_wrapper_before_creating_manifest(self) -> None:
        wrapper = self.work / "invalid.onnx"
        engine = self.work / "model.engine"
        output = self.work / "model.engine.json"
        make_wrapper(wrapper)
        model = onnx.load_model(wrapper)
        for item in model.metadata_props:
            if item.key == "ssv.wrapper.contract":
                item.value = "legacy_rgba"
        onnx.save_model(model, wrapper)
        engine.write_bytes(b"serialized-engine")

        result = self.run_tool(wrapper, engine, output)

        self.assertEqual(result.returncode, 4, result.stderr)
        self.assertFalse(output.exists())
        self.assertEqual(result.stderr.count("event=fatal_error"), 1)
        self.assertIn("stage=engine_manifest", result.stderr)
        self.assertIn("ssv.wrapper.contract", result.stderr)

    def test_rejects_malformed_wrapper_without_a_traceback(self) -> None:
        wrapper = self.work / "malformed.onnx"
        engine = self.work / "model.engine"
        output = self.work / "model.engine.json"
        wrapper.write_bytes(b"not-an-onnx-model")
        engine.write_bytes(b"serialized-engine")

        result = self.run_tool(wrapper, engine, output)

        self.assertEqual(result.returncode, 4, result.stderr)
        self.assertFalse(output.exists())
        self.assertEqual(result.stderr.count("event=fatal_error"), 1)
        self.assertNotIn("Traceback", result.stderr)
        self.assertIn("cannot load wrapper ONNX", result.stderr)

    def test_requires_force_to_replace_a_different_manifest(self) -> None:
        wrapper = self.work / "model.onnx"
        engine = self.work / "model.engine"
        output = self.work / "model.engine.json"
        make_wrapper(wrapper)
        engine.write_bytes(b"first-engine")
        first = self.run_tool(wrapper, engine, output)
        self.assertEqual(first.returncode, 0, first.stderr)
        first_contents = output.read_bytes()

        engine.write_bytes(b"second-engine")
        rejected = self.run_tool(wrapper, engine, output)
        self.assertEqual(rejected.returncode, 4, rejected.stderr)
        self.assertEqual(output.read_bytes(), first_contents)

        replaced = self.run_tool(wrapper, engine, output, "--force")
        self.assertEqual(replaced.returncode, 0, replaced.stderr)
        self.assertNotEqual(output.read_bytes(), first_contents)
        manifest = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(
            manifest["engine"]["sha256"],
            hashlib.sha256(b"second-engine").hexdigest(),
        )


if __name__ == "__main__":
    unittest.main()
