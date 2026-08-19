"""基于 Qdrant 的语义检索。"""

from __future__ import annotations

from typing import Any

from ssv_agent.embedding.provider import EmbeddingProvider
from ssv_agent.event_store.qdrant_store import SsvQdrantStore
from ssv_agent.search.schema import DecomposedQuery


async def search_embeddings(
    query: DecomposedQuery,
    qdrant_store: SsvQdrantStore,
    provider: EmbeddingProvider,
) -> list[dict[str, Any]]:
    """用 Qdrant 召回 event_id/score 候选，不信任 payload 事实字段。"""
    vector = await provider.embed_query(query.query_text)
    hits = qdrant_store.search_events(
        vector,
        top_k=min(max(query.top_k, 1), 100),
    )
    return [
        {
            "event_id": hit["event_id"],
            "score": float(hit["score"]),
            "match_reason": "embed",
        }
        for hit in hits
        if isinstance(hit.get("event_id"), str) and "score" in hit
    ]
