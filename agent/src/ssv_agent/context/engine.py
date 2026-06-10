"""上下文引擎 —— 收集五大要素，产出统一 ContextPack。

设计原则:
  1. T4 自包含 — 不依赖 T1/T2/T3 提供额外数据即可完整运行
  2. 模块未初始化时产出空 Block，不影响其他要素
  3. 集成数据为空时静默跳过
"""

from __future__ import annotations

from typing import Optional

import structlog

from ssv_agent.context.history_manager import HistoryManager
from ssv_agent.context.pack import (
    ContextPack,
    HistoryBlock,
    RetrievalBlock,
    SystemPromptBlock,
    ToolDefinitionsBlock,
    UserInputBlock,
)
from ssv_agent.context.retrieval_manager import RetrievalManager
from ssv_agent.context.system_prompt import SystemPromptManager
from ssv_agent.context.tool_definitions import ToolDefinitionsRenderer
from ssv_agent.context.user_input import UserInputBuilder

logger = structlog.get_logger()


class ContextEngine:
    """上下文引擎 —— 收集五大要素，产出 ContextPack。

    五要素收集顺序:
      SystemPromptManager.render(strategy)  → system_prompt
      ToolDefinitionsRenderer.render(...)   → tool_definitions
      HistoryManager.get_window(event)      → history
      RetrievalManager.search(event, ...)   → retrieval_context
      UserInputBuilder.build(event, ...)    → user_input

    用法:
        engine = ContextEngine()
        pack = engine.collect(event, strategy, tool_definitions=[...])
        # pack 可直接传给 BudgetEngine.allocate() → PromptAssembler.assemble()
    """

    def __init__(
        self,
        system_prompt_manager: Optional[SystemPromptManager] = None,
        tool_renderer: Optional[ToolDefinitionsRenderer] = None,
        history_manager: Optional[HistoryManager] = None,
        retrieval_manager: Optional[RetrievalManager] = None,
        user_input_builder: Optional[UserInputBuilder] = None,
        evidence_provider: Optional[object] = None,
        track_provider: Optional[object] = None,
        metadata_provider: Optional[object] = None,
        sequence_provider: Optional[object] = None,
    ) -> None:
        self._sp_mgr = system_prompt_manager
        self._tool_renderer = tool_renderer
        self._history_mgr = history_manager
        self._retrieval_mgr = retrieval_manager
        self._ui_builder = user_input_builder

        # 集成接口（均可为 None）
        self._evidence = evidence_provider
        self._track = track_provider
        self._metadata = metadata_provider
        self._sequence = sequence_provider

        self._collect_count = 0

    @property
    def collect_count(self) -> int:
        return self._collect_count

    # ── 主入口 ─────────────────────────────────────────────────────────

    def collect(
        self,
        event,
        strategy: str,
        tool_definitions: Optional[list] = None,
    ) -> ContextPack:
        """收集五大要素，产出 ContextPack。

        Args:
            event: DetectionEvent。
            strategy: 复核策略名。
            tool_definitions: ToolDefinition 列表。

        Returns:
            ContextPack: 五要素统一容器。
        """
        self._collect_count += 1

        # 1. 系统提示词
        sp_block = self._collect_system_prompt(strategy, tool_definitions or [])

        # 2. 工具定义
        td_block = self._collect_tool_definitions(
            tool_definitions or [], strategy
        )

        # 3. 历史记录
        hist_block = self._collect_history(event)

        # 4. 检索上下文
        retrieval_block = self._collect_retrieval(event, strategy)

        # 5. 用户输入
        ui_block = self._collect_user_input(event, strategy, retrieval_block)

        pack = ContextPack(
            system_prompt=sp_block,
            tool_definitions=td_block,
            history=hist_block,
            retrieval_context=retrieval_block,
            user_input=ui_block,
            metadata={
                "event": event,
                "strategy": strategy,
                "collect_count": self._collect_count,
            },
        )

        logger.debug(
            "context pack collected",
            context_id=pack.context_id,
            strategy=strategy,
            sp_chars=len(sp_block.content),
            tools_count=len(td_block.tools),
            history_count=len(hist_block.messages),
            retrieval_count=len(retrieval_block.items),
            ui_chars=len(ui_block.event_context),
            collect_count=self._collect_count,
        )
        return pack

    # ── 各要素收集 ───────────────────────────────────────────────────────

    def _collect_system_prompt(
        self, strategy: str, tools: list
    ) -> SystemPromptBlock:
        """收集系统提示词要素。"""
        if self._sp_mgr is None:
            logger.warning("SystemPromptManager not initialized, returning empty block")
            return SystemPromptBlock(content="", versions={})

        # 工具定义渲染为文本（嵌入 system prompt）
        tool_text = ""
        if self._tool_renderer and tools:
            tool_text = self._tool_renderer.render_as_text(tools, strategy)

        content, versions = self._sp_mgr.render(
            strategy=strategy,
            tool_definitions_text=tool_text,
        )
        return SystemPromptBlock(content=content, versions=versions)

    def _collect_tool_definitions(
        self, tools: list, strategy: str
    ) -> ToolDefinitionsBlock:
        """收集工具定义要素。"""
        if self._tool_renderer is None:
            return ToolDefinitionsBlock()

        return self._tool_renderer.render(tools, strategy=strategy)

    def _collect_history(self, event) -> HistoryBlock:
        """收集历史记录要素。"""
        if self._history_mgr is None:
            return HistoryBlock()
        return self._history_mgr.get_window(event)

    def _collect_retrieval(self, event, strategy: str) -> RetrievalBlock:
        """收集检索上下文要素。"""
        if self._retrieval_mgr is None:
            return RetrievalBlock()
        return self._retrieval_mgr.search(event, strategy)

    def _collect_user_input(
        self,
        event,
        strategy: str,
        retrieval_block: RetrievalBlock,
    ) -> UserInputBlock:
        """收集用户输入要素（含模板渲染 + 集成数据注入）。"""
        # 证据摘要
        evidence_summary = ""
        if self._evidence:
            try:
                evidence_summary = self._evidence.get_evidence_summary(event)
            except Exception as exc:
                logger.warning("evidence provider failed", error=str(exc))

        # 跟踪上下文
        track_contexts = []
        if self._track:
            for d in event.detections:
                if d.track_id >= 0:
                    try:
                        track_contexts.append({
                            "track_id": d.track_id,
                            "consecutive": self._track.get_consecutive_count(
                                event, d.track_id
                            ),
                            "state": self._track.get_track_state(event, d.track_id),
                            "age": self._track.get_track_age(event, d.track_id),
                        })
                    except Exception as exc:
                        logger.warning("track provider failed", error=str(exc))

        # 场景元数据
        source_metadata = None
        if self._metadata:
            try:
                from ssv_agent.context.pack import SourceMetadata
                scene_info = self._metadata.get_scene_info(event)
                if scene_info:
                    source_metadata = SourceMetadata(scene_info=scene_info)
            except Exception as exc:
                logger.warning("metadata provider failed", error=str(exc))

        if self._ui_builder is None:
            # 降级：使用简单文本
            return UserInputBlock(
                event_context=f"请复核以下检测事件:\n\n{event.source} frame={event.frame_id}",
                evidence_summary=evidence_summary,
            )

        return self._ui_builder.build(
            event=event,
            strategy=strategy,
            retrieval_block=retrieval_block,
            evidence_summary=evidence_summary,
            source_metadata=source_metadata,
        )
