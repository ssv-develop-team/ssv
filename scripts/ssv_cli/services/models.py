"""Python CLI 使用的模型生命周期服务。"""

from __future__ import annotations

import argparse
import contextlib
import importlib
import json
import shutil
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from ..context import ProjectContext
from ..output import CliError, info


def _optional_dependency_error(package: str, extra: str) -> CliError:
    return CliError(
        f"模型操作需要 Python 依赖 {package}；请安装项目 extra: "
        f"python -m pip install -e '.[{extra}]'"
    )


def _import_optional(
    module_name: str, *, packages: Sequence[str], extra: str
) -> Any:
    try:
        return importlib.import_module(module_name)
    except ModuleNotFoundError as exc:
        missing = (exc.name or module_name).split(".", 1)[0]
        package = next((candidate for candidate in packages if missing == candidate), None)
        if package is not None:
            raise _optional_dependency_error(package, extra) from exc
        raise CliError(
            f"加载模型工具 {module_name} 失败：内部依赖 {missing} 不可用；请检查项目环境"
        ) from exc
    except (ImportError, OSError) as exc:
        raise CliError(f"加载模型工具 {module_name} 失败: {exc}") from exc


def _require_optional(packages: Sequence[str], *, extra: str) -> None:
    for package in packages:
        _import_optional(package, packages=packages, extra=extra)


class ModelService:
    """将模型工具适配到项目上下文，不调用包管理器。"""

    def __init__(self, context: ProjectContext) -> None:
        self.context = context

    def export_default(self) -> int:
        model_file = self.context.root / "models" / "yolov8n.onnx"
        if model_file.is_file():
            info(f"模型已存在: {model_file}")
            return 0
        ultralytics = _import_optional(
            "ultralytics", packages=("ultralytics",), extra="model-export"
        )
        model_file.parent.mkdir(parents=True, exist_ok=True)
        try:
            info("使用已安装的 ultralytics 导出 YOLOv8n ONNX...")
            with contextlib.chdir(self.context.root):
                model = ultralytics.YOLO("yolov8n.pt")
                exported = model.export(format="onnx", imgsz=640, simplify=True)
            exported_path = Path(str(exported))
            if not exported_path.is_absolute():
                exported_path = self.context.root / exported_path
            if not exported_path.is_file():
                fallback = self.context.root / "yolov8n.onnx"
                exported_path = fallback if fallback.is_file() else exported_path
            if not exported_path.is_file():
                raise CliError(f"ultralytics 导出完成但没有生成 ONNX 文件: {exported_path}")
            if exported_path.resolve() != model_file.resolve():
                shutil.move(str(exported_path), str(model_file))
        except CliError:
            raise
        except (ImportError, OSError, RuntimeError, ValueError) as exc:
            raise CliError(f"YOLOv8n ONNX 导出失败: {exc}") from exc
        info(f"导出成功: {model_file}")
        return 0

    def prepare(self, arguments: list[str]) -> int:
        module = _import_optional(
            "scripts.model.prepare_wrapper",
            packages=("numpy", "onnx", "onnxruntime"),
            extra="model",
        )
        try:
            options = module.parse_args(arguments)
            _require_optional(("numpy", "onnx", "onnxruntime"), extra="model")
            options.input = self.context.resolve(options.input)
            options.output = self.context.resolve(options.output)
            status, source_sha256 = module.prepare_wrapper(options)
        except SystemExit as exc:
            return int(exc.code or 0)
        except CliError:
            raise
        except module.PrepareModelError as exc:
            print(module.format_fatal_error(exc), file=sys.stderr)
            return exc.exit_code
        except Exception as exc:  # noqa: BLE001
            fatal = module.PrepareModelError(f"unexpected model preparation failure: {exc}")
            print(module.format_fatal_error(fatal), file=sys.stderr)
            return fatal.exit_code
        print(
            "event=model_prepared status="
            f"{status} output={module.format_log_value(str(options.output))}"
            f" contract={module.WRAPPER_CONTRACT} source_sha256={source_sha256}"
        )
        return 0

    def verify(self, arguments: list[str]) -> int:
        module = _import_optional(
            "scripts.model.verify_helmet_models",
            packages=("ultralytics",),
            extra="model-export",
        )
        parser = argparse.ArgumentParser(
            prog="./ssv model verify",
            description="Verify helmet YOLO .pt models.",
            allow_abbrev=False,
        )
        parser.add_argument("--models-dir", default="models")
        parser.add_argument("--source", default="")
        parser.add_argument("--export", default="")
        parser.add_argument("--output", default="artifacts/model-verification/helmet-model-summary.json")
        try:
            options = parser.parse_args(arguments)
        except SystemExit as exc:
            return int(exc.code or 0)
        _require_optional(("ultralytics",), extra="model-export")
        models_dir = self.context.resolve(options.models_dir)
        source = self.context.resolve(options.source) if options.source else None
        export = self.context.resolve(options.export) if options.export else None
        model_paths = sorted(models_dir.glob("comp-*.pt"))
        if not model_paths:
            raise CliError(f"No comp-*.pt models found in {models_dir}")
        try:
            summary = {"models": []}
            for path in model_paths:
                summary["models"].append(
                    module.verify_model(
                        path, source, export is not None and path.resolve() == export.resolve()
                    )
                )
            output = self.context.resolve(options.output)
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(
                json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
            )
        except Exception as exc:
            raise CliError(f"安全帽模型验证失败: {exc}") from exc
        print(f"Wrote {output}")
        return 0

    def write_manifest(self, arguments: list[str]) -> int:
        module = _import_optional(
            "scripts.model.write_tensorrt_manifest",
            packages=("onnx",),
            extra="model",
        )
        try:
            options = module.parse_args(arguments)
            _require_optional(("onnx",), extra="model")
            options.wrapper = self.context.resolve(options.wrapper)
            options.engine = self.context.resolve(options.engine)
            options.output = self.context.resolve(options.output)
            manifest = module.build_manifest(options)
            module.write_manifest(options.output, manifest, options.force)
        except SystemExit as exc:
            return int(exc.code or 0)
        except module.ManifestError as exc:
            print(
                f"event=fatal_error exit_code={exc.exit_code} stage={module.format_log_value(exc.stage)}"
                f" source_id=model-manifest error={module.format_log_value(str(exc))}",
                file=sys.stderr,
            )
            return exc.exit_code
        print(
            f"event=tensorrt_manifest_written output={module.format_log_value(str(options.output))}"
            f" engine_sha256={manifest['engine']['sha256']}"
        )
        return 0
