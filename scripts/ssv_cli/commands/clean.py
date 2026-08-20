"""Build artifact cleanup."""

from __future__ import annotations

import shutil
from argparse import Namespace
from pathlib import Path

from ..context import ProjectContext
from ..output import CliError, header, info

_BUILD_MARKERS = ("build.ninja", "meson-info", "ssv-deps.env")


def _validate_target(context: ProjectContext, target: Path) -> None:
    if target.is_symlink():
        raise CliError(f"拒绝清理符号链接构建目录: {target}")

    resolved = target.resolve()
    if context.root == resolved or context.root.is_relative_to(resolved):
        raise CliError(f"拒绝清理项目根目录或其祖先: {target}")

    if target.is_dir() and not any((target / marker).exists() for marker in _BUILD_MARKERS):
        raise CliError(f"拒绝清理未识别的目录: {target}; 缺少 Meson 或依赖快照标记")


def run(context: ProjectContext, _args: Namespace) -> int:
    header("清理构建目录")
    target = context.build_dir
    _validate_target(context, target)
    if target.is_dir():
        shutil.rmtree(target)
        info(f"已删除: {context.display_path(target)}")
    else:
        info(f"构建目录不存在或不是目录: {context.display_path(target)}")
    return 0
