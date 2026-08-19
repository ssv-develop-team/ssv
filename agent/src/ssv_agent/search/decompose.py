"""自然语言查询分解。"""

from __future__ import annotations

from typing import Any

from ssv_agent.search.schema import DecomposedQuery


async def decompose_query(
    question: str,
    *,
    llm: Any | None = None,
    top_k: int = 10,
) -> DecomposedQuery:
    """把自然语言拆成结构化查询。

    当前为占位实现：直接透传问题文本；LLM 分解接口保留，后续接入模型后
    再填充 source / 时间范围 / event_type / verdict 等字段。
    """
    return DecomposedQuery(query_text=question, top_k=top_k)
