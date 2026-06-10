"""用户输入构建器 —— 将事件 + 集成数据渲染为完整的 user message 文本。

设计原则（D6）:
  UserInputBuilder 输出完整的 user message 文本（含模板渲染）。
  让 PromptAssembler 的职责收缩为"拼接 + 截断"两个操作。
  收敛: prompt_context 字符串拼接 + _build_event_context() → UserInputBuilder.build()
"""

from __future__ import annotations

from typing import Optional

import structlog

from ssv_agent.context.pack import (
    RetrievalBlock,
    SourceMetadata,
    TrackContext,
    UserInputBlock,
)
from ssv_agent.prompts.templates import STRATEGY_TASK_TEMPLATES

logger = structlog.get_logger()


class UserInputBuilder:
    """用户输入构建器 —— 构造完整的 user message 文本。

    收敛了以下散落逻辑:
      - ReviewContext.prompt_context 字符串拼接 (models/event.py)
      - _build_event_context() 字符串拼接 (prompts/assembler.py)
      - _render_task_template() 模板渲染 (prompts/assembler.py)

    用法:
        builder = UserInputBuilder()
        block = builder.build(event, strategy, retrieval_block, ...)
        # block.event_context → 渲染后的完整 user message
    """

    def __init__(self, include_bbox: bool = True) -> None:
        self._include_bbox = include_bbox
        self._build_count = 0

    @property
    def build_count(self) -> int:
        return self._build_count

    def build(
        self,
        event,
        strategy: str,
        retrieval_block: Optional[RetrievalBlock] = None,
        evidence_summary: str = "",
        track_contexts: Optional[list[TrackContext]] = None,
        source_metadata: Optional[SourceMetadata] = None,
        full_details: bool = True,
    ) -> UserInputBlock:
        """构建用户输入 Block。

        Args:
            event: DetectionEvent。
            strategy: 复核策略名。
            retrieval_block: 检索上下文（用于填充规则）。
            evidence_summary: 证据文件摘要。
            track_contexts: 跟踪上下文列表。
            source_metadata: 视频源元数据。
            full_details: True → 完整事件文本（含 bbox），False → 精简文本。

        Returns:
            UserInputBlock: 含渲染后的事件上下文。
        """
        self._build_count += 1

        # 事件文本
        event_context = self._render_event(event, full_details=full_details)

        # 模板渲染
        template = STRATEGY_TASK_TEMPLATES.get(strategy)
        if template is None:
            task_text = f"请复核以下检测事件:\n\n{event_context}"
        else:
            rules_text = self._render_rule_snippets(retrieval_block)
            evidence = evidence_summary or "（暂无可用的关键帧证据）"

            task_text = template.content.format(
                event_context=event_context,
                evidence_summary=evidence,
                rule_snippets=rules_text,
            )

        logger.debug(
            "user input built",
            strategy=strategy,
            full_details=full_details,
            chars=len(task_text),
            build_count=self._build_count,
        )

        return UserInputBlock(
            event_context=task_text,
            evidence_summary=evidence_summary,
            track_context=track_contexts or [],
            source_metadata=source_metadata,
        )

    def _render_event(self, event, full_details: bool = True) -> str:
        """构造事件上下文文本。

        收敛自:
          - ReviewContext.prompt_context 属性
          - PromptAssembler._build_event_context()
        """
        lines = [
            f"来源: {event.source}",
            f"帧编号: {event.frame_id}",
            f"时间戳: {event.timestamp_ms}",
            f"严重程度: {event.severity.value if event.severity else 'unknown'}",
            f"触发原因: {event.trigger_reason.value if event.trigger_reason else 'unknown'}",
            f"检测目标数: {len(event.detections)}",
            "",
            "检测详情:",
        ]
        for i, d in enumerate(event.detections):
            if full_details:
                tracked = f", track_id={d.track_id}" if d.is_tracked else " (未跟踪)"
                lines.append(
                    f"  [{i}] {d.class_name} conf={d.confidence:.2f} "
                    f"bbox=[{d.bbox[0]:.2f},{d.bbox[1]:.2f},{d.bbox[2]:.2f},{d.bbox[3]:.2f}]{tracked}"
                )
            else:
                lines.append(f"  [{i}] {d.class_name} conf={d.confidence:.2f}")
        return "\n".join(lines)

    @staticmethod
    def _render_rule_snippets(
        retrieval_block: Optional[RetrievalBlock],
    ) -> str:
        """渲染规则片段为分级格式，帮助 LLM 自行判断采信权重。

        未匹配时显式告知 LLM，防止编造规则。
        """
        if not retrieval_block or not retrieval_block.items:
            return "（未找到匹配规则。请基于系统提示中的判断原则独立复核。）"

        # 按来源分级
        regulations: list[str] = []
        experts: list[str] = []
        others: list[str] = []

        for item in retrieval_block.items:
            line = f"- {item.source}:\n  {item.content}"
            if item.source_type == "regulation":
                regulations.append(line)
            elif item.source_type == "expert":
                experts.append(line)
            else:
                others.append(line)

        parts: list[str] = []
        if regulations:
            parts.append("### 法规依据\n" + "\n".join(regulations))
        if experts:
            parts.append("### 专家经验\n" + "\n".join(experts))
        if others:
            parts.append("### 其他参考\n" + "\n".join(others))

        return "\n\n".join(parts)
