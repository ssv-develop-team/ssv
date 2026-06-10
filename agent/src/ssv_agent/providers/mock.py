"""Mock Provider —— 用于测试和开发阶段，不依赖真实模型 API。"""

from __future__ import annotations

import time
from typing import Optional

import structlog

from ssv_agent.providers.base import (
    BaseProvider,
    BaseVLMProvider,
    ReviewConclusion,
)

logger = structlog.get_logger()


class MockProvider(BaseProvider):
    """Mock 文本模型 provider。

    根据预定义策略返回结论文本，不调用任何外部 API。
    """

    def __init__(
        self,
        fixed_response: Optional[str] = None,
        simulate_latency_ms: float = 0.0,
    ) -> None:
        self._fixed = fixed_response
        self._latency = simulate_latency_ms
        self._call_count = 0

    @property
    def provider_name(self) -> str:
        return "mock-text-provider"

    @property
    def call_count(self) -> int:
        return self._call_count

    def analyze(self, context) -> str:
        """模拟分析，返回固定或基于上下文的结论文本。

        M4 兼容: context 可以是 ReviewContext 或 ContextPack。
        ContextPack 会先通过 to_review_context() 转换。
        """
        self._call_count += 1

        if self._latency > 0:
            time.sleep(self._latency / 1000.0)

        if self._fixed:
            return self._fixed

        # M4 ContextPack 兼容 → 转为 ReviewContext
        from ssv_agent.context.pack import ContextPack
        if isinstance(context, ContextPack):
            context = context.to_review_context()

        # 根据策略生成模拟结论文本
        from ssv_agent.models.event import ReviewStrategy

        strategy = getattr(context, "strategy", None)
        if strategy == ReviewStrategy.DIRECT_CONFIRM:
            return "[mock] 直接确认: 检测到明确的安全违规，无需额外复核"
        elif strategy == ReviewStrategy.VISUAL_REVIEW:
            return "[mock] 视觉复核: 经关键帧分析，确认检测结果有效"
        elif strategy == ReviewStrategy.RULE_EXPLAIN:
            return (
                "[mock] 规则解释: 根据《安全帽佩戴管理规定》第3条，"
                "进入施工区域必须正确佩戴安全帽。当前检测显示存在违规情况。"
            )
        elif strategy == ReviewStrategy.NOTIFY_REPORT:
            return "[mock] 通知报告: 严重安全违规事件，建议立即通知现场安全员"
        else:
            return f"[mock] 分析完成 (strategy={strategy})"

    def analyze_from_messages(self, messages: list) -> str:
        """M5 目标接口: 接收已组装的 messages 列表，直接调用 LLM API。

        M4 阶段: 模拟实现，忽略 messages 内容，返回与 analyze() 相同的结果。
        """
        self._call_count += 1
        if self._fixed:
            return self._fixed
        return "[mock] analyze_from_messages: 模拟 LLM 调用完成"


class MockVLMProvider(BaseVLMProvider):
    """Mock 多模态视觉复核 provider。

    返回预定义复核结论，不读取真实图像。
    """

    def __init__(
        self,
        fixed_conclusion: Optional[ReviewConclusion] = None,
        simulate_latency_ms: float = 0.0,
    ) -> None:
        self._fixed = fixed_conclusion
        self._latency = simulate_latency_ms
        self._call_count = 0

    @property
    def provider_name(self) -> str:
        return "mock-vlm-provider"

    @property
    def call_count(self) -> int:
        return self._call_count

    def review_keyframe(
        self, image_path: str, context
    ) -> ReviewConclusion:
        """模拟视觉复核。"""
        self._call_count += 1

        if self._latency > 0:
            time.sleep(self._latency / 1000.0)

        if self._fixed:
            return self._fixed

        return ReviewConclusion(
            is_helmet_worn=False,
            explanation=f"[mock] 已复核关键帧 {image_path}，确认未佩戴安全帽",
            confidence=0.92,
            key_observations=["头部区域可见", "安全帽区域未检测到"],
        )
