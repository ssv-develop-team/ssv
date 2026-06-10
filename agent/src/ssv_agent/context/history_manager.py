"""历史记录管理器 —— 记录 + 滑动窗口查询 + 压缩策略。

设计原则:
  1. M4 实现 Level 0（全量保留），Level 1-3 接口预留
  2. 空历史 → 返回空 HistoryBlock，组装管线静默跳过
  3. 关联度驱动的优先级驱逐（而非简单 FIFO）
"""

from __future__ import annotations

from typing import Optional

import structlog

from ssv_agent.context.pack import (
    CompressionRecord,
    HistoryBlock,
    HistoryMessage,
)

logger = structlog.get_logger()

# 关联度因子权重
WEIGHT_TRACK_OVERLAP = 0.50
WEIGHT_TIME_PROXIMITY = 0.25
WEIGHT_SEVERITY = 0.15
WEIGHT_SAME_TRIGGER = 0.10


class HistoryManager:
    """历史记录管理器 —— 写入、窗口查询、压缩。

    当前 M4 阶段: 内存 dict 存储，Level 0 全量保留。
    M5 激活: Level 1-3 压缩 + Redis 持久化。

    用法:
        mgr = HistoryManager(max_window_size=50)
        mgr.record(event_id="...", track_ids=[1,2], ...)
        block = mgr.get_window(current_event)
    """

    def __init__(self, max_window_size: int = 50) -> None:
        self._records: dict[str, HistoryMessage] = {}
        self._max_window = max_window_size
        self._record_count = 0

    @property
    def record_count(self) -> int:
        return len(self._records)

    # ── 公共接口 ─────────────────────────────────────────────────────────

    def record(
        self,
        event_id: str,
        track_ids: list[int] | None = None,
        timestamp_ms: int = 0,
        severity: str = "low",
        trigger_reason: str = "",
        strategy: str = "",
        conclusion_summary: str = "",
    ) -> None:
        """记录一条复核完成的历史。

        Args:
            event_id: 事件唯一标识。
            track_ids: 涉及的目标 track_id 列表（-1 表示未跟踪）。
            timestamp_ms: 事件时间戳（毫秒）。
            severity: 严重程度。
            trigger_reason: 触发原因。
            strategy: 使用的复核策略。
            conclusion_summary: 压缩后的结论摘要。
        """
        msg = HistoryMessage(
            event_id=event_id,
            track_ids=track_ids or [],
            timestamp_ms=timestamp_ms,
            severity=severity,
            trigger_reason=trigger_reason,
            strategy=strategy,
            conclusion_summary=conclusion_summary,
        )
        self._records[event_id] = msg
        self._record_count += 1

        logger.debug(
            "history recorded",
            event_id=event_id,
            total_records=len(self._records),
        )

    def get_window(
        self,
        current_event,
        max_messages: Optional[int] = None,
    ) -> HistoryBlock:
        """获取与当前事件相关的历史窗口。

        Args:
            current_event: 当前 DetectionEvent。
            max_messages: 返回的最大消息数（None 则用默认值）。

        Returns:
            HistoryBlock: 含排序后的历史消息和压缩元数据。
        """
        max_n = max_messages or self._max_window
        if not self._records:
            return HistoryBlock()

        # 计算每条历史记录与当前事件的关联度
        scored: list[tuple[float, HistoryMessage]] = []
        current_track_ids = {
            d.track_id for d in current_event.detections if d.track_id >= 0
        }
        current_trigger = (
            current_event.trigger_reason.value
            if current_event.trigger_reason
            else ""
        )
        current_ts = current_event.timestamp_ms

        for msg in self._records.values():
            score = self._relevance_score(
                msg,
                current_track_ids=current_track_ids,
                current_trigger=current_trigger,
                current_timestamp_ms=current_ts,
            )
            scored.append((score, msg))

        # 按 score 降序排列
        scored.sort(key=lambda x: x[0], reverse=True)

        # 取 top-N
        selected = scored[:max_n]

        # 记录压缩级别（M4 全量，Level 0）
        compression_level = "full"
        compression_records: list[CompressionRecord] = []

        return HistoryBlock(
            messages=[msg for _, msg in selected],
            summary="",
            compression_level=compression_level,
            compression_records=compression_records,
        )

    # ── 关联度计算 ───────────────────────────────────────────────────────

    @staticmethod
    def _relevance_score(
        msg: HistoryMessage,
        current_track_ids: set[int],
        current_trigger: str,
        current_timestamp_ms: int,
    ) -> float:
        """计算历史记录对当前决策的关联度（0~1）。

        因子:
          - track_id 重叠 (权重 0.50): 同一目标的事件最重要
          - 时间邻近度 (权重 0.25): 10s 内 0.25分，30s 内 0.15分
          - 严重程度 (权重 0.15): critical/high 0.15分
          - 同触发原因 (权重 0.10): 同类型事件的处理经验
        """
        score = 0.0

        # Track ID 重叠
        msg_track_ids = set(msg.track_ids) - {-1}
        if msg_track_ids and current_track_ids:
            overlap = len(msg_track_ids & current_track_ids)
            if overlap > 0:
                score += WEIGHT_TRACK_OVERLAP * min(1.0, overlap / len(current_track_ids))

        # 时间邻近度
        if current_timestamp_ms > 0 and msg.timestamp_ms > 0:
            delta_ms = abs(current_timestamp_ms - msg.timestamp_ms)
            delta_s = delta_ms / 1000.0
            if delta_s <= 10:
                score += WEIGHT_TIME_PROXIMITY * 1.0
            elif delta_s <= 30:
                score += WEIGHT_TIME_PROXIMITY * 0.6

        # 严重程度
        if msg.severity in ("critical", "high"):
            score += WEIGHT_SEVERITY

        # 同触发原因
        if current_trigger and msg.trigger_reason == current_trigger:
            score += WEIGHT_SAME_TRIGGER

        return min(1.0, score)
