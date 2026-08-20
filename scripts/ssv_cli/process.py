"""Subprocess and executable handoff helpers."""

from __future__ import annotations

import os
import shutil
import subprocess
from collections.abc import Sequence
from pathlib import Path
from typing import NoReturn

from .context import ProjectContext
from .output import CliError


def require_command(command: str, hint: str | None = None) -> str:
    path = shutil.which(command)
    if path is None:
        suffix = f"；{hint}" if hint else ""
        raise CliError(f"{command} 未找到{suffix}")
    return path


def run_command(
    context: ProjectContext,
    argv: Sequence[str | Path],
    *,
    environment: dict[str, str] | None = None,
    cwd: Path | None = None,
    capture_output: bool = False,
    timeout: float | None = None,
) -> subprocess.CompletedProcess[str]:
    command = [str(item) for item in argv]
    try:
        return subprocess.run(
            command,
            cwd=str(cwd or context.root),
            env=environment or context.environment,
            text=True,
            capture_output=capture_output,
            check=False,
            timeout=timeout,
        )
    except FileNotFoundError as exc:
        raise CliError(f"命令未找到: {command[0]}") from exc
    except subprocess.TimeoutExpired as exc:
        raise CliError(f"命令超时: {' '.join(command)}") from exc


def exec_command(
    context: ProjectContext,
    argv: Sequence[str | Path],
    *,
    environment: dict[str, str] | None = None,
    cwd: Path | None = None,
) -> NoReturn:
    command = [str(item) for item in argv]
    try:
        os.chdir(cwd or context.root)
        os.execvpe(command[0], command, environment or context.environment)
    except FileNotFoundError as exc:
        raise CliError(f"命令未找到: {command[0]}") from exc
