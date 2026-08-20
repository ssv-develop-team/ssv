#!/usr/bin/env python3

from __future__ import annotations

import argparse
import ctypes
import errno
import fcntl
import hashlib
import os
import sys
import tempfile
from pathlib import Path
from typing import Any, NoReturn

np: Any
onnx: Any
ort: Any
TensorProto: Any
helper: Any
numpy_helper: Any
shape_inference: Any

WRAPPER_CONTRACT = "rgba_u8_nhwc_v1"
TOOL_NAME = "ssv.prepare_wrapper"
TOOL_VERSION = "1.0.0"
SUPPORTED_OUTPUT_FORMATS = ("yolov8", "yolo_nx6")


def _load_dependencies() -> None:
    global TensorProto, helper, np, numpy_helper, onnx, ort, shape_inference

    import numpy as numpy_module
    import onnx as onnx_module
    import onnxruntime as ort_module
    from onnx import TensorProto as tensor_proto
    from onnx import helper as helper_module
    from onnx import numpy_helper as numpy_helper_module
    from onnx import shape_inference as shape_inference_module

    np = numpy_module
    onnx = onnx_module
    ort = ort_module
    TensorProto = tensor_proto
    helper = helper_module
    numpy_helper = numpy_helper_module
    shape_inference = shape_inference_module


class PrepareModelError(RuntimeError):
    def __init__(
        self, message: str, *, exit_code: int = 4, stage: str = "model_prepare"
    ) -> None:
        super().__init__(message)
        self.exit_code = exit_code
        self.stage = stage


class PrepareModelArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> NoReturn:
        raise PrepareModelError(message, exit_code=2, stage="cli")


class StoreOnce(argparse.Action):
    def __call__(
        self,
        parser: argparse.ArgumentParser,
        namespace: argparse.Namespace,
        values: object,
        option_string: str | None = None,
    ) -> None:
        if getattr(namespace, self.dest, None) is not None:
            parser.error(f"{option_string} may be specified only once")
        setattr(namespace, self.dest, values)


class SetTrueOnce(argparse.Action):
    def __init__(self, *args: object, **kwargs: object) -> None:
        super().__init__(*args, nargs=0, **kwargs)

    def __call__(
        self,
        parser: argparse.ArgumentParser,
        namespace: argparse.Namespace,
        values: object,
        option_string: str | None = None,
    ) -> None:
        if getattr(namespace, self.dest, False):
            parser.error(f"{option_string} may be specified only once")
        setattr(namespace, self.dest, True)


def parse_args(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = PrepareModelArgumentParser(
        prog="./ssv model prepare",
        description="Prepare an SSV RGBA uint8 wrapper ONNX model.",
        allow_abbrev=False,
    )
    parser.add_argument(
        "--input",
        required=True,
        type=Path,
        action=StoreOnce,
        help="source ONNX model",
    )
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        action=StoreOnce,
        help="wrapper ONNX model",
    )
    parser.add_argument("--family", required=True, choices=("yolo",), action=StoreOnce)
    parser.add_argument(
        "--output-format",
        required=True,
        choices=SUPPORTED_OUTPUT_FORMATS,
        action=StoreOnce,
    )
    parser.add_argument("--force", action=SetTrueOnce, default=False)
    return parser.parse_args(arguments)


def format_log_value(value: str) -> str:
    safe_characters = set("_-./:")
    if value and all(
        character.isalnum() or character in safe_characters for character in value
    ):
        return value
    escaped = (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )
    return f'"{escaped}"'


def format_fatal_error(error: PrepareModelError) -> str:
    return (
        f"event=fatal_error exit_code={error.exit_code}"
        f" stage={format_log_value(error.stage)}"
        " source_id=model-prepare"
        f" error={format_log_value(str(error))}"
    )


def require_source_contract(model: onnx.ModelProto) -> tuple[str, int, int]:
    _load_dependencies()
    if len(model.graph.input) != 1:
        raise PrepareModelError(
            f"input model must have exactly one graph input, got {len(model.graph.input)}"
        )

    source_input = model.graph.input[0]
    tensor_type = source_input.type.tensor_type
    if tensor_type.elem_type != TensorProto.FLOAT:
        raise PrepareModelError("input model tensor must use float32")

    dimensions: list[int] = []
    for dimension in tensor_type.shape.dim:
        if dimension.WhichOneof("value") != "dim_value" or dimension.dim_value <= 0:
            raise PrepareModelError("input model shape must be static [1,3,H,W]")
        dimensions.append(dimension.dim_value)
    if len(dimensions) != 4 or dimensions[:2] != [1, 3]:
        raise PrepareModelError("input model shape must be static [1,3,H,W]")

    initializer_names = {initializer.name for initializer in model.graph.initializer}
    if source_input.name in initializer_names:
        raise PrepareModelError(
            "input model image input must not also be an initializer"
        )
    return source_input.name, dimensions[2], dimensions[3]


def unique_graph_name(model: onnx.ModelProto, base: str) -> str:
    used_names = {
        value.name
        for values in (model.graph.input, model.graph.output, model.graph.value_info)
        for value in values
    }
    used_names.update(initializer.name for initializer in model.graph.initializer)
    for node in model.graph.node:
        used_names.update(node.input)
        used_names.update(node.output)

    candidate = base
    suffix = 2
    while candidate in used_names:
        candidate = f"{base}_{suffix}"
        suffix += 1
    return candidate


def set_wrapper_metadata(
    model: onnx.ModelProto,
    *,
    width: int,
    height: int,
    source_sha256: str,
    family: str,
    output_format: str,
) -> None:
    metadata = {item.key: item.value for item in model.metadata_props}
    if any(key.startswith("ssv.wrapper.") for key in metadata):
        raise PrepareModelError("input model already contains SSV wrapper metadata")
    metadata.update(
        {
            "ssv.wrapper.channel_rule": "drop_alpha_keep_rgb",
            "ssv.wrapper.contract": WRAPPER_CONTRACT,
            "ssv.wrapper.dtype": "uint8",
            "ssv.wrapper.height": str(height),
            "ssv.wrapper.layout": "NHWC",
            "ssv.wrapper.model_family": family,
            "ssv.wrapper.normalization": "divide_by_255",
            "ssv.wrapper.output_format": output_format,
            "ssv.wrapper.source_sha256": source_sha256,
            "ssv.wrapper.tool": TOOL_NAME,
            "ssv.wrapper.tool_version": TOOL_VERSION,
            "ssv.wrapper.width": str(width),
        }
    )
    del model.metadata_props[:]
    for key in sorted(metadata):
        item = model.metadata_props.add()
        item.key = key
        item.value = metadata[key]


def build_wrapper_model(
    source_bytes: bytes, *, family: str, output_format: str
) -> tuple[onnx.ModelProto, int, int, str]:
    _load_dependencies()
    source_sha256 = hashlib.sha256(source_bytes).hexdigest()
    try:
        model = onnx.load_model_from_string(source_bytes)
        onnx.checker.check_model(model, full_check=True)
    except Exception as error:
        raise PrepareModelError(f"invalid input ONNX model: {error}") from error

    source_input_name, height, width = require_source_contract(model)
    rgba_input = unique_graph_name(model, "ssv.wrapper.rgba")
    channel_indices = unique_graph_name(model, "ssv.wrapper.rgb_indices")
    gathered = unique_graph_name(model, "ssv.wrapper.rgb_u8")
    casted = unique_graph_name(model, "ssv.wrapper.rgb_f32")
    divisor = unique_graph_name(model, "ssv.wrapper.divisor")
    normalized = unique_graph_name(model, "ssv.wrapper.normalized")

    original_nodes = list(model.graph.node)
    del model.graph.node[:]
    model.graph.node.extend(
        [
            helper.make_node(
                "Gather",
                [rgba_input, channel_indices],
                [gathered],
                axis=3,
                name="ssv.wrapper.gather_rgb",
            ),
            helper.make_node(
                "Cast",
                [gathered],
                [casted],
                to=TensorProto.FLOAT,
                name="ssv.wrapper.cast_float",
            ),
            helper.make_node(
                "Div",
                [casted, divisor],
                [normalized],
                name="ssv.wrapper.normalize",
            ),
            helper.make_node(
                "Transpose",
                [normalized],
                [source_input_name],
                perm=[0, 3, 1, 2],
                name="ssv.wrapper.transpose_nchw",
            ),
        ]
    )
    model.graph.node.extend(original_nodes)
    model.graph.initializer.extend(
        [
            numpy_helper.from_array(
                np.asarray([0, 1, 2], dtype=np.int64), name=channel_indices
            ),
            numpy_helper.from_array(np.asarray(255.0, dtype=np.float32), name=divisor),
        ]
    )
    del model.graph.input[:]
    model.graph.input.extend(
        [
            helper.make_tensor_value_info(
                rgba_input, TensorProto.UINT8, [1, height, width, 4]
            )
        ]
    )
    model.producer_name = TOOL_NAME
    model.producer_version = TOOL_VERSION
    set_wrapper_metadata(
        model,
        width=width,
        height=height,
        source_sha256=source_sha256,
        family=family,
        output_format=output_format,
    )
    return model, height, width, source_sha256


def static_tensor_dimensions(value: onnx.ValueInfoProto) -> list[int]:
    dimensions: list[int] = []
    for dimension in value.type.tensor_type.shape.dim:
        if dimension.WhichOneof("value") != "dim_value" or dimension.dim_value <= 0:
            raise PrepareModelError(
                "wrapper input dimensions must be static and positive"
            )
        dimensions.append(dimension.dim_value)
    return dimensions


def node_attribute(node: onnx.NodeProto, name: str) -> object:
    _load_dependencies()
    for attribute in node.attribute:
        if attribute.name == name:
            return helper.get_attribute_value(attribute)
    raise PrepareModelError(f"wrapper {node.op_type} node is missing attribute {name}")


def validate_wrapper_contract(model: onnx.ModelProto) -> tuple[int, int]:
    _load_dependencies()
    metadata = {item.key: item.value for item in model.metadata_props}
    if len(metadata) != len(model.metadata_props):
        raise PrepareModelError("wrapper metadata contains duplicate keys")

    expected_metadata = {
        "ssv.wrapper.channel_rule": "drop_alpha_keep_rgb",
        "ssv.wrapper.contract": WRAPPER_CONTRACT,
        "ssv.wrapper.dtype": "uint8",
        "ssv.wrapper.layout": "NHWC",
        "ssv.wrapper.model_family": "yolo",
        "ssv.wrapper.normalization": "divide_by_255",
        "ssv.wrapper.tool": TOOL_NAME,
    }
    for key, expected in expected_metadata.items():
        if metadata.get(key) != expected:
            raise PrepareModelError(f"wrapper metadata {key} must be {expected!r}")
    output_format = metadata.get("ssv.wrapper.output_format")
    if output_format not in SUPPORTED_OUTPUT_FORMATS:
        raise PrepareModelError(
            "wrapper metadata ssv.wrapper.output_format must be one of "
            + ", ".join(SUPPORTED_OUTPUT_FORMATS)
        )
    if not metadata.get("ssv.wrapper.tool_version"):
        raise PrepareModelError("wrapper metadata is missing ssv.wrapper.tool_version")

    source_sha256 = metadata.get("ssv.wrapper.source_sha256", "")
    try:
        valid_hash = len(source_sha256) == 64 and int(source_sha256, 16) >= 0
    except ValueError:
        valid_hash = False
    if not valid_hash or source_sha256.lower() != source_sha256:
        raise PrepareModelError("wrapper source SHA-256 metadata is invalid")

    try:
        height = int(metadata["ssv.wrapper.height"])
        width = int(metadata["ssv.wrapper.width"])
    except (KeyError, ValueError) as error:
        raise PrepareModelError(
            "wrapper width and height metadata must be integers"
        ) from error
    if height <= 0 or width <= 0:
        raise PrepareModelError("wrapper width and height metadata must be positive")

    if len(model.graph.input) != 1:
        raise PrepareModelError(
            f"wrapper must have exactly one graph input, got {len(model.graph.input)}"
        )
    wrapper_input = model.graph.input[0]
    if wrapper_input.type.tensor_type.elem_type != TensorProto.UINT8:
        raise PrepareModelError("wrapper input tensor must use uint8")
    if static_tensor_dimensions(wrapper_input) != [1, height, width, 4]:
        raise PrepareModelError("wrapper input must match uint8 [1,H,W,4] metadata")

    if [node.op_type for node in model.graph.node[:4]] != [
        "Gather",
        "Cast",
        "Div",
        "Transpose",
    ]:
        raise PrepareModelError(
            "wrapper graph must begin with Gather, Cast, Div, and Transpose"
        )
    gather, cast, divide, transpose = model.graph.node[:4]
    if (
        gather.input[0] != wrapper_input.name
        or gather.output[0] != cast.input[0]
        or cast.output[0] != divide.input[0]
        or divide.output[0] != transpose.input[0]
    ):
        raise PrepareModelError(
            "wrapper preprocessing nodes are not connected in order"
        )
    if node_attribute(gather, "axis") != 3:
        raise PrepareModelError("wrapper Gather axis must be 3")
    if node_attribute(cast, "to") != TensorProto.FLOAT:
        raise PrepareModelError("wrapper Cast target must be float32")
    if list(node_attribute(transpose, "perm")) != [0, 3, 1, 2]:
        raise PrepareModelError("wrapper Transpose permutation must be [0,3,1,2]")

    initializers = {item.name: item for item in model.graph.initializer}
    try:
        indices = numpy_helper.to_array(initializers[gather.input[1]])
        divisor = numpy_helper.to_array(initializers[divide.input[1]])
    except KeyError as error:
        raise PrepareModelError(
            "wrapper preprocessing initializer is missing"
        ) from error
    if indices.dtype != np.int64 or not np.array_equal(
        indices, np.asarray([0, 1, 2], dtype=np.int64)
    ):
        raise PrepareModelError("wrapper Gather indices must select RGB channels")
    if divisor.dtype != np.float32 or divisor.shape != () or float(divisor) != 255.0:
        raise PrepareModelError("wrapper normalization divisor must be float32 255")
    return height, width


def run_cpu_smoke(
    model_bytes: bytes, *, height: int, width: int, output_count: int
) -> None:
    session_options = ort.SessionOptions()
    session_options.log_severity_level = 3
    session = ort.InferenceSession(
        model_bytes,
        sess_options=session_options,
        providers=["CPUExecutionProvider"],
    )
    inputs = session.get_inputs()
    if len(inputs) != 1:
        raise PrepareModelError(
            f"wrapper CPU smoke expected one input, got {len(inputs)}"
        )
    sample = np.zeros((1, height, width, 4), dtype=np.uint8)
    outputs = session.run(None, {inputs[0].name: sample})
    if len(outputs) != output_count:
        raise PrepareModelError("wrapper CPU smoke returned an unexpected output set")


def validate_and_serialize(model: onnx.ModelProto, *, height: int, width: int) -> bytes:
    _load_dependencies()
    try:
        onnx.checker.check_model(model, full_check=True)
        inferred = shape_inference.infer_shapes(
            model, check_type=True, strict_mode=True, data_prop=False
        )
        onnx.checker.check_model(inferred, full_check=True)
        candidate_bytes = inferred.SerializeToString(deterministic=True)
        contract_height, contract_width = validate_wrapper_contract(inferred)
        if (contract_height, contract_width) != (height, width):
            raise PrepareModelError("wrapper dimensions changed during shape inference")
        run_cpu_smoke(
            candidate_bytes,
            height=height,
            width=width,
            output_count=len(inferred.graph.output),
        )
        return candidate_bytes
    except PrepareModelError:
        raise
    except Exception as error:
        raise PrepareModelError(f"wrapper validation failed: {error}") from error


def validate_wrapper_model(model_bytes: bytes) -> None:
    _load_dependencies()
    try:
        model = onnx.load_model_from_string(model_bytes)
        onnx.checker.check_model(model, full_check=True)
        inferred = shape_inference.infer_shapes(
            model, check_type=True, strict_mode=True, data_prop=False
        )
        onnx.checker.check_model(inferred, full_check=True)
        height, width = validate_wrapper_contract(inferred)
        inferred_bytes = inferred.SerializeToString(deterministic=True)
        run_cpu_smoke(
            inferred_bytes,
            height=height,
            width=width,
            output_count=len(inferred.graph.output),
        )
    except PrepareModelError:
        raise
    except Exception as error:
        raise PrepareModelError(
            f"existing wrapper validation failed: {error}"
        ) from error


def exchange_paths(first: Path, second: Path) -> None:
    rename_exchange = 2
    libc = ctypes.CDLL(None, use_errno=True)
    try:
        renameat2 = libc.renameat2
    except AttributeError as error:
        raise OSError(
            errno.ENOSYS, "atomic wrapper replacement requires Linux renameat2", first
        ) from error
    renameat2.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    ]
    renameat2.restype = ctypes.c_int
    if (
        renameat2(
            -100,
            os.fsencode(first),
            -100,
            os.fsencode(second),
            rename_exchange,
        )
        != 0
    ):
        error_number = ctypes.get_errno()
        raise OSError(error_number, os.strerror(error_number), first)


def publish_wrapper(output: Path, candidate_bytes: bytes, *, force: bool) -> str:
    output.parent.mkdir(parents=True, exist_ok=True)
    directory_descriptor = os.open(output.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        # Serialize cooperating publishers without leaving a lock file beside models.
        fcntl.flock(directory_descriptor, fcntl.LOCK_EX)
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
        )
        temporary_path = Path(temporary_name)
        cleanup_temporary = True
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(candidate_bytes)
                stream.flush()
                os.fsync(stream.fileno())
            os.chmod(temporary_path, 0o644)

            try:
                # Linking a complete same-directory file publishes atomically and
                # cannot replace a path created after validation began.
                os.link(temporary_path, output)
                return "created"
            except FileExistsError:
                pass
            except OSError as error:
                raise PrepareModelError(
                    f"cannot publish output atomically: {error}"
                ) from error

            try:
                existing_bytes = output.read_bytes()
            except OSError as error:
                raise PrepareModelError(
                    f"cannot read existing output: {error}"
                ) from error
            if existing_bytes == candidate_bytes:
                return "unchanged"
            if not force:
                raise PrepareModelError(
                    "output already exists with different content; pass --force only to replace a verified SSV wrapper"
                )
            try:
                validate_wrapper_model(existing_bytes)
            except PrepareModelError as error:
                raise PrepareModelError(
                    f"refusing --force because the existing output is not a valid SSV wrapper: {error}"
                ) from error

            try:
                current_bytes = output.read_bytes()
            except OSError as error:
                raise PrepareModelError(
                    f"cannot recheck existing output before replacement: {error}"
                ) from error
            if current_bytes != existing_bytes:
                raise PrepareModelError(
                    "refusing --force because the existing output changed during validation"
                )

            # From this point onward the temporary path may acquire displaced
            # user-owned content. Preserve it across asynchronous interruption.
            cleanup_temporary = False
            try:
                exchange_paths(output, temporary_path)
            except OSError as error:
                cleanup_temporary = True
                raise PrepareModelError(
                    f"cannot atomically replace verified wrapper output: {error}"
                ) from error

            # The displaced path is preserved until it is proven to be the exact
            # wrapper validated above. A non-cooperating writer therefore cannot
            # make --force destroy an arbitrary file in the final race window.
            displaced_error: str | None = None
            try:
                displaced_bytes = temporary_path.read_bytes()
            except OSError as error:
                displaced_error = f"cannot verify displaced output: {error}"
            else:
                if displaced_bytes != existing_bytes:
                    displaced_error = (
                        "existing output changed during atomic replacement"
                    )

            if displaced_error is not None:
                try:
                    exchange_paths(output, temporary_path)
                except OSError as rollback_error:
                    raise PrepareModelError(
                        f"{displaced_error}; rollback failed and displaced output was preserved at {temporary_path}: {rollback_error}"
                    ) from rollback_error
                cleanup_temporary = True
                raise PrepareModelError(
                    f"refusing --force because the {displaced_error}"
                )

            cleanup_temporary = True
            return "replaced"
        finally:
            if cleanup_temporary:
                temporary_path.unlink(missing_ok=True)
    finally:
        fcntl.flock(directory_descriptor, fcntl.LOCK_UN)
        os.close(directory_descriptor)


def prepare_wrapper(options: argparse.Namespace) -> tuple[str, str]:
    _load_dependencies()
    try:
        source_bytes = options.input.read_bytes()
    except OSError as error:
        raise PrepareModelError(f"cannot read input model: {error}") from error

    model, height, width, source_sha256 = build_wrapper_model(
        source_bytes,
        family=options.family,
        output_format=options.output_format,
    )
    candidate_bytes = validate_and_serialize(model, height=height, width=width)
    status = publish_wrapper(options.output, candidate_bytes, force=options.force)
    return status, source_sha256


def main(arguments: list[str] | None = None) -> int:
    try:
        options = parse_args(arguments)
        status, source_sha256 = prepare_wrapper(options)
    except PrepareModelError as error:
        print(format_fatal_error(error), file=sys.stderr)
        return error.exit_code
    # The CLI boundary must turn even unexpected library failures into one fatal record.
    except Exception as error:  # noqa: BLE001
        fatal = PrepareModelError(f"unexpected model preparation failure: {error}")
        print(format_fatal_error(fatal), file=sys.stderr)
        return fatal.exit_code

    print(
        f"event=model_prepared status={status}"
        f" output={format_log_value(str(options.output))}"
        f" contract={WRAPPER_CONTRACT} source_sha256={source_sha256}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
