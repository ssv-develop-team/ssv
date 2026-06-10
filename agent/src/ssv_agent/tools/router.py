"""工具调用路由 —— 注册工具、按名调用、超时控制、错误记录。"""

from __future__ import annotations

import structlog

from ssv_agent.models.event import ToolResult
from ssv_agent.tools.base import BaseTool

logger = structlog.get_logger()


class ToolRouter:
    """工具调用路由器。

    管理已注册的工具，提供统一的调用接口和错误处理。

    用法:
        router = ToolRouter()
        router.register(MyTool())
        result = router.call_tool("my_tool", {"key": "value"})
    """

    def __init__(self, default_timeout_ms: int = 30000) -> None:
        self._tools: dict[str, BaseTool] = {}
        self._default_timeout = default_timeout_ms
        self._call_count = 0

    @property
    def call_count(self) -> int:
        return self._call_count

    @property
    def registered_tools(self) -> list[str]:
        return list(self._tools.keys())

    def register(self, tool: BaseTool) -> None:
        """注册一个工具。"""
        if tool.name in self._tools:
            logger.warning("tool already registered, overwriting", tool=tool.name)
        self._tools[tool.name] = tool
        logger.debug("tool registered", tool=tool.name)

    def unregister(self, tool_name: str) -> None:
        """注销一个工具。"""
        self._tools.pop(tool_name, None)

    def call_tool(self, name: str, params: dict) -> ToolResult:
        """调用指定工具。

        Args:
            name: 工具名称。
            params: 工具参数字典。

        Returns:
            ToolResult: 成功或失败的调用结果。
        """
        self._call_count += 1

        if name not in self._tools:
            return ToolResult(
                tool_name=name,
                success=False,
                error=f"工具未注册: {name}",
            )

        tool = self._tools[name]
        try:
            logger.info("calling tool", tool=name, params=list(params.keys()))
            result = tool.execute(params)
            logger.debug("tool result", tool=name, success=result.success)
            return result
        except Exception as exc:
            logger.warning("tool call failed", tool=name, error=str(exc))
            return ToolResult(
                tool_name=name,
                success=False,
                error=str(exc),
            )
