"""规则解释组装（确定性实现，LLM 接口预留）。"""

from __future__ import annotations

from typing import Any

from ssv_agent.knowledge.schema import Chunk


async def compose_rule_explanation(
    query: str,
    chunks: list[Chunk],
    *,
    llm: Any | None = None,
) -> str:
    """结合规则片段生成带来源的解释。

    当前为确定性模板实现，保证来源可追溯；`llm` 参数预留，后续接入模型后
    可替换为自然语言解释生成。
    """
    if not chunks:
        return "未检索到相关规则片段，解释缺少知识依据。"

    lines = [f"问题：{query}", "依据的规则片段："]
    for chunk in chunks:
        source = chunk.metadata.get("source", "未知来源")
        rule_id = chunk.metadata.get("rule_id", "unknown")
        section = chunk.metadata.get("section", "未知章节")
        lines.append(f"- [{rule_id}/{source}/{section}] {chunk.content}")
    lines.append("说明：以上内容按检索结果引用，未检索到的部分不推断。")
    return "\n".join(lines)
