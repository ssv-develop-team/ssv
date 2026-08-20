"""Project path and environment discovery shared by all commands."""

from __future__ import annotations

import os
import re
import shlex
from dataclasses import dataclass
from pathlib import Path

from .output import CliError

_ENV_KEY = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def _parse_dotenv_value(value: str) -> str:
    value = value.strip()
    if not value:
        return ""
    if value[0] in {'"', "'"}:
        try:
            parsed = shlex.split(value, posix=True)
        except ValueError:
            return value
        if len(parsed) == 1:
            return parsed[0]
    return value


def load_dotenv(path: Path, environment: dict[str, str]) -> None:
    """Load simple KEY=VALUE entries without replacing process variables."""

    if not path.is_file():
        return
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        if "=" not in line:
            raise CliError(f"invalid .env entry at {path}:{line_number}")
        key, value = line.split("=", 1)
        key = key.strip()
        if not _ENV_KEY.fullmatch(key):
            raise CliError(f"invalid .env key at {path}:{line_number}: {key}")
        environment.setdefault(key, _parse_dotenv_value(value))


def _resolve(root: Path, value: str | Path) -> Path:
    path = Path(value).expanduser()
    return path if path.is_absolute() else root / path


@dataclass(frozen=True)
class ProjectContext:
    """Immutable paths and environment used by one CLI invocation."""

    root: Path
    environment: dict[str, str]
    config_path: Path | None
    build_dir: Path
    compose_file: Path

    @classmethod
    def discover(cls, root: Path | None = None) -> ProjectContext:
        project_root = (root or Path(__file__).resolve().parents[2]).resolve()
        environment = dict(os.environ)
        load_dotenv(project_root / ".env", environment)

        configured_path = environment.get("SSV_CONFIG_PATH")
        config_path: Path | None = None
        if configured_path:
            config_path = _resolve(project_root, configured_path)
        else:
            for candidate in (
                project_root / "ssv.yaml",
                project_root / "config" / "ssv.yaml",
                Path("/etc/ssv/ssv.yaml"),
            ):
                if candidate.is_file():
                    config_path = candidate
                    break

        build_dir = _resolve(project_root, environment.get("SSV_BUILD_DIR", "build"))
        return cls(
            root=project_root,
            environment=environment,
            config_path=config_path,
            build_dir=build_dir,
            compose_file=project_root / "docker" / "compose.yaml",
        )

    def resolve(self, value: str | Path) -> Path:
        return _resolve(self.root, value)

    def display_path(self, path: Path) -> str:
        try:
            return str(path.resolve().relative_to(self.root))
        except ValueError:
            return str(path)

    def child_environment(self, **updates: str) -> dict[str, str]:
        environment = dict(self.environment)
        environment.update({key: value for key, value in updates.items() if value is not None})
        return environment
