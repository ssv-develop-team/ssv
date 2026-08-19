"""事件冷却去重：同一视频源同一跟踪目标在冷却窗口内只触发一次复核。

去重状态放在 Redis（SET NX PX），跨消费者进程共享冷却状态；Redis 异常时
fail-open，宁可多触发一次复核也不静默丢事件。
"""

from __future__ import annotations

import hashlib
from enum import Enum

import structlog
from redis import Redis
from redis.exceptions import RedisError

from ssv_agent.review_context import ReviewContext

logger = structlog.get_logger()

DEFAULT_KEY_PREFIX = "ssv:agent:dedup"


class DedupDecision(str, Enum):
    """一次去重决策结果。"""

    RUN = "run"
    SKIP = "skip"
    FAIL_OPEN = "fail_open"


def _track_key(prefix: str, source: str, track_id: int) -> str:
    """由 source+track 生成稳定且无歧义的 Redis 键。"""
    digest = hashlib.sha256(f"{source}\0{track_id}".encode("utf-8")).hexdigest()[:32]
    return f"{prefix}:{digest}"


def _event_token(context: ReviewContext) -> str:
    """返回同一 ingress 重投时稳定的冷却键值。"""
    ingress_id = context.ingress_id or context.event_id
    value = f"{context.event_id}\0{ingress_id}".encode("utf-8")
    return hashlib.sha256(value).hexdigest()


class EventDeduper:
    """按 source+track 的冷却去重器。

    事件包含的每个 track 对应一个冷却键：只要存在尚未冷却的 track，事件就需要
    复核，并把本次事件的所有 track 标记为已冷却；全部 track 都处于冷却期时判定
    为重复。track_id 缺失（-1）时退化为按 source 冷却。
    """

    def __init__(
        self,
        redis: Redis,
        *,
        key_prefix: str = DEFAULT_KEY_PREFIX,
        cooldown_seconds: float = 30.0,
    ) -> None:
        if cooldown_seconds <= 0:
            raise ValueError("cooldown_seconds must be positive")
        self._redis = redis
        self._key_prefix = key_prefix
        self._cooldown_ms = int(cooldown_seconds * 1000)

    def decide(self, context: ReviewContext) -> DedupDecision:
        """判断事件是否需要复核，同时刷新本次事件的冷却状态。"""
        track_ids = {detection.track_id for detection in context.detections}
        if not track_ids:
            return DedupDecision.RUN

        has_new = False
        is_redelivery = False
        token = _event_token(context)
        try:
            for track_id in track_ids:
                key = _track_key(self._key_prefix, context.source, track_id)
                acquired = bool(
                    self._redis.set(key, token, nx=True, px=self._cooldown_ms)
                )
                has_new = has_new or acquired
                if not acquired and self._redis.get(key) == token:
                    is_redelivery = True
        except RedisError as exc:
            logger.warning(
                "dedup check failed, failing open",
                source=context.source,
                error=str(exc),
            )
            return DedupDecision.FAIL_OPEN
        return DedupDecision.RUN if has_new or is_redelivery else DedupDecision.SKIP
