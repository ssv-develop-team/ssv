"""上下文构造器 —— 汇总事件元数据、证据路径和规则片段，生成 ReviewContext。

.. deprecated:: M4
    功能已收敛到 context/engine.py (ContextEngine) + context/retrieval_manager.py。
    M5 移除。
"""

from __future__ import annotations

import structlog
import warnings

from ssv_agent.models.event import (
    DetectionEvent,
    ReviewContext,
    ReviewStrategy,
)

logger = structlog.get_logger()


class ContextBuilder:
    """构造 Provider 所需的复核上下文。

    当前阶段（M4）证据路径和规则片段为空占位，后续 M3 完成后接入。
    """

    def __init__(self) -> None:
        self._build_count = 0

    @property
    def build_count(self) -> int:
        return self._build_count

    def build(
        self,
        event: DetectionEvent,
        strategy: ReviewStrategy,
    ) -> ReviewContext:
        """构造复核上下文。

        .. deprecated:: M4
            使用 ContextEngine.collect() 替代。M5 移除。
        """
        warnings.warn(
            "ContextBuilder is deprecated, use ContextEngine.collect() instead",
            DeprecationWarning,
            stacklevel=2,
        )
        self._build_count += 1

        ctx = ReviewContext(
            event=event,
            strategy=strategy,
            evidence_summary=self._collect_evidence(event),
            rule_snippets=self._lookup_rules(event),
            history_summary="",
        )

        logger.debug(
            "context built",
            strategy=strategy.value,
            detections=len(event.detections),
            evidence_count=len(ctx.evidence_summary) > 0,
            rule_count=len(ctx.rule_snippets),
        )
        return ctx

    def _collect_evidence(self, event: DetectionEvent) -> str:
        """收集证据文件摘要。

        当前 M3 未完成，证据路径为空，返回占位符。
        后续接入：读取 frame_path/clip_path，生成文件摘要。
        """
        if event.evidence_paths:
            paths = ", ".join(event.evidence_paths)
            return f"证据文件: {paths}"
        return ""

    def _lookup_rules(self, event: DetectionEvent) -> list[str]:
        """检索相关安全规则。

        当前 M4 未完成向量数据库，返回默认规则片段。
        后续接入：调用向量检索或规则知识库。
        """
        rules: list[str] = []
        trigger = event.trigger_reason.value

        if trigger in ("no_helmet", "low_confidence"):
            rules.append("安全帽佩戴规范: 进入施工区域必须正确佩戴安全帽")
        if trigger in ("consecutive_hits",):
            rules.append("连续违规判定: 同一目标连续3帧以上未佩戴安全帽触发告警")
        if trigger in ("rule_conflict",):
            rules.append("冲突判定规则: 人员框存在但安全帽框不稳定时需人工复核")

        return rules
