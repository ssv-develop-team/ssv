"""Load the validated dependency snapshot for native runtime commands."""

from __future__ import annotations

import re
import shlex
from pathlib import Path

from ..context import ProjectContext
from ..output import CliError
from .dependencies import SNAPSHOT_KEYS

_SNAPSHOT_LINE = re.compile(r"^([A-Z0-9_]+)=(.*)$")
_UNSAFE = (";", "`", "&", "|", "<", ">", "$(")
_SNAPSHOT_KEYS = SNAPSHOT_KEYS
_SNAPSHOT_KEY_SET = frozenset(_SNAPSHOT_KEYS)


def _decode_shell_word(raw: str, *, path: Path, line_number: int) -> str:
    if any(token in raw for token in _UNSAFE):
        raise CliError(f"unsafe dependency snapshot line: {path}:{line_number}")
    if raw == "":
        return ""
    try:
        values = shlex.split(raw, comments=False, posix=True)
    except ValueError as exc:
        raise CliError(f"invalid dependency snapshot line: {path}:{line_number}") from exc
    if len(values) != 1:
        raise CliError(f"invalid dependency snapshot line: {path}:{line_number}")
    return values[0]


def load_dependency_snapshot(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise CliError(f"dependency snapshot not found: {path}; run ./ssv build first")
    values: dict[str, str] = {}
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = _SNAPSHOT_LINE.fullmatch(raw_line)
        if match is None:
            raise CliError(f"invalid dependency snapshot line: {path}:{line_number}")
        key, raw_value = match.groups()
        if key not in _SNAPSHOT_KEY_SET:
            raise CliError(f"unknown variable in dependency snapshot: {key}")
        if key in values:
            raise CliError(f"duplicate variable in dependency snapshot: {key}")
        values[key] = _decode_shell_word(raw_value, path=path, line_number=line_number)
    missing = [key for key in _SNAPSHOT_KEYS if key not in values]
    if missing:
        raise CliError(f"missing variable in dependency snapshot: {missing[0]}")
    return values


def _join_unique(*groups: str) -> str:
    seen: set[str] = set()
    result: list[str] = []
    for group in groups:
        for item in group.split(":"):
            if item and item not in seen:
                seen.add(item)
                result.append(item)
    return ":".join(result)


def load_runtime_environment(context: ProjectContext) -> dict[str, str]:
    """Return the child environment required by the native runtime."""

    snapshot = load_dependency_snapshot(context.build_dir / "ssv-deps.env")
    environment = dict(context.environment)
    environment.update(snapshot)

    plugin_paths = [
        context.build_dir / "gst" / "ssv-template",
        context.build_dir / "gst" / "ssv-infer",
        context.build_dir / "gst" / "ssv-track",
        context.build_dir / "gst" / "ssv-pub",
        context.build_dir / "gst" / "ssv-overlay",
    ]
    environment["GST_PLUGIN_PATH"] = _join_unique(
        ":".join(str(path) for path in plugin_paths),
        context.environment.get("GST_PLUGIN_PATH", ""),
    )
    environment["LD_LIBRARY_PATH"] = _join_unique(
        str(context.build_dir / "gst" / "ssv-common"),
        snapshot.get("SSV_DEPS_RUNTIME_PATH", ""),
        context.environment.get("LD_LIBRARY_PATH", ""),
    )
    return environment
