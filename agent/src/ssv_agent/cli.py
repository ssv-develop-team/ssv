from __future__ import annotations

import argparse
import os

from dotenv import find_dotenv, load_dotenv

from ssv_agent.config import SsvConfig, load_config
from ssv_agent.logging import setup_logging


def run(config: SsvConfig) -> None:
    """按需启动现有 Agent 服务，避免 Web 命令加载 worker 运行栈。"""
    from ssv_agent.service import run as run_agent

    run_agent(config)


def main() -> None:
    load_dotenv(find_dotenv())

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
    args = parser.parse_args()

    config_path = args.config or os.environ.get("SSV_CONFIG_PATH")
    cfg = load_config(config_path)

    log_level = args.log_level or cfg.logging.python_log_level
    setup_logging(log_level)

    if args.command == "web":
        from ssv_agent.web_server import run_web

        run_web(cfg)
        return

    run(cfg)
