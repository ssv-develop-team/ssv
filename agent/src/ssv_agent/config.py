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
    state_machine_timeout: int = 300   # 状态机单次复核超时（秒）
    max_retries: int = 3              # 失败重试次数
    mock_provider: bool = True        # 使用 mock provider（默认开启，后续接入真实模型时关闭）
    provider_type: str = "local_yolo" # mock_provider=false 时使用的 provider 类型
    review_result_stream: str = "ssv:review-results"  # 复核结果 Redis Stream key
    prompt_max_tokens: int = 4096     # 提示词 token 预算上限（字符数/2 估算）
    prompt_log_enabled: bool = False  # 提示词调试日志开关（mock 模式下默认关闭）
    local_yolo_model_path: str = "/home/lzy/work-code/comp-2-freeze10.pt"
    local_yolo_confidence_threshold: float = 0.25
    local_yolo_device: str = "cpu"
    openai_api_base_url: str = ""
    openai_api_key_env: str = "SSV_AGENT_API_KEY"
    openai_text_model: str = "qwen3.7-plus"
    openai_vision_model: str = ""
    openai_timeout_seconds: float = 60.0
    openai_temperature: float = 0.2
    openai_max_tokens: int = 2048

    # ── M4 上下文工程新增 ───────────────────────────────────────────────
    context_history_max_window: int = 50       # 历史记录最大窗口大小
    context_engine_debug: bool = False         # 上下文引擎 debug 模式（硬约束超限 raise）
    rules_yaml_path: str = "config/rules.yaml" # 安全规则 YAML 文件路径


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
    if v := os.environ.get("SSV_AGENT_MOCK_PROVIDER"):
        cfg.agent.mock_provider = v.lower() in ("1", "true", "yes", "on")
    if v := os.environ.get("SSV_AGENT_PROVIDER"):
        cfg.agent.provider_type = v
    if v := os.environ.get("SSV_AGENT_MODEL_PATH"):
        cfg.agent.local_yolo_model_path = v
    if v := os.environ.get("SSV_AGENT_API_BASE_URL"):
        cfg.agent.openai_api_base_url = v
    if v := os.environ.get("SSV_AGENT_API_KEY_ENV"):
        cfg.agent.openai_api_key_env = v
    if v := os.environ.get("SSV_AGENT_TEXT_MODEL"):
        cfg.agent.openai_text_model = v
    if v := os.environ.get("SSV_AGENT_VISION_MODEL"):
        cfg.agent.openai_vision_model = v
    if v := os.environ.get("SSV_AGENT_API_TIMEOUT_SECONDS"):
        cfg.agent.openai_timeout_seconds = float(v)
    if v := os.environ.get("SSV_AGENT_API_TEMPERATURE"):
        cfg.agent.openai_temperature = float(v)
    if v := os.environ.get("SSV_AGENT_API_MAX_TOKENS"):
        cfg.agent.openai_max_tokens = int(v)


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
