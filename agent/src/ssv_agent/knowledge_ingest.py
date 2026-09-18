"""Build the Qdrant rule index from the configured Agent rule catalog."""

from __future__ import annotations

import argparse
import asyncio
import os
from pathlib import Path

from dotenv import load_dotenv

from ssv_agent.knowledge.backends.qdrant import ingest_rules
from ssv_agent.config import load_config
from ssv_agent.runtime import (
    agent_root as _agent_root,
    discover_config_path as _discover_config,
    runtime_environment as _runtime_environment,
    resolve_agent_path as _resolve_agent_path,
    select_qdrant_target as _select_qdrant_target,
)


def _load_agent_dotenv() -> None:
    """加载当前目录、Agent 目录或仓库根目录的本地环境配置。"""
    for candidate in (
        Path.cwd() / ".env",
        _agent_root() / ".env",
        _agent_root().parent / ".env",
    ):
        if candidate.is_file():
            load_dotenv(candidate)
            return


def main() -> None:
    _load_agent_dotenv()
    parser = argparse.ArgumentParser(description="Ingest local rules into Qdrant")
    parser.add_argument("--config", type=Path, default=None, help="Agent YAML 配置路径")
    parser.add_argument("--knowledge-dir", type=Path, default=None)
    parser.add_argument(
        "--embedding-backend",
        choices=("mock", "openai_compatible", "bge_m3"),
        default=None,
        help="覆盖配置中的 embedding backend",
    )
    parser.add_argument("--embedding-model", default=None, help="覆盖配置中的 embedding model")
    parser.add_argument("--qdrant-path", type=Path, default=None, help="覆盖 Qdrant 本地路径")
    args = parser.parse_args()

    config_path = args.config or _discover_config()
    config = load_config(config_path) if config_path else None
    configured_environment = _runtime_environment(config) if config is not None else {}
    backend = args.embedding_backend or (
        configured_environment.get("SSV_EMBEDDING_BACKEND")
        or os.getenv("SSV_EMBEDDING_BACKEND", "mock")
    )
    model = args.embedding_model
    if model is None and config is not None and args.embedding_backend is None:
        model = configured_environment.get("SSV_EMBEDDING_MODEL")
    if model is None and config is None:
        model = os.getenv("SSV_EMBEDDING_MODEL") or None
    os.environ["SSV_EMBEDDING_BACKEND"] = backend
    if model is None:
        os.environ.pop("SSV_EMBEDDING_MODEL", None)
    else:
        os.environ["SSV_EMBEDDING_MODEL"] = model
    embedding_base_url = configured_environment.get("SSV_EMBEDDING_BASE_URL")
    if embedding_base_url is None:
        os.environ.pop("SSV_EMBEDDING_BASE_URL", None)
    else:
        os.environ["SSV_EMBEDDING_BASE_URL"] = embedding_base_url
    query_text_type = configured_environment.get("SSV_EMBEDDING_QUERY_TEXT_TYPE")
    if query_text_type is None:
        os.environ.pop("SSV_EMBEDDING_QUERY_TEXT_TYPE", None)
    else:
        os.environ["SSV_EMBEDDING_QUERY_TEXT_TYPE"] = query_text_type
    rules_dir = configured_environment.get("SSV_KNOWLEDGE_RULES_DIR")
    if rules_dir is None:
        os.environ.pop("SSV_KNOWLEDGE_RULES_DIR", None)
    else:
        os.environ["SSV_KNOWLEDGE_RULES_DIR"] = rules_dir
    qdrant_path, qdrant_url = _select_qdrant_target(config, args.qdrant_path)
    os.environ["SSV_QDRANT_PATH"] = str(_resolve_agent_path(qdrant_path).resolve())
    if qdrant_url is None:
        os.environ.pop("SSV_QDRANT_URL", None)
    else:
        os.environ["SSV_QDRANT_URL"] = qdrant_url

    knowledge_dir = (
        _resolve_agent_path(args.knowledge_dir)
        if args.knowledge_dir is not None
        else None
    )
    result = asyncio.run(ingest_rules(knowledge_dir) if knowledge_dir else ingest_rules())
    if not result.success:
        raise SystemExit(result.error_message or "rule ingestion failed")
    print(
        f"ingested rule chunks={result.chunks_count} source={result.document_id} "
        f"backend={backend} qdrant={qdrant_url or os.environ['SSV_QDRANT_PATH']}"
    )


if __name__ == "__main__":
    main()
