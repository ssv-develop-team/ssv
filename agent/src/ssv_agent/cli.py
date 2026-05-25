from __future__ import annotations

import argparse
import os
from pathlib import Path

from dotenv import find_dotenv, load_dotenv

from ssv_agent.config import load_config
from ssv_agent.logging import setup_logging
from ssv_agent.service import run


def main() -> None:
    load_dotenv(find_dotenv())

    parser = argparse.ArgumentParser(description="Site Safety Vision agent service")
    parser.add_argument(
        "--config",
        type=str,
        default=None,
        help="Path to YAML config file (default: SSV_CONFIG_PATH or ../config/ssv.default.yaml)",
    )
    parser.add_argument(
        "--log-level",
        type=str,
        default=None,
        help="Override log level (DEBUG, INFO, WARNING, ERROR)",
    )
    args = parser.parse_args()

    config_path = args.config or os.environ.get("SSV_CONFIG_PATH")
    if config_path is None:
        # Default: look relative to agent package
        candidate = Path(__file__).resolve().parent.parent.parent.parent / "config" / "ssv.default.yaml"
        if candidate.exists():
            config_path = str(candidate)

    cfg = load_config(config_path)

    log_level = args.log_level or cfg.logging.python_log_level
    setup_logging(log_level)

    run(cfg)
