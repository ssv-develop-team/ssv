"""Small, testable output and error helpers for the CLI."""

from __future__ import annotations

import sys
from typing import NoReturn


class CliError(Exception):
    """An expected user-facing CLI failure."""

    def __init__(self, message: str, exit_code: int = 1) -> None:
        super().__init__(message)
        self.exit_code = exit_code


def info(message: str) -> None:
    print(f"[SSV] {message}")


def warn(message: str) -> None:
    print(f"[SSV] 警告: {message}", file=sys.stderr)


def error(message: str) -> None:
    print(f"[SSV] 错误: {message}", file=sys.stderr)


def header(message: str) -> None:
    print(f"\n── {message} ──\n")


def fail(message: str, exit_code: int = 1) -> NoReturn:
    raise CliError(message, exit_code)
