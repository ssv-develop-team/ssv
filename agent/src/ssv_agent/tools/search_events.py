"""search_events 工具：融合检索入口。"""

from __future__ import annotations

import asyncio
import json
import os
import time
from typing import Annotated

from langchain.tools import tool
from pydantic import Field

from ssv_agent.embedding.registry import get_configured_provider
from ssv_agent.event_store import EventCase, EventLedger
from ssv_agent.event_store.qdrant_store import SsvQdrantStore
from ssv_agent.event_store.sqlite_store import SsvEventStore
from ssv_agent.search.attribute_search import search_attributes
from ssv_agent.search.decompose import decompose_query
from ssv_agent.search.embed_search import search_embeddings
from ssv_agent.search.fusion import fuse
from ssv_agent.search.event_text import build_event_text
from ssv_agent.search.schema import DecomposedQuery, SearchResponse, SearchResultItem

_MAX_TOP_K = 100


def _embedding_backend() -> str:
    return os.getenv("SSV_EMBEDDING_BACKEND", "mock")


def _embedding_model() -> str | None:
    return os.getenv("SSV_EMBEDDING_MODEL") or None


def _current_verdict(case: EventCase) -> str | None:
    if case.review is not None and case.review.get("verdict") is not None:
        return str(case.review["verdict"])
    return case.verdict


def _matches(case: EventCase, query: DecomposedQuery) -> bool:
    if query.source is not None and case.source != query.source:
        return False
    if query.start_ms is not None and case.timestamp_ms < query.start_ms:
        return False
    if query.end_ms is not None and case.timestamp_ms > query.end_ms:
        return False
    if query.event_type is not None and case.event_type != query.event_type:
        return False
    if query.verdict is not None and _current_verdict(case) != query.verdict:
        return False
    if query.class_name is not None and not any(
        detection.get("class_name") == query.class_name
        for detection in case.detections
    ):
        return False
    return True


def _hydrate(
    items: list[SearchResultItem],
    query: DecomposedQuery,
) -> list[SearchResultItem]:
    """只信任 Qdrant 的 event_id/score，其余展示字段回到 SQLite。"""
    hydrated: list[SearchResultItem] = []
    with EventLedger() as ledger:
        for item in items:
            case = ledger.get_case(item.event_id)
            if case is None or not _matches(case, query):
                continue
            review = case.review or {}
            hydrated.append(
                SearchResultItem(
                    event_id=case.event_id,
                    source=case.source,
                    timestamp_ms=case.timestamp_ms,
                    event_type=case.event_type,
                    verdict=review.get("verdict") or case.verdict,
                    confidence=review.get("confidence", case.confidence),
                    score=item.score,
                    match_reason=item.match_reason,
                    snippet=build_event_text(case),
                )
            )
    return hydrated


@tool("search_events", parse_docstring=True)
def search_events_tool(
    query: str,
    source: str | None = None,
    start_ms: int | None = None,
    end_ms: int | None = None,
    event_type: str | None = None,
    verdict: str | None = None,
    class_name: str | None = None,
    top_k: Annotated[int, Field(ge=1, le=_MAX_TOP_K)] = 10,
) -> str:
    """融合检索事件：结构化条件 + 语义相似度。

    Args:
        query: 自然语言查询或事件描述。
        source: 视频源名称过滤。
        start_ms: 起始时间（毫秒时间戳）。
        end_ms: 结束时间（毫秒时间戳）。
        event_type: 事件类型过滤。
        verdict: 复核结论过滤。
        class_name: 检测类别过滤。
        top_k: 最大返回条数。

    Returns:
        JSON 字符串，包含 items、mode 与耗时。
    """

    async def _run() -> SearchResponse:
        started = time.monotonic()
        decomposed = await decompose_query(query, top_k=top_k)
        decomposed = decomposed.model_copy(
            update={
                "source": source if source is not None else decomposed.source,
                "start_ms": start_ms if start_ms is not None else decomposed.start_ms,
                "end_ms": end_ms if end_ms is not None else decomposed.end_ms,
                "event_type": (
                    event_type if event_type is not None else decomposed.event_type
                ),
                "verdict": verdict if verdict is not None else decomposed.verdict,
                "class_name": (
                    class_name if class_name is not None else decomposed.class_name
                ),
                "top_k": top_k,
            }
        )

        attribute_items: list[dict] = []
        with SsvEventStore() as store:
            attribute_items = search_attributes(decomposed, store)

        embed_items: list[dict] = []
        try:
            provider = get_configured_provider(
                _embedding_backend(),
                _embedding_model(),
            )
            candidate_query = decomposed.model_copy(update={"top_k": _MAX_TOP_K})
            with SsvQdrantStore() as qdrant_store:
                embed_items = await search_embeddings(
                    candidate_query,
                    qdrant_store,
                    provider,
                )
        except Exception:
            # 语义路不可用时降级为属性检索。
            embed_items = []

        candidates, mode = fuse(
            attribute_items,
            embed_items,
            top_k=_MAX_TOP_K,
        )
        items = _hydrate(candidates, decomposed)[:top_k]
        return SearchResponse(
            query=query,
            total=len(items),
            items=items,
            mode=mode,
            latency_ms=int((time.monotonic() - started) * 1000),
        )

    try:
        response = asyncio.run(_run())
        return response.model_dump_json()
    except Exception as exc:
        return json.dumps({"error": str(exc)}, ensure_ascii=False)
