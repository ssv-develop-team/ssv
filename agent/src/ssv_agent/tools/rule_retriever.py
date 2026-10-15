"""rule_retriever 工具：规则知识检索。"""

from __future__ import annotations

import asyncio
from concurrent.futures import ThreadPoolExecutor
import json
import os
from collections.abc import Coroutine
from typing import Any

from langchain.tools import tool

from ssv_agent.knowledge.registry import get_retriever


def _knowledge_backend() -> str:
    return os.getenv("SSV_KNOWLEDGE_BACKEND", "local_markdown")


def _run_async(coroutine: Coroutine[Any, Any, Any]) -> Any:
    """在同步工具中安全运行协程，包括调用方已有事件循环的情况。"""
    try:
        asyncio.get_running_loop()
    except RuntimeError:
        return asyncio.run(coroutine)
    with ThreadPoolExecutor(max_workers=1, thread_name_prefix="ssv-rule-retriever") as executor:
        return executor.submit(asyncio.run, coroutine).result()


@tool("rule_retriever", parse_docstring=True)
def rule_retriever_tool(
    query: str,
    top_k: int = 5,
    source: str | None = None,
    rule_id: str | None = None,
    rule_version: str | None = None,
    event_type: str | None = None,
) -> str:
    """检索安全规则片段，返回带来源的知识 chunk。

    Args:
        query: 检索问题或事件描述。
        top_k: 最大返回片段数。
        source: 规则来源过滤（可选）。
        rule_id: 规则标识过滤（可选）。
        rule_version: 规则版本过滤（可选）。
        event_type: 业务事件类型过滤（可选）。

    Returns:
        JSON 字符串，包含 chunks 与来源元数据。
    """
    try:
        retriever = get_retriever(_knowledge_backend())
        filters = {
            key: value
            for key, value in {
                "source": source,
                "rule_id": rule_id,
                "rule_version": rule_version,
                "event_type": event_type,
            }.items()
            if value
        }
        result = _run_async(retriever.retrieve(query, top_k=top_k, filters=filters))
        return result.model_dump_json()
    except Exception as exc:
        return json.dumps(
            {"success": False, "error_message": str(exc)},
            ensure_ascii=False,
        )
