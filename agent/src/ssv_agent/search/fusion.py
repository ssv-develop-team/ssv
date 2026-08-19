"""属性检索与语义检索的融合排序。"""

from __future__ import annotations

from typing import Any

from ssv_agent.search.schema import SearchResultItem


def fuse(
    attribute_items: list[dict[str, Any]],
    embed_items: list[dict[str, Any]],
    *,
    attribute_weight: float = 0.5,
    embed_weight: float = 0.5,
    top_k: int = 10,
) -> tuple[list[SearchResultItem], str]:
    """按 event_id 合并两路结果，加权得分后排序。"""
    attr_map = {item["event_id"]: item for item in attribute_items}
    embed_map = {item["event_id"]: item for item in embed_items}

    merged: dict[str, SearchResultItem] = {}
    for event_id in {*attr_map, *embed_map}:
        attr = attr_map.get(event_id)
        embed = embed_map.get(event_id)
        if attr and embed:
            score = attribute_weight * attr["score"] + embed_weight * embed["score"]
            reason = "both"
            base = {**attr}
            base.update({k: v for k, v in embed.items() if not base.get(k)})
        elif attr:
            score = attr["score"]
            reason = "attribute"
            base = attr
        else:
            score = embed["score"]
            reason = "embed"
            base = embed

        merged[event_id] = SearchResultItem(
            event_id=event_id,
            source=base.get("source", ""),
            timestamp_ms=base.get("timestamp_ms", 0),
            event_type=base.get("event_type"),
            verdict=base.get("verdict"),
            confidence=base.get("confidence"),
            score=round(score, 4),
            match_reason=reason,
            snippet=base.get("snippet", ""),
        )

    items = sorted(merged.values(), key=lambda item: item.score, reverse=True)[:top_k]
    if attribute_items and embed_items:
        mode = "fusion"
    elif attribute_items:
        mode = "attribute_only"
    else:
        mode = "embed_only"
    return items, mode
