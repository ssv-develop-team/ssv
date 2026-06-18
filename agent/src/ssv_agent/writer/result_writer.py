"""结果回写器 —— 将 ReviewResult 输出到 Redis 和结构化日志。"""

from __future__ import annotations

import json
from typing import Optional

import structlog
from redis import Redis

from ssv_agent.models.event import EventState, ReviewResult

logger = structlog.get_logger()


class ResultWriter:
    """复核结果回写器。

    回写目标:
    1. Redis Streams (ssv:review-results) —— 供下游消费
    2. 结构化日志 —— 供调试和审计

    用法:
        writer = ResultWriter(redis_client, stream_key="ssv:review-results")
        writer.write(result)
    """

    def __init__(
        self,
        redis_client: Optional[Redis] = None,
        stream_key: str = "ssv:review-results",
    ) -> None:
        self._redis = redis_client
        self._stream = stream_key
        self._write_count = 0

    @property
    def write_count(self) -> int:
        return self._write_count

    def write(self, result: ReviewResult) -> None:
        """回写复核结果。"""
        self._write_count += 1

        # 1. 结构化日志（始终输出）
        logger.info(
            "review result",
            event_id=result.event_id,
            source=result.source,
            frame_id=result.frame_id,
            state=result.final_state.value,
            strategy=result.strategy.value,
            conclusion=result.conclusion[:120],
            severity=result.severity.value,
            detections=result.detections_count,
            tools_used=len(result.tool_results),
        )

        # 2. Redis Streams（如果可用）
        if self._redis:
            try:
                payload = json.dumps(
                    result.model_dump(mode="json"),
                    ensure_ascii=False,
                )
                self._redis.xadd(self._stream, {"review": payload})
            except Exception as exc:
                logger.error("redis write failed", error=str(exc))

        # 3. 终态日志（FAILED/NEEDS_HUMAN 额外警告）
        if result.final_state != EventState.COMPLETED:
            logger.warning(
                "review ended with non-completed state",
                state=result.final_state.value,
                conclusion=result.conclusion,
            )
