"""Agent 进程共享的路径、配置发现和运行环境值。"""

from __future__ import annotations

import json
import os
from pathlib import Path

from ssv_agent.config import SsvConfig

_AGENT_ROOT = Path(__file__).resolve().parents[2]


def agent_root() -> Path:
    """返回 Agent 项目根目录。"""
    return _AGENT_ROOT


def resolve_agent_path(value: str | Path) -> Path:
    """把 Agent 配置或 CLI 路径解析到稳定的 Agent 项目目录。"""
    path = Path(value).expanduser()
    return path if path.is_absolute() else _AGENT_ROOT / path


def discover_config_path() -> Path | None:
    """按 Agent CLI 的约定发现主配置文件。"""
    candidates: list[Path] = []
    if env_path := os.getenv("SSV_CONFIG_PATH"):
        candidates.append(Path(env_path))
    candidates.extend(
        (
            Path("ssv.yaml"),
            Path("config/ssv.yaml"),
            _AGENT_ROOT.parent / "config" / "ssv.yaml",
        )
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def embedding_settings(config: SsvConfig) -> tuple[str, str | None]:
    """返回搜索、入库和事件索引共同使用的 embedding 设置。"""
    indexing = config.agent.indexing
    return indexing.embedding_backend, indexing.embedding_model


def runtime_environment(config: SsvConfig) -> dict[str, str | None]:
    """将主配置转换为 Agent 运行时组件读取的环境值。"""
    backend, model = embedding_settings(config)
    indexing = config.agent.indexing
    knowledge = config.agent.knowledge
    return {
        "SSV_EMBEDDING_BACKEND": backend,
        "SSV_EMBEDDING_MODEL": model,
        "SSV_EMBEDDING_BASE_URL": indexing.embedding_base_url,
        "SSV_EMBEDDING_QUERY_TEXT_TYPE": indexing.query_text_type,
        "SSV_EVENT_DB_PATH": str(resolve_agent_path(config.agent.event_db_path).resolve()),
        "SSV_OUTPUTS_DIR": str(resolve_agent_path(config.agent.output_dir).resolve()),
        "SSV_KNOWLEDGE_BACKEND": knowledge.backend,
        "SSV_KNOWLEDGE_RULES_DIR": str(resolve_agent_path(knowledge.rules_dir).resolve()),
        "SSV_QDRANT_PATH": str(resolve_agent_path(knowledge.qdrant_path).resolve()),
        "SSV_QDRANT_URL": knowledge.qdrant_url,
        "SSV_KNOWLEDGE_MIN_SCORE": str(knowledge.min_score),
        "SSV_EVIDENCE_ROOTS": json.dumps(config.agent.evidence_roots),
    }


def select_qdrant_target(
    config: SsvConfig | None,
    requested: Path | None,
) -> tuple[Path, str | None]:
    """选择互斥的 Qdrant 本地路径或服务 URL。"""
    if requested is not None:
        return requested, None
    if config is not None:
        knowledge = config.agent.knowledge
        return Path(knowledge.qdrant_path), knowledge.qdrant_url
    return Path("data/qdrant"), None
