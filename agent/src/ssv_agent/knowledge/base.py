"""自由文本和纯文本文件的独立入库、去重与向量检索接口。"""

from __future__ import annotations

import asyncio
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Self

from ssv_agent.embedding.provider import EmbeddingProvider
from ssv_agent.embedding.registry import (
    EmbeddingIdentity,
    get_configured_provider,
    resolve_embedding_identity,
)
from ssv_agent.knowledge.documents import ChunkingConfig, KnowledgeDocument, split_document
from ssv_agent.knowledge.vector_store import KnowledgeVectorStore

MAX_EMBEDDING_BATCH_SIZE = 20


class KnowledgeError(RuntimeError):
    def __init__(self, stage: str, message: str) -> None:
        super().__init__(message)
        self.stage = stage


@dataclass(frozen=True)
class KnowledgeIngestResult:
    document_id: str
    source: str
    chunks_count: int
    skipped: bool = False
    success: bool = True


def _validate_vectors(vectors: list[list[float]], expected: int) -> None:
    if len(vectors) != expected:
        raise ValueError(f"embedding count mismatch: expected={expected}, actual={len(vectors)}")
    dimension = len(vectors[0]) if vectors else 0
    if dimension == 0 or any(
        len(vector) != dimension
        or any(not math.isfinite(value) for value in vector)
        or not any(value != 0 for value in vector)
        for vector in vectors
    ):
        raise ValueError("embedding vectors must be non-empty, finite, non-zero and same-sized")


class KnowledgeBase:
    """拥有存储连接；调用方负责所注入 provider 的生命周期。"""

    def __init__(
        self,
        *,
        path: str | Path | None = None,
        url: str | None = None,
        api_key: str | None = None,
        embedding_backend: str | None = None,
        embedding_model: str | None = None,
        provider: EmbeddingProvider | None = None,
        embedding_identity: EmbeddingIdentity | None = None,
        chunking: ChunkingConfig | None = None,
    ) -> None:
        if provider is not None and (
            embedding_identity is None
            or embedding_backend is not None
            or embedding_model is not None
        ):
            raise ValueError("an injected provider requires its explicit embedding_identity only")
        if provider is None and embedding_identity is not None:
            raise ValueError("embedding_identity is only accepted with an injected provider")
        self.identity = embedding_identity or resolve_embedding_identity(
            embedding_backend, embedding_model
        )
        if self.identity.backend == "mock" or getattr(provider, "backend_name", None) == "mock":
            raise KnowledgeError(
                "configuration", "knowledge search requires a real embedding backend"
            )
        self.chunking = chunking or ChunkingConfig()
        try:
            # 在打开存储前固定 provider，避免稍后环境变化把另一 endpoint 的向量写入此身份。
            self._provider = provider or get_configured_provider(
                self.identity.backend, self.identity.model
            )
        except Exception as exc:
            raise KnowledgeError("configuration", str(exc)) from exc
        self._write_lock = asyncio.Lock()
        self._closed = False
        try:
            self._store = KnowledgeVectorStore(self.identity, path=path, url=url, api_key=api_key)
        except Exception as exc:
            raise KnowledgeError("storage", str(exc)) from exc

    def __enter__(self) -> Self:
        self._check_open()
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def close(self) -> None:
        if not self._closed:
            self._store.close()
            self._closed = True

    def _check_open(self) -> None:
        if self._closed:
            raise KnowledgeError("storage", "knowledge base is closed")

    async def ingest_text(self, text: str, *, source: str = "text") -> KnowledgeIngestResult:
        self._check_open()
        try:
            document = KnowledgeDocument.from_text(text, source=source)
        except ValueError as exc:
            raise KnowledgeError("input", str(exc)) from exc
        return await self._ingest(document)

    async def ingest_file(self, path: str | Path) -> KnowledgeIngestResult:
        self._check_open()
        try:
            document = KnowledgeDocument.from_file(path)
        except ValueError as exc:
            raise KnowledgeError("input", str(exc)) from exc
        return await self._ingest(document)

    async def _ingest(self, document: KnowledgeDocument) -> KnowledgeIngestResult:
        async with self._write_lock:
            try:
                record = self._store.get_document(document.document_id)
            except Exception as exc:
                raise KnowledgeError("storage", str(exc)) from exc
            if (
                record
                and record.get("status") == "completed"
                and record.get("processing_hash") == self.chunking.processing_hash
            ):
                return KnowledgeIngestResult(
                    document.document_id, document.source, int(record["chunks_count"]), skipped=True
                )
            passages = split_document(document, self.chunking)
            if not passages:
                raise KnowledgeError(
                    "input", "document has no searchable text after structure cleanup"
                )
            try:
                vectors: list[list[float]] = []
                for start in range(0, len(passages), MAX_EMBEDDING_BATCH_SIZE):
                    batch = passages[start : start + MAX_EMBEDDING_BATCH_SIZE]
                    batch_vectors = await self._provider.embed_texts(
                        [p.embedding_text for p in batch]
                    )
                    _validate_vectors(batch_vectors, len(batch))
                    vectors.extend(batch_vectors)
                _validate_vectors(vectors, len(passages))
            except Exception as exc:
                raise KnowledgeError("embedding", str(exc)) from exc
            try:
                self._store.write(document, passages, vectors, self.chunking)
            except Exception as exc:
                raise KnowledgeError("storage", str(exc)) from exc
            return KnowledgeIngestResult(document.document_id, document.source, len(passages))

    async def search(
        self,
        query: str,
        *,
        top_k: int = 5,
        min_score: float | None = None,
        document_id: str | None = None,
    ) -> list[dict[str, Any]]:
        self._check_open()
        if not isinstance(query, str) or not query.strip():
            raise KnowledgeError("input", "query must not be empty")
        if type(top_k) is not int or not 1 <= top_k <= 100:
            raise KnowledgeError("input", "top_k must be an integer between 1 and 100")
        if min_score is not None and (
            isinstance(min_score, bool)
            or not isinstance(min_score, (int, float))
            or not math.isfinite(min_score)
            or not -1 <= min_score <= 1
        ):
            raise KnowledgeError("input", "min_score must be finite and between -1 and 1")
        if document_id is not None and (
            not isinstance(document_id, str) or not document_id.strip()
        ):
            raise KnowledgeError("input", "document_id must not be empty")
        try:
            vector = await self._provider.embed_query(query)
            _validate_vectors([vector], 1)
        except Exception as exc:
            raise KnowledgeError("embedding", str(exc)) from exc
        try:
            return self._store.search(
                vector, top_k=top_k, min_score=min_score, document_id=document_id
            )
        except Exception as exc:
            raise KnowledgeError("storage", str(exc)) from exc
