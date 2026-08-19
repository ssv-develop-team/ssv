"""融合检索的数据模型。"""

from __future__ import annotations

from pydantic import BaseModel


class DecomposedQuery(BaseModel):
    """自然语言查询分解结果。"""

    query_text: str
    source: str | None = None
    start_ms: int | None = None
    end_ms: int | None = None
    event_type: str | None = None
    verdict: str | None = None
    class_name: str | None = None
    top_k: int = 10


class SearchResultItem(BaseModel):
    """一条融合检索结果。"""

    event_id: str
    source: str
    timestamp_ms: int
    event_type: str | None = None
    verdict: str | None = None
    confidence: float | None = None
    score: float
    match_reason: str
    snippet: str


class SearchResponse(BaseModel):
    """一次融合检索的响应。"""

    query: str
    total: int
    items: list[SearchResultItem]
    mode: str
    latency_ms: int
