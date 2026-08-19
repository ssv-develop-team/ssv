"""基于 SQLite 的结构化检索。"""

from __future__ import annotations

from typing import Any

from ssv_agent.event_store.schema import EventQuery
from ssv_agent.event_store.sqlite_store import SsvEventStore
from ssv_agent.search.event_text import build_event_text
from ssv_agent.search.schema import DecomposedQuery


def search_attributes(
    query: DecomposedQuery,
    store: SsvEventStore,
) -> list[dict[str, Any]]:
    """按结构化条件查询事件，精确命中给基础分 1.0。"""
    event_query = EventQuery(
        source=query.source,
        start_ms=query.start_ms,
        end_ms=query.end_ms,
        event_type=query.event_type,
        verdict=query.verdict,
        class_name=query.class_name,
        limit=query.top_k,
    )
    rows = store.query_events(event_query)
    return [
        {
            "event_id": row["event_id"],
            "source": row["source"],
            "timestamp_ms": row["timestamp_ms"],
            "event_type": row.get("event_type"),
            "verdict": row.get("verdict"),
            "confidence": row.get("confidence"),
            "score": 1.0,
            "match_reason": "attribute",
            "snippet": build_event_text(row),
        }
        for row in rows
    ]
