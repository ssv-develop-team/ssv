from __future__ import annotations

import os
from pathlib import Path

import yaml
from pydantic import BaseModel


class LoggingConfig(BaseModel):
    cpp_debug_level: str = "ssv*:4"
    python_log_level: str = "INFO"


class RedisConfig(BaseModel):
    host: str = "localhost"
    port: int = 6379
    db: int = 0
    stream_key: str = "ssv:events"
    consumer_group: str = "ssv-agent"


class DisplayConfig(BaseModel):
    enabled: bool = False
    sink: str = "autovideosink"


class PipelineConfig(BaseModel):
    analysis_fps: int = 5
    frame_width: int = 640
    frame_height: int = 480


class InferenceConfig(BaseModel):
    model_path: str = ""
    confidence_threshold: float = 0.5
    device: str = "cpu"
    target_class: str = "person"


class TrackingConfig(BaseModel):
    enabled: bool = True
    frame_rate: int = 30
    track_threshold: float = 0.5
    track_buffer: int = 30
    match_threshold: float = 0.3
    mock_track: bool = False


class AgentConfig(BaseModel):
    state_machine_timeout: int = 300
    max_retries: int = 3


class SsvConfig(BaseModel):
    version: str = "1.0"
    logging: LoggingConfig = LoggingConfig()
    redis: RedisConfig = RedisConfig()
    display: DisplayConfig = DisplayConfig()
    pipeline: PipelineConfig = PipelineConfig()
    inference: InferenceConfig = InferenceConfig()
    tracking: TrackingConfig = TrackingConfig()
    agent: AgentConfig = AgentConfig()
    sources: list[dict] = []


def _apply_env_overrides(cfg: SsvConfig) -> None:
    """Override selected config fields from environment variables."""
    if v := os.environ.get("REDIS_HOST"):
        cfg.redis.host = v
    if v := os.environ.get("REDIS_PORT"):
        cfg.redis.port = int(v)
    if v := os.environ.get("SSV_LOG_LEVEL"):
        cfg.logging.python_log_level = v
    if v := os.environ.get("SSV_DISPLAY_SINK"):
        cfg.display.sink = v


def load_config(path: str | Path | None = None) -> SsvConfig:
    """Load configuration from YAML file.

    Search order: explicit path -> SSV_CONFIG_PATH env -> config/ssv.default.yaml -> defaults.
    Environment variables (REDIS_HOST, REDIS_PORT, SSV_LOG_LEVEL, SSV_DISPLAY_SINK)
    override corresponding YAML values.
    """
    cfg: SsvConfig | None = None

    if path is not None:
        p = Path(path)
        if p.exists():
            with open(p) as f:
                data = yaml.safe_load(f) or {}
            cfg = SsvConfig.model_validate(data)
        else:
            raise FileNotFoundError(f"Config file not found: {p}")

    if cfg is None:
        env_path = os.environ.get("SSV_CONFIG_PATH")
        if env_path and Path(env_path).exists():
            with open(env_path) as f:
                data = yaml.safe_load(f) or {}
            cfg = SsvConfig.model_validate(data)

    if cfg is None:
        relative = Path("config/ssv.default.yaml")
        if relative.exists():
            with open(relative) as f:
                data = yaml.safe_load(f) or {}
            cfg = SsvConfig.model_validate(data)

    if cfg is None:
        cfg = SsvConfig()

    _apply_env_overrides(cfg)
    return cfg
