from __future__ import annotations

import argparse
import os
import sys

from dotenv import dotenv_values

from ssv_agent.config import SsvConfig, load_config
from ssv_agent.logging import setup_logging
from ssv_agent.runtime import agent_root

_MINIO_ROOT_KEYS = frozenset({"MINIO_ROOT_USER", "MINIO_ROOT_PASSWORD"})
_DOCKER_PORT_KEYS = frozenset({"REDIS_PORT", "QDRANT_PORT", "MINIO_API_PORT", "MINIO_CONSOLE_PORT"})


def _load_environment() -> None:
    """与统一 CLI 使用相同的根配置和端口；Agent 仅保留应用凭据。"""
    root = agent_root().parent
    for key in _MINIO_ROOT_KEYS:
        os.environ.pop(key, None)
    for path, allowed_keys in (
        (root / ".env", None),
        (root / "docker" / ".env", _DOCKER_PORT_KEYS),
        (agent_root() / ".env", None),
    ):
        for key, value in dotenv_values(path, interpolate=False).items():
            if key in _MINIO_ROOT_KEYS or value is None:
                continue
            if allowed_keys is None or key in allowed_keys:
                os.environ.setdefault(key, value)


def _retry_artifact(config: SsvConfig, artifact_id: str) -> int:
    """仅重置 dead 上传任务，不创建产物、不调用模型。"""
    from ssv_agent.event_store import EventLedger
    from ssv_agent.runtime import resolve_agent_path

    with EventLedger(
        resolve_agent_path(config.agent.event_db_path),
        evidence_roots=config.agent.evidence_roots,
        artifact_storage=config.agent.storage,
    ) as ledger:
        try:
            job = ledger.retry_dead_artifact_upload(artifact_id)
        except (KeyError, ValueError) as exc:
            print(f"artifact retry rejected: {exc}", file=sys.stderr)
            raise SystemExit(2) from exc
    print(f"artifact {artifact_id} -> {job.state.value}")
    return 0


def run(config: SsvConfig) -> None:
    """按需启动现有 Agent 服务，避免 Web 命令加载 worker 运行栈。"""
    from ssv_agent.service import run as run_agent

    run_agent(config)


def main() -> None:
    _load_environment()

    parser = argparse.ArgumentParser(description="Site Safety Vision agent service")
    parser.add_argument(
        "--config",
        type=str,
        default=None,
        help="Path to YAML config file (default: SSV_CONFIG_PATH, ssv.yaml, or config/ssv.yaml)",
    )
    parser.add_argument(
        "--log-level",
        type=str,
        default=None,
        help="Override log level (DEBUG, INFO, WARNING, ERROR)",
    )
    subparsers = parser.add_subparsers(dest="command")
    web_parser = subparsers.add_parser("web", help="Run the read-only Web API")
    web_parser.add_argument(
        "--config",
        type=str,
        default=argparse.SUPPRESS,
        help="Path to YAML config file",
    )
    web_parser.add_argument(
        "--log-level",
        type=str,
        default=argparse.SUPPRESS,
        help="Override log level (DEBUG, INFO, WARNING, ERROR)",
    )
    artifacts_parser = subparsers.add_parser(
        "artifacts", help="Manage registered artifact upload jobs"
    )
    artifacts_subparsers = artifacts_parser.add_subparsers(
        dest="artifacts_command", required=True
    )
    retry_parser = artifacts_subparsers.add_parser(
        "retry", help="Requeue a dead artifact upload job"
    )
    retry_parser.add_argument("--artifact-id", required=True)
    args = parser.parse_args()

    config_path = args.config or os.environ.get("SSV_CONFIG_PATH")
    cfg = load_config(config_path)

    log_level = args.log_level or cfg.logging.python_log_level
    setup_logging(log_level)

    if args.command == "web":
        from ssv_agent.web_server import run_web

        run_web(cfg)
        return

    if args.command == "artifacts":
        if args.artifacts_command == "retry":
            _retry_artifact(cfg, args.artifact_id)
        return

    run(cfg)
