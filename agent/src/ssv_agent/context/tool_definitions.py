"""工具定义渲染器 —— 将 ToolDefinition 列表渲染为 LLM 可见的文本/结构化格式。

设计原则:
  1. ToolDefinitionsRenderer 只依赖 ToolDefinition 纯数据，不依赖 BaseTool 实例
  2. 支持三种注入格式: OpenAI native / Anthropic native / 紧凑纯文本
  3. 按策略条件过滤可见工具
"""

from __future__ import annotations

import json

import structlog

from ssv_agent.context.pack import ToolDefinition, ToolDefinitionsBlock

logger = structlog.get_logger()

# 按策略过滤可见工具
_STRATEGY_TOOL_FILTER: dict[str, set[str]] = {
    "direct_confirm": {"confirm_violation"},
    "visual_review": {"visual_review", "confirm_violation"},
    "rule_explain": {"rule_retrieval", "confirm_violation"},
    "notify_report": {"generate_notification", "report_generation", "confirm_violation"},
}


class ToolDefinitionsRenderer:
    """工具定义渲染器 —— 将工具列表渲染为目标格式。

    用法:
        renderer = ToolDefinitionsRenderer()
        block = renderer.render(tools, strategy="direct_confirm", format="text")
        # block.tools → 过滤后的工具定义
        # block.format → 目标格式
    """

    def __init__(self) -> None:
        self._render_count = 0

    @property
    def render_count(self) -> int:
        return self._render_count

    def render(
        self,
        tools: list[ToolDefinition],
        strategy: str = "",
        format: str = "text",
    ) -> "ToolDefinitionsBlock":
        """渲染工具定义为指定格式。

        Args:
            tools: 已注册的工具定义列表。
            strategy: 复核策略名（用于过滤可见工具）。
            format: 目标格式 ("openai" / "anthropic" / "text")。

        Returns:
            ToolDefinitionsBlock: 含过滤后的工具和格式标记。
        """
        self._render_count += 1

        # 条件过滤
        filtered = self._filter_by_strategy(tools, strategy)

        logger.debug(
            "tool definitions rendered",
            strategy=strategy,
            format=format,
            total_tools=len(tools),
            visible_tools=len(filtered),
        )
        return ToolDefinitionsBlock(tools=filtered, format=format)

    def render_as_text(self, tools: list[ToolDefinition], strategy: str = "") -> str:
        """将工具定义渲染为紧凑纯文本（嵌入 system prompt）。

        Args:
            tools: 已注册的工具定义列表。
            strategy: 复核策略名（用于过滤）。

        Returns:
            渲染后的文本。
        """
        filtered = self._filter_by_strategy(tools, strategy)
        if not filtered:
            return ""

        lines = ["## 可用工具"]
        for tool in filtered:
            lines.append(f"\n### {tool.name}")
            lines.append(f"- 用途: {tool.description}")
            if tool.parameters_schema:
                try:
                    params_str = json.dumps(tool.parameters_schema, ensure_ascii=False)
                except (TypeError, ValueError):
                    params_str = str(tool.parameters_schema)
                lines.append(f"- 参数: {params_str}")
            lines.append(f"- 返回: {tool.returns_description}")
            if tool.side_effects:
                lines.append(f"- 副作用: {tool.side_effects}")

        return "\n".join(lines)

    def render_as_openai(self, tools: list[ToolDefinition]) -> list[dict]:
        """渲染为 OpenAI function calling 格式。

        Args:
            tools: 工具定义列表。

        Returns:
            OpenAI tools 参数格式。
        """
        result: list[dict] = []
        for tool in tools:
            result.append({
                "type": "function",
                "function": {
                    "name": tool.name,
                    "description": tool.description,
                    "parameters": tool.parameters_schema,
                },
            })
        return result

    @staticmethod
    def _filter_by_strategy(
        tools: list[ToolDefinition], strategy: str
    ) -> list[ToolDefinition]:
        """按策略过滤可见工具。"""
        if not strategy:
            return list(tools)

        visible = _STRATEGY_TOOL_FILTER.get(strategy)
        if visible is None:
            return list(tools)

        return [t for t in tools if t.name in visible]
