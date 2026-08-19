"""list_events 工具：按条件筛选事件列表。"""

from __future__ import annotations

import json

from langchain.tools import tool

from ssv_agent.event_store.schema import EventQuery
from ssv_agent.event_store.sqlite_store import SsvEventStore


@tool("list_events", parse_docstring=True)
def list_events_tool(
    source: str | None = None,
    start_ms: int | None = None,
    end_ms: int | None = None,
    event_type: str | None = None,
    status: str | None = None,
    verdict: str | None = None,
    class_name: str | None = None,
    limit: int = 20,
) -> str:
    """按条件筛选事件，返回按时间倒序的事件列表。

    Args:
        source: 视频源名称。
        start_ms: 起始时间（毫秒时间戳）。
        end_ms: 结束时间（毫秒时间戳）。
        event_type: 事件类型。
        status: 事件状态。
        verdict: 复核结论。
        class_name: 检测类别名。
        limit: 最大返回条数，1-100。

    Returns:
        JSON 字符串，包含 total 与 items。
    """
    query = EventQuery(
        source=source,
        start_ms=start_ms,
        end_ms=end_ms,
        event_type=event_type,
        status=status,
        verdict=verdict,
        class_name=class_name,
        limit=limit,
    )
    with SsvEventStore() as store:
        items = store.query_events(query)
    return json.dumps({"total": len(items), "items": items}, ensure_ascii=False)
