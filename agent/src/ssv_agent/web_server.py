"""只读 Web API 的独立 Uvicorn 入口。"""

from __future__ import annotations

import uvicorn

from ssv_agent.config import SsvConfig
from ssv_agent.web_api import create_app


def run_web(config: SsvConfig) -> None:
    """按 Agent 配置启动 Web API，不创建消费器或后台 worker。"""
    uvicorn.run(
        create_app(config),
        host=config.agent.web.host,
        port=config.agent.web.port,
        log_level=config.logging.python_log_level.lower(),
    )
