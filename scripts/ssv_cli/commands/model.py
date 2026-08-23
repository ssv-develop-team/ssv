"""Model lifecycle commands."""

from __future__ import annotations

from argparse import Namespace

from ..context import ProjectContext
from ..output import header
from ..services.models import ModelService


def export(context: ProjectContext, _args: Namespace) -> int:
    header("导出 YOLO ONNX 模型")
    return ModelService(context).export_default()


def verify(context: ProjectContext, args: Namespace) -> int:
    return ModelService(context).verify(list(getattr(args, "model_args", ())))


def write_manifest(context: ProjectContext, args: Namespace) -> int:
    return ModelService(context).write_manifest(list(getattr(args, "model_args", ())))
