"""T4 提示词管理模块 —— 集中管理 Agent 提示词的模板、组装、版本和示例。

模块结构:
  - system:     系统提示词（角色定义 + 输出格式约束 + 五层分层）
  - templates:  按复核策略组织的提示词模板
  - examples:   Few-shot 示例库
  - assembler:  提示词组装器（消费 ContextPack → messages 列表）
  - manager:    [DEPRECATED] 轻量管理 → 合并到 context/system_prompt.py
  - logger:     调试日志（记录每次 LLM 调用的 prompt 和 response）
"""

from ssv_agent.prompts.assembler import PromptAssembler, PromptAssembly, PromptMessage
from ssv_agent.prompts.logger import CallRecord, PromptLogger

# manager.py 已废弃（D7），功能合并到 context/system_prompt.py
# 保留导入以兼容旧代码，但推荐使用 SystemPromptManager
from ssv_agent.prompts.manager import get_versions, validate

__all__ = [
    "PromptAssembler",
    "PromptAssembly",
    "PromptMessage",
    "PromptLogger",
    "CallRecord",
    "get_versions",
    "validate",
]
