"""独立知识向量与文档处理记录，不使用规则整库替换语义。"""

from __future__ import annotations

import os
import uuid
from dataclasses import asdict
from pathlib import Path
from typing import Any

from qdrant_client import QdrantClient, models

from ssv_agent.embedding.registry import EmbeddingIdentity
from ssv_agent.event_store.qdrant_store import derive_physical_collection_name
from ssv_agent.knowledge.documents import ChunkingConfig, KnowledgeDocument, KnowledgePassage

DEFAULT_KNOWLEDGE_PATH = Path(__file__).resolve().parents[3] / "data" / "knowledge_qdrant"


def _point_id(identifier: str) -> str:
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ssv-knowledge:{identifier}"))


def _passage_payload(passage: KnowledgePassage) -> dict[str, Any]:
    payload = asdict(passage)
    payload.pop("embedding_content")
    if not passage.table_name:
        payload.pop("table_name")
    if not passage.table_headers:
        payload.pop("table_headers")
    return payload


class KnowledgeVectorStore:
    def __init__(
        self,
        identity: EmbeddingIdentity,
        *,
        path: str | Path | None = None,
        url: str | None = None,
        api_key: str | None = None,
    ) -> None:
        if path is not None and url is not None:
            raise ValueError("path and url are mutually exclusive")
        self.collection = derive_physical_collection_name("ssv_knowledge", identity)
        self.documents_collection = derive_physical_collection_name(
            "ssv_knowledge_documents", identity
        )
        resolved_url = url if url is not None else os.getenv("SSV_QDRANT_URL")
        if path is None and resolved_url:
            self._client = QdrantClient(
                url=resolved_url,
                api_key=api_key if api_key is not None else os.getenv("SSV_QDRANT_API_KEY"),
            )
        else:
            resolved_path = Path(path or os.getenv("SSV_QDRANT_PATH") or DEFAULT_KNOWLEDGE_PATH)
            resolved_path = resolved_path.expanduser().resolve()
            resolved_path.mkdir(parents=True, exist_ok=True)
            self._client = QdrantClient(path=str(resolved_path))

    def close(self) -> None:
        self._client.close()

    def get_document(self, document_id: str) -> dict[str, Any] | None:
        if not self._client.collection_exists(self.documents_collection):
            return None
        records = self._client.retrieve(
            self.documents_collection, ids=[_point_id(document_id)], with_payload=True
        )
        return dict(records[0].payload or {}) if records else None

    def _write_document(self, payload: dict[str, Any]) -> None:
        if not self._client.collection_exists(self.documents_collection):
            self._client.create_collection(self.documents_collection, vectors_config={})
        self._client.upsert(
            self.documents_collection,
            points=[
                models.PointStruct(id=_point_id(payload["document_id"]), vector={}, payload=payload)
            ],
            wait=True,
        )

    def _ensure_vectors(self, dimension: int) -> None:
        if not self._client.collection_exists(self.collection):
            self._client.create_collection(
                self.collection,
                vectors_config=models.VectorParams(size=dimension, distance=models.Distance.COSINE),
            )
        config = self._client.get_collection(self.collection).config.params.vectors
        if not isinstance(config, models.VectorParams) or config.size != dimension:
            raise ValueError("knowledge vector dimension does not match the existing index")

    def write(
        self,
        document: KnowledgeDocument,
        passages: list[KnowledgePassage],
        vectors: list[list[float]],
        config: ChunkingConfig,
    ) -> None:
        self._ensure_vectors(len(vectors[0]))
        record = {
            **asdict(document),
            "processing_hash": config.processing_hash,
            "chunking": asdict(config),
            "chunks_count": len(passages),
            "status": "indexing",
        }
        # 先撤销完成标记，全部片段和清理都成功后再标记完成，失败可幂等重试。
        self._write_document(record)
        points = [
            models.PointStruct(
                id=_point_id(passage.chunk_id), vector=vector, payload=_passage_payload(passage)
            )
            for passage, vector in zip(passages, vectors, strict=True)
        ]
        for start in range(0, len(points), 128):
            self._client.upsert(self.collection, points=points[start : start + 128], wait=True)
        self._client.delete(
            self.collection,
            points_selector=models.FilterSelector(
                filter=models.Filter(
                    must=[
                        models.FieldCondition(
                            key="document_id", match=models.MatchValue(value=document.document_id)
                        )
                    ],
                    must_not=[models.HasIdCondition(has_id=[point.id for point in points])],
                )
            ),
            wait=True,
        )
        self._write_document({**record, "status": "completed"})

    def search(
        self,
        vector: list[float],
        *,
        top_k: int,
        min_score: float | None,
        document_id: str | None,
    ) -> list[dict[str, Any]]:
        if not self._client.collection_exists(self.collection):
            return []
        self._ensure_vectors(len(vector))
        query_filter = None
        if document_id is not None:
            query_filter = models.Filter(
                must=[
                    models.FieldCondition(
                        key="document_id", match=models.MatchValue(value=document_id)
                    )
                ]
            )
        response = self._client.query_points(
            self.collection,
            query=vector,
            limit=top_k,
            score_threshold=min_score,
            query_filter=query_filter,
            with_payload=True,
        )
        return [{**(point.payload or {}), "score": point.score} for point in response.points]
