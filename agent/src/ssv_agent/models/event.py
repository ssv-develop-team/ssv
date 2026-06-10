"""T4 事件领域模型 —— 强类型 pydantic 数据类。

替换 event_consumer 中的 JSON dict 松耦合操作，保证从 Redis 消费到状态机
全链路使用统一的数据结构。
"""

from __future__ import annotations

import time
from enum import Enum

from pydantic import BaseModel, Field


# ── 检测结果 ──────────────────────────────────────────────────────────────


class Detection(BaseModel):
    """单条检测结果，由 ssvpub C++ 插件写入 Redis Streams。

    坐标统一使用原始视频帧归一化坐标 [0,1]，与 ssv_meta 契约一致。
    """

    model_config = {"populate_by_name": True}

    class_name: str = Field(default="", alias="class", description="类别名称，如 helmet/head")
    class_id: int = Field(default=-1, description="模型类别 ID，-1 表示未设置")
    confidence: float = Field(default=0.0, ge=0.0, le=1.0, description="检测置信度 [0,1]")
    bbox: list[float] = Field(
        default_factory=lambda: [0.0, 0.0, 0.0, 0.0],
        description="归一化左上右下坐标 [x1, y1, x2, y2]",
    )
    track_id: int = Field(default=-1, description="跟踪 ID，-1 表示未跟踪")

    @property
    def area(self) -> float:
        """检测框面积（归一化坐标下）。"""
        w = max(0.0, self.bbox[2] - self.bbox[0])
        h = max(0.0, self.bbox[3] - self.bbox[1])
        return w * h

    @property
    def is_tracked(self) -> bool:
        """是否已被跟踪器分配了 track_id。"""
        return self.track_id >= 0


# ── 事件严重程度 ──────────────────────────────────────────────────────────


class Severity(str, Enum):
    """事件严重程度，对应设计文档中的事件触发条件分级。"""

    LOW = "low"            # 低置信度需复核
    MEDIUM = "medium"      # 连续命中或区域违规
    HIGH = "high"          # 明确的未佩戴安全帽
    CRITICAL = "critical"  # 需立即通知的严重事件


# ── 事件状态 ───────────────────────────────────────────────────────────────


class EventState(str, Enum):
    """Agent 状态机状态，对应设计文档中定义的 8 个流转状态 + 3 个终态。"""

    # 流转状态
    PENDING = "pending"                         # 待消费
    PARSING = "parsing"                         # 事件解析
    CONTEXT_BUILDING = "context_building"       # 上下文构造
    STRATEGY_SELECTING = "strategy_selecting"   # 策略选择
    TOOL_CALLING = "tool_calling"               # 工具调用
    RESULT_AGGREGATING = "result_aggregating"   # 结果汇总
    RESULT_WRITING = "result_writing"           # 结果回写

    # 终态
    COMPLETED = "completed"           # 复核完成
    FAILED = "failed"                 # 复核失败
    NEEDS_HUMAN = "needs_human"       # 待人工复核


# ── 复核策略 ───────────────────────────────────────────────────────────────


class ReviewStrategy(str, Enum):
    """复核策略类型，对应设计文档中定义的四类流程。"""

    DIRECT_CONFIRM = "direct_confirm"   # 直接确认：高置信度违规
    VISUAL_REVIEW = "visual_review"     # 视觉复核：低置信度/遮挡
    RULE_EXPLAIN = "rule_explain"       # 规则解释：结合安全规则
    NOTIFY_REPORT = "notify_report"     # 通知报告：严重事件


# ── 触发原因 ───────────────────────────────────────────────────────────────


class TriggerReason(str, Enum):
    """事件进入 Agent 队列的触发原因。"""

    NO_HELMET = "no_helmet"                       # 检测到未佩戴安全帽
    LOW_CONFIDENCE = "low_confidence"             # 置信度低于阈值
    CONSECUTIVE_HITS = "consecutive_hits"          # 连续多帧命中
    RULE_CONFLICT = "rule_conflict"               # 规则判定冲突
    MANUAL = "manual"                              # 手动触发


# ── Redis 事件消息 ─────────────────────────────────────────────────────────


class DetectionEvent(BaseModel):
    """从 Redis Streams 消费的检测事件消息。

    字段对齐 ssvpub C++ 插件输出的 JSON schema。
    """

    model_config = {"populate_by_name": True}

    event_type: str = Field(default="detection", alias="type", description="消息类型")
    source: str = Field(default="pipeline-0", description="视频源标识")
    timestamp_ms: int = Field(default=0, description="事件时间戳（毫秒）")
    frame_id: int = Field(default=0, description="pipeline 内递增帧编号")
    detections: list[Detection] = Field(default_factory=list, description="检测框列表")

    # ── Agent 扩展字段（当前阶段由 Agent 在消费后填入） ──────────────────
    severity: Severity = Field(default=Severity.LOW, description="事件严重程度")
    trigger_reason: TriggerReason = Field(
        default=TriggerReason.LOW_CONFIDENCE, description="触发原因"
    )
    state: EventState = Field(default=EventState.PENDING, description="Agent 处理状态")
    evidence_paths: list[str] = Field(
        default_factory=list, description="证据文件路径列表（M3 阶段暂为空）"
    )

    @property
    def has_helmet(self) -> bool:
        """事件中是否包含安全帽检测。"""
        return any(d.class_name == "helmet" for d in self.detections)

    @property
    def has_head(self) -> bool:
        """事件中是否包含头部/人员检测。"""
        return any(d.class_name == "head" for d in self.detections)

    @property
    def helmet_detections(self) -> list[Detection]:
        """仅返回安全帽检测。"""
        return [d for d in self.detections if d.class_name == "helmet"]

    @property
    def head_detections(self) -> list[Detection]:
        """仅返回头部/人员检测。"""
        return [d for d in self.detections if d.class_name == "head"]

    @property
    def max_confidence(self) -> float:
        """最高检测置信度。"""
        if not self.detections:
            return 0.0
        return max(d.confidence for d in self.detections)

    @property
    def tracked_objects(self) -> list[Detection]:
        """已被跟踪的目标。"""
        return [d for d in self.detections if d.is_tracked]

    def infer_severity(self) -> Severity:
        """根据检测结果推断事件严重程度。"""
        if not self.detections:
            return Severity.LOW

        has_helmet = self.has_helmet
        has_head = self.has_head
        max_conf = self.max_confidence
        tracked_count = len(self.tracked_objects)

        if has_head and not has_helmet:
            if max_conf > 0.9 and tracked_count > 0:
                return Severity.CRITICAL
            if max_conf > 0.8:
                return Severity.HIGH
            return Severity.MEDIUM

        if has_head and has_helmet:
            if max_conf < 0.6:
                return Severity.LOW
            return Severity.LOW

        if has_helmet and not has_head:
            return Severity.MEDIUM

        return Severity.LOW

    def infer_trigger_reason(self) -> TriggerReason:
        """根据检测结果推断触发原因。"""
        has_helmet = self.has_helmet
        has_head = self.has_head
        max_conf = self.max_confidence

        # 低置信度优先判断（无论是否有违规都需要复核）
        if max_conf < 0.6:
            return TriggerReason.LOW_CONFIDENCE
        if has_head and not has_helmet:
            return TriggerReason.NO_HELMET
        if has_head and has_helmet:
            return TriggerReason.RULE_CONFLICT
        return TriggerReason.MANUAL

    def select_strategy(self) -> ReviewStrategy:
        """根据事件特征自动选择复核策略。"""
        severity = self.infer_severity()
        max_conf = self.max_confidence

        # 严重事件 → 通知报告
        if severity == Severity.CRITICAL:
            return ReviewStrategy.NOTIFY_REPORT

        # 高置信度明确违规（有 head 无 helmet）→ 直接确认
        if severity == Severity.HIGH and max_conf > 0.8:
            return ReviewStrategy.DIRECT_CONFIRM

        # head + helmet 同时存在 → 规则冲突，走规则解释
        if self.has_head and self.has_helmet:
            return ReviewStrategy.RULE_EXPLAIN

        # 低置信度或遮挡 → 视觉复核
        if severity == Severity.LOW or max_conf < 0.6:
            return ReviewStrategy.VISUAL_REVIEW

        # 默认 → 规则解释
        return ReviewStrategy.RULE_EXPLAIN


# ── 复核结果 ───────────────────────────────────────────────────────────────


class ToolResult(BaseModel):
    """单次工具调用结果。"""

    tool_name: str = Field(description="工具名称")
    success: bool = Field(default=True, description="工具调用是否成功")
    output: str = Field(default="", description="工具输出摘要")
    error: str = Field(default="", description="失败时的错误信息")
    called_at: float = Field(
        default_factory=time.time, description="工具调用时间戳"
    )


class ReviewResult(BaseModel):
    """Agent 复核的最终结果，回写到 Redis 或文件。"""

    event_id: str = Field(default="", description="事件唯一标识（Redis msg_id）")
    source: str = Field(default="", description="视频源标识")
    frame_id: int = Field(default=0, description="帧编号")
    final_state: EventState = Field(default=EventState.COMPLETED, description="最终状态")
    strategy: ReviewStrategy = Field(
        default=ReviewStrategy.DIRECT_CONFIRM, description="使用的复核策略"
    )
    conclusion: str = Field(default="", description="复核结论")
    summary: str = Field(default="", description="事件摘要")
    severity: Severity = Field(default=Severity.LOW, description="最终严重程度")
    detections_count: int = Field(default=0, description="检测目标数")
    tool_results: list[ToolResult] = Field(
        default_factory=list, description="工具调用结果列表"
    )
    provider_used: str = Field(default="mock", description="使用的 provider 名称")
    created_at: float = Field(
        default_factory=time.time, description="复核完成时间戳"
    )

    @property
    def is_terminal(self) -> bool:
        """是否为终态。"""
        return self.final_state in (
            EventState.COMPLETED,
            EventState.FAILED,
            EventState.NEEDS_HUMAN,
        )

    @property
    def succeeded(self) -> bool:
        """复核是否成功完成。"""
        return self.final_state == EventState.COMPLETED


# ── 复核上下文 ─────────────────────────────────────────────────────────────


class ReviewContext(BaseModel):
    """传递给 Provider 的复核上下文，汇总事件 + 证据 + 知识片段。"""

    event: DetectionEvent = Field(description="原始检测事件")
    strategy: ReviewStrategy = Field(description="选择的复核策略")
    evidence_summary: str = Field(
        default="", description="证据文件摘要（M3 阶段暂为空）"
    )
    rule_snippets: list[str] = Field(
        default_factory=list, description="相关安全规则片段（M4 阶段暂为空）"
    )
    history_summary: str = Field(
        default="", description="历史处置经验摘要（M4 阶段暂为空）"
    )

    @property
    def prompt_context(self) -> str:
        """构造 provider 调用所需的文本上下文。

        .. deprecated:: M4
            使用 UserInputBuilder.build() 替代。M5 移除。
        """
        import warnings
        warnings.warn(
            "ReviewContext.prompt_context is deprecated, "
            "use UserInputBuilder.build() instead",
            DeprecationWarning,
            stacklevel=2,
        )
        lines = [
            f"事件来源: {self.event.source}",
            f"帧编号: {self.event.frame_id}",
            f"严重程度: {self.event.severity.value}",
            f"触发原因: {self.event.trigger_reason.value}",
            f"复核策略: {self.strategy.value}",
            f"检测目标数: {len(self.event.detections)}",
        ]
        for i, d in enumerate(self.event.detections):
            lines.append(
                f"  检测[{i}]: class={d.class_name}, conf={d.confidence:.2f}, "
                f"bbox=[{d.bbox[0]:.2f},{d.bbox[1]:.2f},{d.bbox[2]:.2f},{d.bbox[3]:.2f}], "
                f"track_id={d.track_id}"
            )
        if self.evidence_summary:
            lines.append(f"证据: {self.evidence_summary}")
        if self.rule_snippets:
            lines.append("相关规则:")
            for r in self.rule_snippets:
                lines.append(f"  - {r}")
        return "\n".join(lines)
