"""Native C++ runner handoff."""

from __future__ import annotations

from argparse import Namespace

from ..context import ProjectContext
from ..output import CliError
from ..process import exec_command
from ..services.runtime_env import load_runtime_environment


def run(context: ProjectContext, args: Namespace) -> int:
    environment = load_runtime_environment(context)
    runner = context.build_dir / "runner" / "ssv-runner"
    if not runner.is_file() or not runner.stat().st_mode & 0o111:
        raise CliError(f"runner 不存在或不可执行: {runner}; 请先运行 ./ssv build")
    exec_command(context, [runner, *getattr(args, "runner_args", ())], environment=environment)
    return 0
