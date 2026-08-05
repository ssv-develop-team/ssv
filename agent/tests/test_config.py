from __future__ import annotations

import sys
from pathlib import Path

import pytest
from pydantic import ValidationError

from ssv_agent import cli
from ssv_agent.config import SsvConfig, load_config


def test_load_config_uses_yaml_values(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("SSV_CONFIG_PATH", raising=False)
    monkeypatch.delenv("REDIS_HOST", raising=False)
    monkeypatch.delenv("REDIS_PORT", raising=False)
    path = tmp_path / "ssv.yaml"
    path.write_text(
        """
version: "2.0"
logging:
  python_log_level: "DEBUG"
redis:
  host: "redis.local"
  port: 6380
  stream_key: "custom:events"
agent:
  state_machine_timeout: 120
  max_retries: 5
""".strip(),
        encoding="utf-8",
    )

    cfg = load_config(path)

    assert cfg.version == "2.0"
    assert cfg.logging.python_log_level == "DEBUG"
    assert cfg.redis.host == "redis.local"
    assert cfg.redis.port == 6380
    assert cfg.redis.stream_key == "custom:events"
    assert cfg.agent.state_machine_timeout == 120
    assert cfg.agent.max_retries == 5


def test_load_config_applies_environment_overrides(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = tmp_path / "ssv.yaml"
    path.write_text(
        'version: "2.0"\nredis:\n  host: yaml-host\n  port: 1111\n',
        encoding="utf-8",
    )
    monkeypatch.setenv("REDIS_HOST", "env-host")
    monkeypatch.setenv("REDIS_PORT", "2222")

    cfg = load_config(path)

    assert cfg.redis.host == "env-host"
    assert cfg.redis.port == 2222


def test_load_config_validates_environment_overrides(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = tmp_path / "ssv.yaml"
    path.write_text('version: "2.0"\n', encoding="utf-8")
    monkeypatch.setenv("REDIS_PORT", "0")

    with pytest.raises(ValidationError):
        load_config(path)


def test_load_config_uses_config_directory_yaml(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("SSV_CONFIG_PATH", raising=False)
    monkeypatch.chdir(tmp_path)
    config_dir = tmp_path / "config"
    config_dir.mkdir()
    (config_dir / "ssv.yaml").write_text(
        'version: "2.0"\nredis:\n  host: config-dir-host\n',
        encoding="utf-8",
    )

    cfg = load_config()

    assert cfg.redis.host == "config-dir-host"


def test_load_config_rejects_v1(tmp_path: Path) -> None:
    path = tmp_path / "ssv.yaml"
    path.write_text('version: "1.0"\nredis:\n  host: redis.local\n', encoding="utf-8")

    with pytest.raises(ValidationError):
        load_config(path)


@pytest.mark.parametrize(
    "yaml_text",
    [
        "redis:\n  host: redis.local",
        "version: 2.0",
        'version: "2.0"\npipeline: {}',
        'version: "2.0"\npipline: {}',
        'version: "2.0"\nredis:\n  porr: 6380',
        'version: "2.0"\nredis:\n  port: "6380"',
    ],
)
def test_load_config_rejects_non_contract_yaml(tmp_path: Path, yaml_text: str) -> None:
    path = tmp_path / "ssv.yaml"
    path.write_text(yaml_text + "\n", encoding="utf-8")

    with pytest.raises(ValidationError):
        load_config(path)


@pytest.mark.parametrize(
    "fragment",
    [
        "redis:\n  port: 0",
        "redis:\n  port: 65536",
        "redis:\n  db: -1",
        "agent:\n  state_machine_timeout: 0",
        "agent:\n  max_retries: -1",
    ],
)
def test_load_config_rejects_invalid_agent_owned_ranges(tmp_path: Path, fragment: str) -> None:
    path = tmp_path / "ssv.yaml"
    path.write_text(f'version: "2.0"\n{fragment}\n', encoding="utf-8")

    with pytest.raises(ValidationError):
        load_config(path)


def test_load_config_accepts_complete_example(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.delenv("REDIS_HOST", raising=False)
    monkeypatch.delenv("REDIS_PORT", raising=False)
    path = Path(__file__).resolve().parents[2] / "config" / "ssv.example.yaml"

    cfg = load_config(path)

    assert cfg.version == "2.0"
    assert cfg.redis.stream_key == "ssv:events"
    assert cfg.agent.max_retries == 3


def test_load_config_missing_explicit_path_raises(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        load_config(tmp_path / "missing.yaml")


def test_load_config_does_not_use_example_as_default(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("SSV_CONFIG_PATH", raising=False)
    monkeypatch.delenv("REDIS_HOST", raising=False)
    monkeypatch.delenv("REDIS_PORT", raising=False)
    monkeypatch.chdir(tmp_path)
    config_dir = tmp_path / "config"
    config_dir.mkdir()
    (config_dir / "ssv.example.yaml").write_text(
        "redis:\n  host: example-only-host\n",
        encoding="utf-8",
    )

    cfg = load_config()

    assert cfg.redis.host == "localhost"


def test_cli_does_not_pass_example_as_default_config(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    observed_paths: list[str | Path | None] = []
    monkeypatch.delenv("SSV_CONFIG_PATH", raising=False)
    monkeypatch.setattr(sys, "argv", ["ssv-agent"])
    monkeypatch.setattr(cli, "find_dotenv", lambda: "")
    monkeypatch.setattr(cli, "load_dotenv", lambda _: False)
    monkeypatch.setattr(
        cli,
        "load_config",
        lambda path=None: observed_paths.append(path) or SsvConfig(),
    )
    monkeypatch.setattr(cli, "setup_logging", lambda _: None)
    monkeypatch.setattr(cli, "run", lambda _: None)

    cli.main()

    assert observed_paths == [None]
