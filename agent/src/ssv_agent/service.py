from __future__ import annotations

import structlog

from ssv_agent.config import SsvConfig
from ssv_agent.event_consumer import run_consumer

logger = structlog.get_logger()


def run(config: SsvConfig) -> None:
    """Main agent service entry point.

    M2: starts the Redis Streams event consumer (consume + print).
    M6 will add LLM state machine orchestration here.
    """
    logger.info(
        "agent service starting",
        version=config.version,
        redis=f"{config.redis.host}:{config.redis.port}",
        stream=config.redis.stream_key,
    )

    run_consumer(config)

    logger.info("agent service stopped")
