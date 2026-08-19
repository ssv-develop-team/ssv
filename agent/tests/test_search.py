from __future__ import annotations

import asyncio
import json
from pathlib import Path

import pytest
from pydantic import ValidationError

import ssv_agent.tools.search_events as search_events_module
from ssv_agent.embedding.registry import get_provider
from ssv_agent.event_store.qdrant_store import SsvQdrantStore
from ssv_agent.event_store.sqlite_store import SsvEventStore
from ssv_agent.search.attribute_search import search_attributes
from ssv_agent.search.decompose import decompose_query
from ssv_agent.search.embed_search import search_embeddings
from ssv_agent.search.fusion import fuse
from ssv_agent.search.schema import DecomposedQuery
from ssv_agent.tools.search_events import search_events_tool


def test_decompose_fallback() -> None:
    result = asyncio.run(decompose_query("谁没戴安全帽", top_k=5))

    assert result.query_text == "谁没戴安全帽"
    assert result.top_k == 5
    assert result.source is None


def _seed_store(store: SsvEventStore) -> None:
    store.insert_event(
        "1-0",
        source="camera-1",
        timestamp_ms=1000,
        frame_id=1,
        event_type="helmet_violation",
        verdict="violation",
    )
    store.insert_detections(
        "1-0",
        [{"class_name": "person", "class_id": 0, "confidence": 0.9}],
    )


def test_attribute_search(tmp_path: Path) -> None:
    with SsvEventStore(tmp_path / "events.db") as store:
        _seed_store(store)
        items = search_attributes(
            DecomposedQuery(query_text="x", source="camera-1", top_k=10),
            store,
        )

    assert len(items) == 1
    assert items[0]["score"] == 1.0
    assert items[0]["match_reason"] == "attribute"


def test_embed_search(tmp_path: Path) -> None:
    provider = get_provider("mock")
    with SsvEventStore(tmp_path / "events.db") as store:
        _seed_store(store)
    with SsvQdrantStore(tmp_path / "qdrant") as qdrant:
        provider = get_provider("mock")
        vector = asyncio.run(provider.embed_query("helmet_violation"))
        qdrant.upsert_event_vector(
            "1-0",
            vector,
            {
                "source": "stale-camera",
                "timestamp_ms": 1000,
                "event_type": "helmet_violation",
                "verdict": "compliant",
            },
        )
        items = asyncio.run(
            search_embeddings(
                DecomposedQuery(
                    query_text="helmet_violation",
                    source="camera-1",
                    verdict="violation",
                    top_k=10,
                ),
                qdrant,
                provider,
            )
        )

    assert len(items) == 1
    assert items == [
        {
            "event_id": "1-0",
            "score": pytest.approx(1.0),
            "match_reason": "embed",
        }
    ]


def test_fusion_merge_and_dedupe() -> None:
    attr = [
        {
            "event_id": "1-0",
            "source": "camera-1",
            "timestamp_ms": 1000,
            "score": 1.0,
            "snippet": "a",
        },
        {
            "event_id": "2-0",
            "source": "camera-2",
            "timestamp_ms": 2000,
            "score": 1.0,
            "snippet": "b",
        },
    ]
    embed = [
        {
            "event_id": "1-0",
            "source": "camera-1",
            "timestamp_ms": 1000,
            "score": 0.8,
            "snippet": "a",
        }
    ]

    items, mode = fuse(attr, embed, top_k=10)

    assert mode == "fusion"
    assert len(items) == 2
    by_id = {item.event_id: item for item in items}
    assert by_id["1-0"].score == 0.9
    assert by_id["1-0"].match_reason == "both"
    assert by_id["2-0"].match_reason == "attribute"


def test_search_events_tool(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("SSV_EVENT_DB_PATH", str(tmp_path / "events.db"))
    monkeypatch.setenv("SSV_QDRANT_PATH", str(tmp_path / "qdrant"))
    monkeypatch.setenv("SSV_EMBEDDING_BACKEND", "mock")

    with SsvEventStore() as store:
        _seed_store(store)
    provider = get_provider("mock")
    vector = asyncio.run(provider.embed_query("helmet_violation"))
    with SsvQdrantStore() as qdrant:
        qdrant.upsert_event_vector(
            "1-0",
            vector,
            {
                "source": "camera-1",
                "timestamp_ms": 1000,
                "event_type": "helmet_violation",
                "verdict": "violation",
            },
        )

    out = json.loads(
        search_events_tool.invoke(
            {"query": "helmet_violation", "source": "camera-1"}
        )
    )

    assert out["total"] == 1
    assert out["items"][0]["event_id"] == "1-0"
    assert out["mode"] in ("fusion", "attribute_only")


def test_search_events_uses_backend_and_model_for_provider(
    tmp_path: Path,
    monkeypatch,
) -> None:
    monkeypatch.setenv("SSV_EVENT_DB_PATH", str(tmp_path / "events.db"))
    monkeypatch.setenv("SSV_QDRANT_PATH", str(tmp_path / "qdrant"))
    monkeypatch.delenv("SSV_QDRANT_URL", raising=False)
    monkeypatch.setenv("SSV_EMBEDDING_BACKEND", "mock")
    calls: list[tuple[str, str | None]] = []

    def configured_provider(backend: str, model: str | None = None):
        calls.append((backend, model))
        return get_provider("mock")

    monkeypatch.setattr(
        search_events_module,
        "get_configured_provider",
        configured_provider,
        raising=False,
    )
    with SsvEventStore() as store:
        _seed_store(store)

    monkeypatch.setenv("SSV_EMBEDDING_MODEL", "model-a")
    json.loads(search_events_tool.invoke({"query": "helmet"}))
    monkeypatch.setenv("SSV_EMBEDDING_MODEL", "model-b")
    json.loads(search_events_tool.invoke({"query": "helmet"}))

    assert calls == [("mock", "model-a"), ("mock", "model-b")]


@pytest.mark.parametrize(
    ("filters", "payload_update"),
    [
        ({"source": "payload-camera"}, {"source": "payload-camera"}),
        ({"start_ms": 1_001}, {"timestamp_ms": 2_000}),
        ({"end_ms": 999}, {"timestamp_ms": 500}),
        ({"event_type": "payload-type"}, {"event_type": "payload-type"}),
        ({"verdict": "compliant"}, {"verdict": "compliant"}),
        ({"class_name": "vehicle"}, {"class_name": "vehicle"}),
    ],
    ids=["source", "start", "end", "event-type", "verdict", "class-name"],
)
def test_search_events_rejects_payload_match_when_sqlite_does_not_match(
    tmp_path: Path,
    monkeypatch,
    filters: dict[str, object],
    payload_update: dict[str, object],
) -> None:
    monkeypatch.setenv("SSV_EVENT_DB_PATH", str(tmp_path / "events.db"))
    monkeypatch.setenv("SSV_QDRANT_PATH", str(tmp_path / "qdrant"))
    monkeypatch.delenv("SSV_QDRANT_URL", raising=False)
    monkeypatch.setenv("SSV_EMBEDDING_BACKEND", "mock")
    monkeypatch.setattr(search_events_module, "search_attributes", lambda *_: [])

    with SsvEventStore() as store:
        _seed_store(store)
    provider = get_provider("mock")
    vector = asyncio.run(provider.embed_query("helmet_violation"))
    payload = {
        "source": "camera-1",
        "timestamp_ms": 1_000,
        "event_type": "helmet_violation",
        "verdict": "violation",
        "class_name": "person",
    }
    payload.update(payload_update)
    with SsvQdrantStore() as qdrant:
        qdrant.upsert_event_vector("1-0", vector, payload)

    out = json.loads(
        search_events_tool.invoke({"query": "helmet_violation", **filters})
    )

    assert out["total"] == 0


def test_search_events_hydrates_stale_qdrant_hit_from_sqlite(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("SSV_EVENT_DB_PATH", str(tmp_path / "events.db"))
    monkeypatch.setenv("SSV_QDRANT_PATH", str(tmp_path / "qdrant"))
    monkeypatch.delenv("SSV_QDRANT_URL", raising=False)
    monkeypatch.setenv("SSV_EMBEDDING_BACKEND", "mock")
    monkeypatch.setattr(
        "ssv_agent.tools.search_events.search_attributes",
        lambda *_: [],
    )

    with SsvEventStore() as store:
        _seed_store(store)
    provider = get_provider("mock")
    vector = asyncio.run(provider.embed_query("helmet_violation"))
    with SsvQdrantStore() as qdrant:
        qdrant.upsert_event_vector(
            "1-0",
            vector,
            {
                "source": "stale-camera",
                "timestamp_ms": 1,
                "event_type": "stale_type",
                "verdict": "compliant",
            },
        )

    out = json.loads(
        search_events_tool.invoke(
            {
                "query": "helmet_violation",
                "source": "camera-1",
                "start_ms": 900,
                "end_ms": 1_100,
                "event_type": "helmet_violation",
                "verdict": "violation",
                "class_name": "person",
            }
        )
    )

    assert out["total"] == 1
    assert out["items"][0]["source"] == "camera-1"
    assert out["items"][0]["event_type"] == "helmet_violation"
    assert out["items"][0]["verdict"] == "violation"


def test_search_events_applies_top_k_after_authoritative_filtering(
    tmp_path: Path,
    monkeypatch,
) -> None:
    monkeypatch.setenv("SSV_EVENT_DB_PATH", str(tmp_path / "events.db"))
    monkeypatch.setenv("SSV_QDRANT_PATH", str(tmp_path / "qdrant"))
    monkeypatch.delenv("SSV_QDRANT_URL", raising=False)
    monkeypatch.setenv("SSV_EMBEDDING_BACKEND", "mock")
    monkeypatch.setattr(search_events_module, "search_attributes", lambda *_: [])

    with SsvEventStore() as store:
        store.insert_event(
            "high-score-wrong",
            source="wrong-camera",
            timestamp_ms=2_000,
            frame_id=2,
            event_type="helmet_violation",
            verdict="violation",
        )
        store.insert_event(
            "lower-score-right",
            source="target-camera",
            timestamp_ms=1_000,
            frame_id=1,
            event_type="helmet_violation",
            verdict="violation",
        )

    provider = get_provider("mock")
    query_vector = asyncio.run(provider.embed_query("helmet_violation"))
    with SsvQdrantStore() as qdrant:
        qdrant.upsert_event_vector(
            "high-score-wrong",
            query_vector,
            {"source": "target-camera"},
        )
        qdrant.upsert_event_vector(
            "lower-score-right",
            [-value for value in query_vector],
            {"source": "stale-camera"},
        )

    out = json.loads(
        search_events_tool.invoke(
            {
                "query": "helmet_violation",
                "source": "target-camera",
                "top_k": 1,
            }
        )
    )

    assert out["total"] == 1
    assert out["items"][0]["event_id"] == "lower-score-right"


@pytest.mark.parametrize("top_k", [0, 101])
def test_search_events_rejects_top_k_outside_public_bounds(top_k: int) -> None:
    assert search_events_tool.args_schema is not None

    with pytest.raises(ValidationError):
        search_events_tool.args_schema.model_validate(
            {"query": "helmet_violation", "top_k": top_k}
        )


def test_search_events_falls_back_to_attributes_when_semantic_search_fails(
    tmp_path: Path,
    monkeypatch,
) -> None:
    monkeypatch.setenv("SSV_EVENT_DB_PATH", str(tmp_path / "events.db"))
    monkeypatch.setenv("SSV_QDRANT_PATH", str(tmp_path / "qdrant"))
    monkeypatch.delenv("SSV_QDRANT_URL", raising=False)
    with SsvEventStore() as store:
        _seed_store(store)

    def unavailable_provider(*_args: object, **_kwargs: object):
        raise RuntimeError("embedding unavailable")

    monkeypatch.setattr(
        search_events_module,
        "get_configured_provider",
        unavailable_provider,
    )

    out = json.loads(
        search_events_tool.invoke(
            {"query": "helmet_violation", "source": "camera-1"}
        )
    )

    assert out["total"] == 1
    assert out["items"][0]["event_id"] == "1-0"
    assert out["mode"] == "attribute_only"


def test_search_events_filters_by_current_sqlite_review_projection(
    tmp_path: Path,
    monkeypatch,
) -> None:
    monkeypatch.setenv("SSV_EVENT_DB_PATH", str(tmp_path / "events.db"))
    monkeypatch.setenv("SSV_QDRANT_PATH", str(tmp_path / "qdrant"))
    monkeypatch.delenv("SSV_QDRANT_URL", raising=False)
    monkeypatch.setenv("SSV_EMBEDDING_BACKEND", "mock")
    monkeypatch.setattr(search_events_module, "search_attributes", lambda *_: [])
    with SsvEventStore() as store:
        _seed_store(store)
        store.insert_result(
            "1-0",
            verdict="compliant",
            confidence=0.95,
            evidence_status="available",
            explanation="Current review supersedes the indexed payload.",
        )

    provider = get_provider("mock")
    vector = asyncio.run(provider.embed_query("helmet_violation"))
    with SsvQdrantStore() as qdrant:
        qdrant.upsert_event_vector(
            "1-0",
            vector,
            {"verdict": "violation"},
        )

    current = json.loads(
        search_events_tool.invoke(
            {"query": "helmet_violation", "verdict": "compliant"}
        )
    )
    stale = json.loads(
        search_events_tool.invoke(
            {"query": "helmet_violation", "verdict": "violation"}
        )
    )

    assert current["total"] == 1
    assert current["items"][0]["verdict"] == "compliant"
    assert stale["total"] == 0
