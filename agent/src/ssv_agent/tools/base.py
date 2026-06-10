"""工具抽象基类 —— 定义工具调用协议 + 自描述能力。

设计原则（4.2 节）:
  新增四个自描述属性 + 一个导出方法，使 LLM 能看见和调用工具。
  ToolDefinitionsRenderer 只依赖 ToolDefinition 纯数据，解耦渲染与执行。
"""

from __future__ import annotations

from abc import ABC, abstractmethod

from ssv_agent.context.pack import ToolDefinition
from ssv_agent.models.event import ToolResult


class BaseTool(ABC):
    """工具抽象基类。

    所有工具调用统一为: 工具名 + 参数 + 返回 ToolResult。

    LLM 可见属性（自描述）:
      - description: LLM 判断"该不该调用"
      - parameters_schema: LLM 构造调用参数
      - returns_description: LLM 理解返回值
      - side_effects: LLM 理解副作用
    """

    @property
    @abstractmethod
    def name(self) -> str:
        """工具名称。"""
        ...

    @property
    @abstractmethod
    def description(self) -> str:
        """工具用途描述 —— LLM 判断"该不该调用"。

        一句话说清输入→输出映射，包含使用时机。
        示例: "输入触发原因，返回匹配的安全规范条文。当检测信号冲突时使用。"
        """
        ...

    @property
    @abstractmethod
    def parameters_schema(self) -> dict:
        """工具参数 JSON Schema —— LLM 构造调用参数。

        示例: {"type":"object","properties":{"event_type":{"enum":["no_helmet",...]}}}
        """
        ...

    @property
    @abstractmethod
    def returns_description(self) -> str:
        """工具返回值描述 —— LLM 理解返回值。

        必须说明失败时的语义（返回空 / 抛出异常）。
        示例: "返回规则文本列表。无匹配时返回空列表（不是错误）。"
        """
        ...

    @property
    @abstractmethod
    def side_effects(self) -> str:
        """工具副作用描述 —— LLM 理解调用后果。

        无副作用时返回空字符串 ""。
        示例: "发送通知到现场安全员" / ""
        """
        ...

    @abstractmethod
    def execute(self, params: dict) -> ToolResult:
        """执行工具调用。

        Args:
            params: 工具参数字典。

        Returns:
            ToolResult: 调用结果。
        """
        ...

    def to_definition(self) -> ToolDefinition:
        """导出为纯数据 ToolDefinition —— 供 ToolDefinitionsRenderer 使用。

        ToolDefinitionsRenderer 只依赖 ToolDefinition 而非 BaseTool 实例，
        解耦渲染与执行。
        """
        return ToolDefinition(
            name=self.name,
            description=self.description,
            parameters_schema=self.parameters_schema,
            returns_description=self.returns_description,
            side_effects=self.side_effects,
        )
