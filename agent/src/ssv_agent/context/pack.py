"""上下文数据容器 —— ContextPack 统一五要素容器 + Block 数据类 + ToolDefinition。

设计原则（D3）:
  ReviewContext 废弃，ContextPack 成为唯一容器。
  M4 保留 to_review_context() 兼容方法，M5 移除。
"""

from __future__ import annotations

import uuid
from dataclasses import dataclass, field
from typing import Optional


# ── ToolDefinition ───────────────────────────────────────────────────────────


@dataclass
class ToolDefinition:
    """工具自描述数据类 —— 供 ToolDefinitionsRenderer 使用。

    BaseTool.to_definition() 导出纯数据对象，解耦渲染与执行。
    """

    name: str
    description: str
    parameters_schema: dict
    returns_description: str
    side_effects: str


# ── HistoryMessage ───────────────────────────────────────────────────────────


@dataclass
class HistoryMessage:
    """压缩后的历史记录消息。

    track_ids 中的 -1 表示未跟踪（T2 未接通时默认值）。
    """

    event_id: str
    track_ids: list[int]          # -1 表示未跟踪
    timestamp_ms: int
    severity: str
    trigger_reason: str
    strategy: str
    conclusion_summary: str       # 一条压缩后的结论摘要


@dataclass
class CompressionRecord:
    """单次压缩操作记录。"""

    level: str                    # "full" / "light" / "per_track" / "global"
    discarded_count: int
    compression_ratio: float      # 压缩后/压缩前 token 估算比


# ── 五要素 Block ─────────────────────────────────────────────────────────────


@dataclass
class SystemPromptBlock:
    """系统提示词要素。"""

    content: str                              # 渲染后的完整系统提示词文本
    versions: dict[str, str] = field(default_factory=dict)  # 各层版本快照


@dataclass
class ToolDefinitionsBlock:
    """工具定义要素。"""

    tools: list[ToolDefinition] = field(default_factory=list)
    format: str = "text"                      # "openai" / "anthropic" / "text"


@dataclass
class HistoryBlock:
    """历史记录要素。"""

    messages: list[HistoryMessage] = field(default_factory=list)
    summary: str = ""                          # 被移除消息的摘要
    compression_level: str = "full"            # "full" / "light" / "per_track" / "global"
    compression_records: list[CompressionRecord] = field(default_factory=list)


@dataclass
class RetrievalItem:
    """单条检索结果。"""

    content: str
    source: str                                # 来源引用
    source_type: str                           # "regulation" / "expert" / "statistical"
    priority: int = 0                          # 排序优先级


@dataclass
class RetrievalBlock:
    """检索上下文要素。"""

    items: list[RetrievalItem] = field(default_factory=list)


@dataclass
class TrackContext:
    """单条跟踪上下文。"""

    track_id: int
    consecutive_count: int = 0
    track_state: str = "unknown"
    track_age: float = 0.0


@dataclass
class SourceMetadata:
    """视频源元数据。"""

    scene_info: str = ""
    location: str = ""
    lighting: str = ""


@dataclass
class UserInputBlock:
    """用户输入要素。"""

    event_context: str = ""              # 渲染后的事件文本
    evidence_summary: str = ""
    track_context: list[TrackContext] = field(default_factory=list)
    source_metadata: Optional[SourceMetadata] = None


# ── ContextPack ──────────────────────────────────────────────────────────────


@dataclass
class ContextPack:
    """五要素统一容器 —— 替代 ReviewContext（D3）。

    ContextEngine.collect() 产出 ContextPack，
    BudgetEngine.allocate() 附加 TokenBudget，
    PromptAssembler.assemble() 消费 ContextPack 产出 PromptAssembly。
    """

    system_prompt: SystemPromptBlock
    tool_definitions: ToolDefinitionsBlock
    history: HistoryBlock
    retrieval_context: RetrievalBlock
    user_input: UserInputBlock
    context_id: str = field(default_factory=lambda: f"ctx_{uuid.uuid4().hex[:12]}")
    token_budget: Optional[object] = None  # BudgetEngine 填充 (TokenBudget 避免循环导入)
    metadata: dict = field(default_factory=dict)

    def to_review_context(self):
        """M4 向后兼容：将 ContextPack 转换为 ReviewContext。

        M5 移除。
        """
        from ssv_agent.models.event import ReviewContext

        # 从 UserInputBlock 中提取 event 相关字段
        return ReviewContext(
            event=self.metadata.get("event"),
            strategy=self.metadata.get("strategy"),
            evidence_summary=self.user_input.evidence_summary,
            rule_snippets=[item.content for item in self.retrieval_context.items],
            history_summary=self.history.summary,
        )
