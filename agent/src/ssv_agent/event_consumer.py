"""Redis Streams 事件消费者 —— 纯 I/O 层，不包含业务逻辑。

职责:
  - 创建/确认 consumer group
  - 从 Redis Streams 拉取事件
  - ACK 已处理事件
  - 发布复核结果到独立 Stream

业务逻辑（解析、状态机、回写）由 service.py 负责。
"""

from __future__ import annotations

import structlog
from typing import Callable

from redis import Redis

from ssv_agent.config import SsvConfig

logger = structlog.get_logger()


class EventConsumer:
    """Redis Streams 消费者（纯 I/O）。

    用法:
        consumer = EventConsumer(config)
        consumer.start(handler_callback)
    """

    def __init__(self, config: SsvConfig) -> None:
        self._stream = config.redis.stream_key
        self._group = config.redis.consumer_group
        self._running = False
        self._redis = Redis(
            host=config.redis.host,
            port=config.redis.port,
            db=config.redis.db,
            decode_responses=True,
        )

    @property
    def redis_client(self) -> Redis:
        """获取底层 Redis 客户端（供 ResultWriter 复用）。"""
        return self._redis

    def _ensure_group(self) -> None:
        """创建 consumer group（如果不存在）。"""
        try:
            self._redis.xgroup_create(
                self._stream, self._group, id="0", mkstream=True
            )
            logger.info("created consumer group", group=self._group, stream=self._stream)
        except Exception:
            pass  # 已存在

    def start(self, handler: Callable[[str, dict[str, str]], None]) -> None:
        """启动阻塞式消费循环。

        Args:
            handler: 事件处理回调，签名为 (msg_id, fields) -> None。
                     业务逻辑由回调负责，consumer 只负责拉取和 ACK。
        """
        self._running = True
        self._ensure_group()
        consumer_name = "ssv-agent-1"

        logger.info(
            "event consumer started",
            stream=self._stream,
            group=self._group,
            consumer=consumer_name,
        )

        while self._running:
            try:
                entries = self._redis.xreadgroup(
                    self._group,
                    consumer_name,
                    {self._stream: ">"},
                    count=10,
                    block=1000,
                )
            except Exception as exc:
                logger.warning("redis read error", error=str(exc))
                continue

            for _stream_name, messages in entries:
                for msg_id, fields in messages:
                    try:
                        handler(msg_id, fields)
                    except Exception as exc:
                        logger.error(
                            "event handler error", msg_id=msg_id, error=str(exc)
                        )

    def stop(self) -> None:
        """停止消费循环。"""
        self._running = False

    def ack(self, msg_id: str) -> None:
        """确认已处理事件。"""
        self._redis.xack(self._stream, self._group, msg_id)
