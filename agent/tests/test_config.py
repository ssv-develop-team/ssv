from __future__ import annotations

from pathlib import Path

import pytest

from ssv_agent.config import load_config


def test_load_config_uses_yaml_values(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("SSV_CONFIG_PATH", raising=False)
    monkeypatch.delenv("REDIS_HOST", raising=False)
    monkeypatch.delenv("REDIS_PORT", raising=False)
    monkeypatch.delenv("SSV_LOG_LEVEL", raising=False)
    monkeypatch.delenv("SSV_DISPLAY_SINK", raising=False)
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
agent:
  provider_type: "local_yolo"
  local_yolo_model_path: "/tmp/model.pt"
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
    assert cfg.agent.provider_type == "local_yolo"
    assert cfg.agent.local_yolo_model_path == "/tmp/model.pt"


def test_load_config_applies_environment_overrides(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = tmp_path / "ssv.yaml"
    path.write_text("redis:\n  host: yaml-host\n  port: 1111\n", encoding="utf-8")
    monkeypatch.setenv("REDIS_HOST", "env-host")
    monkeypatch.setenv("REDIS_PORT", "2222")
    monkeypatch.setenv("SSV_LOG_LEVEL", "DEBUG")
    monkeypatch.setenv("SSV_DISPLAY_SINK", "fakesink")
    monkeypatch.setenv("SSV_AGENT_PROVIDER", "openai_compatible")
    monkeypatch.setenv("SSV_AGENT_MODEL_PATH", "/tmp/helmet.pt")
    monkeypatch.setenv(
        "SSV_AGENT_API_BASE_URL",
        "https://workspace.cn-beijing.maas.aliyuncs.com/compatible-mode/v1",
    )
    monkeypatch.setenv("SSV_AGENT_API_KEY_ENV", "DASHSCOPE_API_KEY")
    monkeypatch.setenv("SSV_AGENT_TEXT_MODEL", "qwen3.7-plus")
    monkeypatch.setenv("SSV_AGENT_VISION_MODEL", "qwen-vl-plus")
    monkeypatch.setenv("SSV_AGENT_API_TIMEOUT_SECONDS", "45")
    monkeypatch.setenv("SSV_AGENT_API_TEMPERATURE", "0.1")
    monkeypatch.setenv("SSV_AGENT_API_MAX_TOKENS", "1024")

    cfg = load_config(path)

    assert cfg.redis.host == "env-host"
    assert cfg.redis.port == 2222
    assert cfg.logging.python_log_level == "DEBUG"
    assert cfg.display.sink == "fakesink"
    assert cfg.agent.provider_type == "openai_compatible"
    assert cfg.agent.local_yolo_model_path == "/tmp/helmet.pt"
    assert cfg.agent.openai_api_base_url.endswith("/compatible-mode/v1")
    assert cfg.agent.openai_api_key_env == "DASHSCOPE_API_KEY"
    assert cfg.agent.openai_text_model == "qwen3.7-plus"
    assert cfg.agent.openai_vision_model == "qwen-vl-plus"
    assert cfg.agent.openai_timeout_seconds == 45
    assert cfg.agent.openai_temperature == 0.1
    assert cfg.agent.openai_max_tokens == 1024


def test_load_config_missing_explicit_path_raises(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        load_config(tmp_path / "missing.yaml")
