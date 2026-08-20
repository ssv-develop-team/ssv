"""Python Agent service command."""

from __future__ import annotations

from argparse import Namespace

from ..context import ProjectContext
from ..output import header, info
from ..process import exec_command, require_command, run_command


def run(context: ProjectContext, args: Namespace) -> int:
    header("启动 Python Agent 服务")
    uv = require_command("uv", "请安装 uv")
    agent_dir = context.root / "agent"
    if not (agent_dir / ".venv").is_dir():
        info("安装 Python 依赖...")
        result = run_command(context, [uv, "sync"], cwd=agent_dir)
        if result.returncode != 0:
            return result.returncode

    command = [uv, "run", "python", "-m", "ssv_agent"]
    config_path = context.resolve(args.config) if args.config else context.config_path
    if config_path is not None:
        info(f"配置: {config_path}")
        command.extend(["--config", str(config_path)])
    else:
        info("配置: 未找到本地运行配置，使用 Agent 内置默认值")
    if args.log_level:
        command.extend(["--log-level", args.log_level])
    info("按 Ctrl+C 停止")
    exec_command(context, command, cwd=agent_dir)
    return 0
