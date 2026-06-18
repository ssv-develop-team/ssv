"""状态机上下文 —— 在状态流转过程中携带事件、上下文、工具结果和错误信息。"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Optional

from ssv_agent.models.event import (
    DetectionEvent,
    EventState,
    ReviewContext,
    ReviewResult,
    ReviewStrategy,
    ToolResult,
)


@dataclass
class StateContext:
    """状态机全生命周期上下文。

    随着状态流转逐步填充各字段，最终汇聚为 ReviewResult。
    """

    event: DetectionEvent
    state: EventState = EventState.PENDING
    strategy: ReviewStrategy = ReviewStrategy.DIRECT_CONFIRM
    context: Optional[ReviewContext] = None
    tool_results: list[ToolResult] = field(default_factory=list)
    error: str = ""
    provider_used: str = "none"
    started_at: float = field(default_factory=time.time)
    completed_at: float = 0.0
    timeout_seconds: int = 300

    def transition(self, new_state: EventState) -> None:
        """记录状态迁移。"""
        self.state = new_state

    def record_tool_result(self, result: ToolResult) -> None:
        """追加一次工具调用结果。"""
        self.tool_results.append(result)

    def set_error(self, error: str) -> None:
        """记录错误信息并标记失败。"""
        self.error = error

    def build_result(self) -> ReviewResult:
        """从当前上下文构造 ReviewResult。"""
        self.completed_at = time.time()
        return ReviewResult(
            event_id=f"{self.event.source}-{self.event.frame_id}",
            source=self.event.source,
            frame_id=self.event.frame_id,
            final_state=self.state,
            strategy=self.strategy,
            conclusion=self._synthesize_conclusion(),
            summary=self._synthesize_summary(),
            severity=self.event.severity,
            detections_count=len(self.event.detections),
            tool_results=self.tool_results,
            provider_used=self.provider_used,
            created_at=self.completed_at,
        )

    def _synthesize_conclusion(self) -> str:
        """根据状态和工具结果合成复核结论。"""
        if self.state == EventState.COMPLETED:
            if self.strategy == ReviewStrategy.DIRECT_CONFIRM:
                return "直接确认: 检测到未佩戴安全帽，高置信度违规"
            if self.strategy == ReviewStrategy.VISUAL_REVIEW:
                return "视觉复核: 经关键帧复核，确认检测结果有效"
            if self.strategy == ReviewStrategy.RULE_EXPLAIN:
                return "规则解释: 结合安全规则，需进一步人工判断"
            if self.strategy == ReviewStrategy.NOTIFY_REPORT:
                return "通知报告: 严重事件，已生成通知"
            return "复核完成"
        if self.state == EventState.FAILED:
            return f"复核失败: {self.error}"
        if self.state == EventState.NEEDS_HUMAN:
            return "待人工复核: 自动复核无法得出确定结论"
        return "未完成"

    def _synthesize_summary(self) -> str:
        """生成事件摘要。"""
        det_summary = ", ".join(
            f"{d.class_name}(conf={d.confidence:.2f})"
            for d in self.event.detections[:5]
        )
        if len(self.event.detections) > 5:
            det_summary += f" ... 共{len(self.event.detections)}个检测"
        return (
            f"[{self.event.severity.value}] 来源={self.event.source} "
            f"帧={self.event.frame_id} 策略={self.strategy.value} "
            f"检测={{{det_summary}}}"
        )

    @property
    def is_terminal(self) -> bool:
        """当前状态是否为终态。"""
        return self.state in (
            EventState.COMPLETED,
            EventState.FAILED,
            EventState.NEEDS_HUMAN,
        )

    @property
    def elapsed_seconds(self) -> float:
        """从启动至今的耗时（秒）。"""
        return time.time() - self.started_at

    @property
    def has_timed_out(self) -> bool:
        """是否已超时。"""
        return self.elapsed_seconds > self.timeout_seconds
