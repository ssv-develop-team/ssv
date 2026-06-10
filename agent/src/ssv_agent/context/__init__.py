"""T4 上下文工程子系统 —— 统一管理五大要素的生命周期。

模块结构:
  - engine:             ContextEngine (collect 主入口)
  - pack:               ContextPack + 五要素 Block + HistoryMessage + ToolDefinition
  - budget:             BudgetEngine + TokenBudget
  - system_prompt:      SystemPromptManager (分层拼接 + 版本管理)
  - tool_definitions:   ToolDefinitionsRenderer
  - history_manager:    HistoryManager (记录 + 滑动窗口 + 压缩)
  - retrieval_manager:  RetrievalManager (YAML 加载 + 降级)
  - user_input:         UserInputBuilder (完整 user message 文本)
"""
