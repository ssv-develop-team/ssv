"""GStreamer plugin inspection."""

from __future__ import annotations

from argparse import Namespace

from ..context import ProjectContext
from ..output import header
from ..process import require_command, run_command
from ..services.runtime_env import load_runtime_environment


def run(context: ProjectContext, _args: Namespace) -> int:
    header("检查插件: ssvtemplate")
    require_command("gst-inspect-1.0", "请安装 GStreamer tools")
    environment = load_runtime_environment(context)
    environment["GST_DEBUG"] = context.environment.get("GST_DEBUG", "ssv*:5")
    result = run_command(
        context,
        ["gst-inspect-1.0", "ssvtemplate"],
        environment=environment,
    )
    return result.returncode
