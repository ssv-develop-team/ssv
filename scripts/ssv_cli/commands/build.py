"""Native build command."""

from __future__ import annotations

from argparse import Namespace

from ..context import ProjectContext
from ..output import header
from ..services.native_build import NativeBuildService


def run(context: ProjectContext, args: Namespace) -> int:
    header("编译 GStreamer 插件")
    return NativeBuildService(context).build(args.profile or "auto")
