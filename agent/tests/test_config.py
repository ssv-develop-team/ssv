from __future__ import annotations

from pathlib import Path

import pytest

from ssv_agent.config import load_config


def test_load_config_uses_yaml_values(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("SSV_CONFIG_PATH", raising=False)
    monkeypatch.delenv("REDIS_HOST", raising=False)
    monkeypatch.delenv("REDIS_PORT", raising=False)
    path = tmp_path / "ssv.yaml"
    path.write_text(
        """
version: "9.9"
redis:
  host: "redis.local"
  port: 6380
  stream_key: "custom:events"
pipeline:
  analysis_fps: 7
inference:
  target_class: "helmet"
""".strip(),
        encoding="utf-8",
    )

    cfg = load_config(path)

    assert cfg.version == "9.9"
    assert cfg.redis.host == "redis.local"
    assert cfg.redis.port == 6380
    assert cfg.redis.stream_key == "custom:events"
    assert cfg.pipeline.analysis_fps == 7
    assert cfg.inference.target_class == "helmet"


def test_load_config_applies_environment_overrides(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = tmp_path / "ssv.yaml"
    path.write_text("redis:\n  host: yaml-host\n  port: 1111\n", encoding="utf-8")
    monkeypatch.setenv("REDIS_HOST", "env-host")
    monkeypatch.setenv("REDIS_PORT", "2222")

    cfg = load_config(path)

    assert cfg.redis.host == "env-host"
    assert cfg.redis.port == 2222


def test_load_config_uses_config_directory_yaml(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("SSV_CONFIG_PATH", raising=False)
    monkeypatch.chdir(tmp_path)
    config_dir = tmp_path / "config"
    config_dir.mkdir()
    (config_dir / "ssv.yaml").write_text(
        "redis:\n  host: config-dir-host\npipeline:\n  analysis_fps: 0\n",
        encoding="utf-8",
    )

    cfg = load_config()

    assert cfg.redis.host == "config-dir-host"
    assert cfg.pipeline.analysis_fps == 0


def test_load_config_missing_explicit_path_raises(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        load_config(tmp_path / "missing.yaml")
