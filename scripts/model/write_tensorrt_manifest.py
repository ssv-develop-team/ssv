#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Any, NoReturn

onnx: Any
TensorProto: Any

MANIFEST_SCHEMA = "ssv.tensorrt-engine-manifest"
MANIFEST_SCHEMA_VERSION = 1
WRAPPER_CONTRACT = "rgba_u8_nhwc_v1"
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}\Z")
TENSORRT_VERSION_PATTERN = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+\Z")
COMPUTE_CAPABILITY_PATTERN = re.compile(r"([0-9]+)\.([0-9]+)\Z")


def _load_onnx() -> None:
    global TensorProto, onnx

    import onnx as onnx_module
    from onnx import TensorProto as tensor_proto

    onnx = onnx_module
    TensorProto = tensor_proto


class ManifestError(RuntimeError):
    def __init__(
        self, message: str, *, exit_code: int = 4, stage: str = "engine_manifest"
    ) -> None:
        super().__init__(message)
        self.exit_code = exit_code
        self.stage = stage


class ManifestArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> NoReturn:
        raise ManifestError(message, exit_code=2, stage="cli")


def positive_integer(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def parse_args(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = ManifestArgumentParser(
        prog="./ssv model manifest",
        description="Write an SSV TensorRT engine manifest.",
        allow_abbrev=False,
    )
    parser.add_argument("--wrapper", required=True, type=Path)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--precision", required=True, choices=("fp32", "fp16"))
    parser.add_argument("--tensorrt-version", required=True)
    parser.add_argument("--cuda-runtime-version", required=True, type=positive_integer)
    parser.add_argument("--compute-capability", required=True)
    parser.add_argument("--force", action="store_true")
    return parser.parse_args(arguments)


def sha256_file(path: Path, label: str) -> str:
    try:
        with path.open("rb") as source:
            digest = hashlib.sha256()
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
            return digest.hexdigest()
    except OSError as error:
        raise ManifestError(f"cannot read {label}: {path}: {error}") from error


def required_metadata(metadata: dict[str, str], key: str) -> str:
    value = metadata.get(key, "")
    if not value:
        raise ManifestError(f"wrapper metadata is missing {key}")
    return value


def load_wrapper_contract(path: Path) -> dict[str, object]:
    _load_onnx()
    from google.protobuf.message import DecodeError

    try:
        model = onnx.load_model(path, load_external_data=False)
    except (OSError, DecodeError) as error:
        raise ManifestError(f"cannot load wrapper ONNX: {path}: {error}") from error

    metadata = {entry.key: entry.value for entry in model.metadata_props}
    fixed_properties = {
        "ssv.wrapper.contract": WRAPPER_CONTRACT,
        "ssv.wrapper.dtype": "uint8",
        "ssv.wrapper.layout": "NHWC",
        "ssv.wrapper.channel_rule": "drop_alpha_keep_rgb",
        "ssv.wrapper.normalization": "divide_by_255",
        "ssv.wrapper.tool": "ssv.prepare_wrapper",
    }
    for key, expected in fixed_properties.items():
        if required_metadata(metadata, key) != expected:
            raise ManifestError(f"wrapper metadata {key} must be {expected}")

    source_sha256 = required_metadata(metadata, "ssv.wrapper.source_sha256")
    if SHA256_PATTERN.fullmatch(source_sha256) is None:
        raise ManifestError(
            "wrapper metadata ssv.wrapper.source_sha256 must be lowercase SHA-256"
        )
    tool_version = required_metadata(metadata, "ssv.wrapper.tool_version")
    model_family = required_metadata(metadata, "ssv.wrapper.model_family")
    output_format = required_metadata(metadata, "ssv.wrapper.output_format")

    if len(model.graph.input) != 1:
        raise ManifestError("wrapper must have exactly one graph input")
    graph_input = model.graph.input[0]
    tensor_type = graph_input.type.tensor_type
    if tensor_type.elem_type != TensorProto.UINT8:
        raise ManifestError("wrapper input must use uint8")
    shape: list[int] = []
    for dimension in tensor_type.shape.dim:
        if dimension.WhichOneof("value") != "dim_value" or dimension.dim_value <= 0:
            raise ManifestError("wrapper input must be static uint8 [1,H,W,4]")
        shape.append(dimension.dim_value)
    if len(shape) != 4 or shape[0] != 1 or shape[3] != 4:
        raise ManifestError("wrapper input must be static uint8 [1,H,W,4]")
    if required_metadata(metadata, "ssv.wrapper.height") != str(
        shape[1]
    ) or required_metadata(metadata, "ssv.wrapper.width") != str(shape[2]):
        raise ManifestError("wrapper input shape does not match width/height metadata")

    return {
        "contract": WRAPPER_CONTRACT,
        "source_sha256": source_sha256,
        "tool_version": tool_version,
        "model_family": model_family,
        "output_format": output_format,
        "input": {
            "name": graph_input.name,
            "dtype": "uint8",
            "layout": "NHWC",
            "shape": shape,
        },
    }


def build_manifest(arguments: argparse.Namespace) -> dict[str, object]:
    if TENSORRT_VERSION_PATTERN.fullmatch(arguments.tensorrt_version) is None:
        raise ManifestError("--tensorrt-version must use major.minor.patch.build")
    capability = COMPUTE_CAPABILITY_PATTERN.fullmatch(arguments.compute_capability)
    if capability is None:
        raise ManifestError("--compute-capability must use major.minor")

    wrapper = load_wrapper_contract(arguments.wrapper)
    wrapper["sha256"] = sha256_file(arguments.wrapper, "wrapper ONNX")
    return {
        "schema": MANIFEST_SCHEMA,
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "engine": {
            "sha256": sha256_file(arguments.engine, "TensorRT engine"),
            "precision": arguments.precision,
            "tensorrt_version": arguments.tensorrt_version,
            "cuda_runtime_version": arguments.cuda_runtime_version,
            "compute_capability": {
                "major": int(capability.group(1)),
                "minor": int(capability.group(2)),
            },
        },
        "wrapper": wrapper,
    }


def write_manifest(path: Path, manifest: dict[str, object], force: bool) -> None:
    contents = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    if path.exists():
        try:
            existing = path.read_text(encoding="utf-8")
        except OSError as error:
            raise ManifestError(
                f"cannot read existing manifest: {path}: {error}"
            ) from error
        if existing == contents:
            return
        if not force:
            raise ManifestError(
                f"refusing to replace a different manifest without --force: {path}"
            )

    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
        )
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                output.write(contents)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary_name, path)
        except BaseException:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass
            raise
    except OSError as error:
        raise ManifestError(f"cannot write manifest: {path}: {error}") from error


def format_log_value(value: str) -> str:
    if value and all(
        character.isalnum() or character in "_-./:" for character in value
    ):
        return value
    return json.dumps(value, ensure_ascii=False)


def main(arguments: list[str] | None = None) -> int:
    try:
        parsed = parse_args(arguments)
        manifest = build_manifest(parsed)
        write_manifest(parsed.output, manifest, parsed.force)
        print(
            "event=tensorrt_manifest_written"
            f" output={format_log_value(str(parsed.output))}"
            f" engine_sha256={manifest['engine']['sha256']}"
        )
        return 0
    except ManifestError as error:
        print(
            f"event=fatal_error exit_code={error.exit_code}"
            f" stage={format_log_value(error.stage)}"
            " source_id=model-manifest"
            f" error={format_log_value(str(error))}",
            file=sys.stderr,
        )
        return error.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
