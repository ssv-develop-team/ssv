"""get_review_result 工具：查询复核结果。"""

from __future__ import annotations

import json

from langchain.tools import tool

from ssv_agent.event_store.sqlite_store import SsvEventStore


@tool("get_review_result", parse_docstring=True)
def get_review_result_tool(event_id: str) -> str:
    """按 event_id 查询复核结果。

    Args:
        event_id: 事件 ID（Redis Stream entry id）。

    Returns:
        JSON 字符串；未找到时返回 found=false。
    """
    with SsvEventStore() as store:
        result = store.get_result(event_id)
    if result is None:
        return json.dumps({"found": False, "event_id": event_id}, ensure_ascii=False)
    return json.dumps({"found": True, **result}, ensure_ascii=False)
