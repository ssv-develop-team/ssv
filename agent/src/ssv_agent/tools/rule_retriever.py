"""rule_retriever 工具：规则知识检索。"""

from __future__ import annotations

import asyncio
import json
import os

from langchain.tools import tool

from ssv_agent.knowledge.registry import get_retriever


def _knowledge_backend() -> str:
    return os.getenv("SSV_KNOWLEDGE_BACKEND", "mock")


@tool("rule_retriever", parse_docstring=True)
def rule_retriever_tool(
    query: str,
    top_k: int = 5,
    source: str | None = None,
) -> str:
    """检索安全规则片段，返回带来源的知识 chunk。

    Args:
        query: 检索问题或事件描述。
        top_k: 最大返回片段数。
        source: 规则来源过滤（可选）。

    Returns:
        JSON 字符串，包含 chunks 与来源元数据。
    """
    try:
        retriever = get_retriever(_knowledge_backend())
        filters = {"source": source} if source else None
        result = asyncio.run(retriever.retrieve(query, top_k=top_k, filters=filters))
        return result.model_dump_json()
    except Exception as exc:
        return json.dumps(
            {"success": False, "error_message": str(exc)},
            ensure_ascii=False,
        )
