from __future__ import annotations

import asyncio
from pathlib import Path

import pytest
from qdrant_client import QdrantClient

from ssv_agent.embedding.provider import EmbeddingProvider
from ssv_agent.embedding.registry import EmbeddingIdentity
from ssv_agent.event_store.qdrant_store import derive_physical_collection_name
from ssv_agent.knowledge.base import KnowledgeBase, KnowledgeError
from ssv_agent.knowledge.documents import ChunkingConfig


class RecordingEmbedding(EmbeddingProvider):
    """固定测试向量，仅验证检索链路，不代表语言模型的语义能力。"""

    backend_name = "test"

    def __init__(self) -> None:
        self.calls: list[list[str]] = []
        self.failure: Exception | None = None
        self.invalid: list[list[float]] | None = None

    async def embed_texts(self, texts: list[str]) -> list[list[float]]:
        self.calls.append(texts)
        if self.failure:
            raise self.failure
        if self.invalid is not None:
            return self.invalid
        return [[1.0, 0.0] if "电缆" in text else [0.0, 1.0] for text in texts]

    async def embed_query(self, query: str) -> list[float]:
        return [1.0, 0.0] if "电缆" in query else [0.0, 1.0]


IDENTITY = EmbeddingIdentity(schema_version=1, backend="test", model="recording-v1")


def test_append_persist_deduplicate_filter_and_rechunk(tmp_path: Path) -> None:
    path = tmp_path / "qdrant"
    provider = RecordingEmbedding()
    with KnowledgeBase(path=path, provider=provider, embedding_identity=IDENTITY) as knowledge:
        first = asyncio.run(knowledge.ingest_text("电缆每周检查。", source="notes"))
        second = asyncio.run(knowledge.ingest_text("茶水间在二楼。", source="location"))
        duplicate = asyncio.run(knowledge.ingest_text("电缆每周检查。", source="notes"))
        assert duplicate.skipped and duplicate.document_id == first.document_id
        assert len(provider.calls) == 2
        assert len(asyncio.run(knowledge.search("电缆", min_score=0.8))) == 1
        assert (
            asyncio.run(knowledge.search("电缆", document_id=second.document_id, min_score=0.8))
            == []
        )
        assert len(asyncio.run(knowledge.search("anything"))) == 2

    with KnowledgeBase(
        path=path,
        provider=provider,
        embedding_identity=IDENTITY,
        chunking=ChunkingConfig(4, 0),
    ) as reopened:
        assert len(asyncio.run(reopened.search("anything"))) == 2
        updated = asyncio.run(reopened.ingest_text("电缆每周检查。", source="notes"))
        assert updated.document_id == first.document_id and not updated.skipped
        assert updated.chunks_count == 2
        hits = asyncio.run(reopened.search("电缆", document_id=first.document_id))
        assert len(hits) == 2
        assert sorted(hit["chunk_index"] for hit in hits) == [0, 1]
        assert len(asyncio.run(reopened.search("anything"))) == 3
    client = QdrantClient(path=str(path))
    try:
        docs = client.scroll(derive_physical_collection_name("ssv_knowledge_documents", IDENTITY))[
            0
        ]
        assert len(docs) == 2
        assert all(doc.payload["status"] == "completed" for doc in docs)
        assert any(doc.payload["content"] == "电缆每周检查。" for doc in docs)
    finally:
        client.close()


def test_changed_content_is_new_document_and_file_input(tmp_path: Path) -> None:
    file = tmp_path / "notes.md"
    file.write_text("# 电缆\n检查绝缘。", encoding="utf-8")
    with KnowledgeBase(
        path=tmp_path / "db", provider=RecordingEmbedding(), embedding_identity=IDENTITY
    ) as knowledge:
        first = asyncio.run(knowledge.ingest_file(file))
        file.write_text("# 电缆\n每周检查绝缘。", encoding="utf-8")
        second = asyncio.run(knowledge.ingest_file(file))
        assert first.document_id != second.document_id
        hits = asyncio.run(knowledge.search("电缆"))
        assert len(hits) == 2
        assert all(hit["source"] == str(file.resolve()) for hit in hits)


def test_batching_and_embedding_failure_preserve_old_knowledge(tmp_path: Path) -> None:
    provider = RecordingEmbedding()
    with KnowledgeBase(
        path=tmp_path,
        provider=provider,
        embedding_identity=IDENTITY,
        chunking=ChunkingConfig(4, 0),
    ) as knowledge:
        asyncio.run(knowledge.ingest_text("甲" * 84))
        assert [len(batch) for batch in provider.calls] == [20, 1]
        provider.failure = RuntimeError("provider unavailable")
        with pytest.raises(KnowledgeError, match="provider unavailable") as error:
            asyncio.run(knowledge.ingest_text("电缆"))
        assert error.value.stage == "embedding"
        assert len(asyncio.run(knowledge.search("anything", top_k=100))) == 21


@pytest.mark.parametrize("invalid", [[], [[]], [[0.0, 0.0]], [[float("nan"), 1]], [[float("inf")]]])
def test_bad_vectors_never_mark_document_complete(
    tmp_path: Path, invalid: list[list[float]]
) -> None:
    provider = RecordingEmbedding()
    provider.invalid = invalid
    with KnowledgeBase(path=tmp_path, provider=provider, embedding_identity=IDENTITY) as knowledge:
        with pytest.raises(KnowledgeError) as error:
            asyncio.run(knowledge.ingest_text("电缆"))
        assert error.value.stage == "embedding"
        assert asyncio.run(knowledge.search("电缆")) == []
        provider.invalid = None
        assert not asyncio.run(knowledge.ingest_text("电缆")).skipped


def test_dimension_mismatch_across_batches_and_query(tmp_path: Path) -> None:
    class ChangingEmbedding(RecordingEmbedding):
        async def embed_texts(self, texts: list[str]) -> list[list[float]]:
            self.calls.append(texts)
            return [[1.0] * (2 if len(self.calls) == 1 else 3) for _ in texts]

    with KnowledgeBase(
        path=tmp_path,
        provider=ChangingEmbedding(),
        embedding_identity=IDENTITY,
        chunking=ChunkingConfig(1, 0),
    ) as knowledge:
        with pytest.raises(KnowledgeError, match="same-sized"):
            asyncio.run(knowledge.ingest_text("甲" * 21))
        assert asyncio.run(knowledge.search("甲")) == []


def test_query_vector_validation_and_existing_dimension(tmp_path: Path) -> None:
    class QueryEmbedding(RecordingEmbedding):
        def __init__(self) -> None:
            super().__init__()
            self.query_vector = [1.0, 0.0]

        async def embed_query(self, query: str) -> list[float]:
            return self.query_vector

    provider = QueryEmbedding()
    with KnowledgeBase(path=tmp_path, provider=provider, embedding_identity=IDENTITY) as knowledge:
        asyncio.run(knowledge.ingest_text("电缆"))
        for vector in ([], [0.0, 0.0], [float("nan"), 1.0]):
            provider.query_vector = vector
            with pytest.raises(KnowledgeError) as error:
                asyncio.run(knowledge.search("电缆"))
            assert error.value.stage == "embedding"
        provider.query_vector = [1.0, 0.0, 0.0]
        with pytest.raises(KnowledgeError, match="dimension") as error:
            asyncio.run(knowledge.search("电缆"))
        assert error.value.stage == "storage"


@pytest.mark.parametrize(
    "kwargs",
    [
        {"query": " "},
        {"top_k": 0},
        {"top_k": True},
        {"min_score": float("nan")},
        {"min_score": 1.1},
        {"min_score": True},
        {"document_id": ""},
    ],
)
def test_invalid_queries(tmp_path: Path, kwargs: dict) -> None:
    with KnowledgeBase(
        path=tmp_path, provider=RecordingEmbedding(), embedding_identity=IDENTITY
    ) as knowledge:
        with pytest.raises(KnowledgeError) as error:
            asyncio.run(knowledge.search(**{"query": "test", **kwargs}))
        assert error.value.stage == "input"


def test_identity_isolation_and_closed_lifecycle(tmp_path: Path) -> None:
    with KnowledgeBase(
        path=tmp_path, provider=RecordingEmbedding(), embedding_identity=IDENTITY
    ) as first:
        asyncio.run(first.ingest_text("电缆"))
    with pytest.raises(KnowledgeError, match="closed"):
        asyncio.run(first.search("电缆"))
    first.close()
    another = EmbeddingIdentity(schema_version=1, backend="test", model="recording-v2")
    with KnowledgeBase(
        path=tmp_path, provider=RecordingEmbedding(), embedding_identity=another
    ) as second:
        assert asyncio.run(second.search("电缆")) == []


def test_reject_mock_and_require_identity_for_injection(tmp_path: Path) -> None:
    with pytest.raises(KnowledgeError, match="real embedding"):
        KnowledgeBase(path=tmp_path, embedding_backend="mock")
    with pytest.raises(ValueError, match="explicit embedding_identity"):
        KnowledgeBase(path=tmp_path, provider=RecordingEmbedding())


def test_storage_failure_is_retriable(tmp_path: Path, monkeypatch) -> None:
    with KnowledgeBase(
        path=tmp_path, provider=RecordingEmbedding(), embedding_identity=IDENTITY
    ) as knowledge:
        asyncio.run(knowledge.ingest_text("原有知识"))
        original = QdrantClient.upsert
        failed = False

        def fail_once(client, collection_name, *args, **kwargs):
            nonlocal failed
            if collection_name.startswith("ssv_knowledge__") and not failed:
                failed = True
                raise RuntimeError("storage unavailable")
            return original(client, collection_name, *args, **kwargs)

        monkeypatch.setattr(QdrantClient, "upsert", fail_once)
        with pytest.raises(KnowledgeError, match="storage unavailable") as error:
            asyncio.run(knowledge.ingest_text("电缆"))
        assert error.value.stage == "storage"
        assert len(asyncio.run(knowledge.search("anything"))) == 1
        assert not asyncio.run(knowledge.ingest_text("电缆")).skipped
        assert asyncio.run(knowledge.ingest_text("电缆")).skipped
        assert len(asyncio.run(knowledge.search("anything"))) == 2


def test_partial_write_can_resume_after_reopening(tmp_path: Path, monkeypatch) -> None:
    original = QdrantClient.upsert
    writes = 0

    def fail_second_vector_batch(client, collection_name, *args, **kwargs):
        nonlocal writes
        if collection_name.startswith("ssv_knowledge__"):
            writes += 1
            if writes == 2:
                raise RuntimeError("second batch unavailable")
        return original(client, collection_name, *args, **kwargs)

    monkeypatch.setattr(QdrantClient, "upsert", fail_second_vector_batch)
    config = ChunkingConfig(1, 0)
    with KnowledgeBase(
        path=tmp_path,
        provider=RecordingEmbedding(),
        embedding_identity=IDENTITY,
        chunking=config,
    ) as knowledge:
        with pytest.raises(KnowledgeError, match="second batch"):
            asyncio.run(knowledge.ingest_text("甲" * 129))

    with KnowledgeBase(
        path=tmp_path,
        provider=RecordingEmbedding(),
        embedding_identity=IDENTITY,
        chunking=config,
    ) as reopened:
        result = asyncio.run(reopened.ingest_text("甲" * 129))
        assert not result.skipped and result.chunks_count == 129
        assert asyncio.run(reopened.ingest_text("甲" * 129)).skipped

    client = QdrantClient(path=str(tmp_path))
    try:
        assert client.count(derive_physical_collection_name("ssv_knowledge", IDENTITY)).count == 129
    finally:
        client.close()


def test_heading_context_reaches_embedding_but_result_keeps_original(tmp_path: Path) -> None:
    provider = RecordingEmbedding()
    text = "# 服务指南\n\n## 采购\n\n找林悦。\n\n## 维修\n\n找陈墨。"
    with KnowledgeBase(path=tmp_path, provider=provider, embedding_identity=IDENTITY) as knowledge:
        result = asyncio.run(knowledge.ingest_text(text))
        assert result.chunks_count == 2
        assert provider.calls[0][1].startswith("服务指南 > 维修\n\n## 维修")
        hits = asyncio.run(knowledge.search("联系人"))
        repair = next(hit for hit in hits if "找陈墨" in hit["content"])
        assert repair["content"] == "## 维修\n\n找陈墨。"
        assert repair["heading_path"] == ["服务指南", "维修"]
        assert "embedding_text" not in repair


def test_changing_strategy_reindexes_same_document_and_cleans_old_chunks(tmp_path: Path) -> None:
    provider = RecordingEmbedding()
    text = "# 采购\n找林悦。\n# 维修\n找陈墨。"
    with KnowledgeBase(
        path=tmp_path,
        provider=provider,
        embedding_identity=IDENTITY,
        chunking=ChunkingConfig(strategy="text"),
    ) as legacy:
        original = asyncio.run(legacy.ingest_text(text))
        assert original.chunks_count == 1
        assert asyncio.run(legacy.ingest_text(text)).skipped
    with KnowledgeBase(path=tmp_path, provider=provider, embedding_identity=IDENTITY) as improved:
        current = asyncio.run(improved.ingest_text(text))
        assert current.document_id == original.document_id
        assert not current.skipped and current.chunks_count == 2
        assert len(asyncio.run(improved.search("联系人"))) == 2
        assert asyncio.run(improved.ingest_text(text)).skipped
    assert len(provider.calls) == 2


def test_structured_table_metadata_and_internal_text_are_separate(tmp_path: Path) -> None:
    provider = RecordingEmbedding()
    text = "# 管道\n表2 颜色\n|物质|颜色|\n|---|---|\n|氧|淡蓝|\n"
    with KnowledgeBase(
        path=tmp_path,
        provider=provider,
        embedding_identity=IDENTITY,
        chunking=ChunkingConfig(strategy="structured"),
    ) as knowledge:
        asyncio.run(knowledge.ingest_text(text))
        assert "物质：氧" in provider.calls[0][0]
        hits = asyncio.run(knowledge.search("氧"))
        assert hits[0]["content"] == "|氧|淡蓝|\n"
        assert hits[0]["table_name"] == "表2 颜色"
        assert tuple(hits[0]["table_headers"]) == ("物质", "颜色")
        assert "embedding_content" not in hits[0]


def test_structured_noise_only_input_is_rejected_before_embedding(tmp_path: Path) -> None:
    provider = RecordingEmbedding()
    with KnowledgeBase(
        path=tmp_path,
        provider=provider,
        embedding_identity=IDENTITY,
        chunking=ChunkingConfig(strategy="structured"),
    ) as knowledge:
        with pytest.raises(KnowledgeError, match="no searchable text") as error:
            asyncio.run(knowledge.ingest_text("![](page.png)\n<!-- PDF 第 1 页 -->\n"))
        assert error.value.stage == "input"
        assert provider.calls == []
