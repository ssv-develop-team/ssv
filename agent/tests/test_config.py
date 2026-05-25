from __future__ import annotations

from pathlib import Path

import pytest

from ssv_agent.config import load_config


def test_load_config_uses_yaml_values(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("SSV_CONFIG_PATH", raising=False)
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
    monkeypatch.setenv("SSV_LOG_LEVEL", "DEBUG")
    monkeypatch.setenv("SSV_DISPLAY_SINK", "fakesink")

    cfg = load_config(path)

    assert cfg.redis.host == "env-host"
    assert cfg.redis.port == 2222
    assert cfg.logging.python_log_level == "DEBUG"
    assert cfg.display.sink == "fakesink"


def test_load_config_missing_explicit_path_raises(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        load_config(tmp_path / "missing.yaml")
