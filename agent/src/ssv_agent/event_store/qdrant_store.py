"""基于 Qdrant 的向量存储。"""

from __future__ import annotations

import hashlib
import json
import os
import uuid
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from qdrant_client import QdrantClient, models

from ssv_agent.embedding.registry import EmbeddingIdentity, resolve_embedding_identity


DEFAULT_EVENT_COLLECTION = "ssv_events"
DEFAULT_RULE_COLLECTION = "ssv_rules"
_SSV_NAMESPACE = uuid.UUID("9b1deb4d-3b7d-4bad-9bdd-2b0d7b3dcb6d")
EmbeddingIdentityInput = EmbeddingIdentity | Mapping[str, object]


def _default_qdrant_path() -> str:
    """返回默认 Qdrant 本地路径：环境变量优先，其次 data/qdrant。"""
    return os.getenv("SSV_QDRANT_PATH", "data/qdrant")


def _build_filter(filters: dict[str, Any] | None) -> models.Filter | None:
    """把简单的字段等值条件转成 Qdrant Filter。"""
    if not filters:
        return None
    conditions = [
        models.FieldCondition(key=key, match=models.MatchValue(value=value))
        for key, value in filters.items()
    ]
    return models.Filter(must=conditions)


def _point_id(domain_id: str) -> str:
    """把业务 id 映射为确定性 UUID（Qdrant 本地模式要求 UUID 或整数）。"""
    return str(uuid.uuid5(_SSV_NAMESPACE, domain_id))


def _coerce_embedding_identity(identity: EmbeddingIdentityInput) -> EmbeddingIdentity:
    if isinstance(identity, EmbeddingIdentity):
        return identity
    if not isinstance(identity, Mapping):
        raise TypeError("embedding_identity must be an EmbeddingIdentity or mapping")

    schema_version = identity.get(
        "schema_version",
        identity.get("adapter_identity_schema_version"),
    )
    backend = identity.get("backend")
    model = identity.get("model")
    if not isinstance(schema_version, int):
        raise ValueError("embedding_identity requires an integer schema_version")
    if not isinstance(backend, str) or not backend:
        raise ValueError("embedding_identity requires a non-empty backend")
    if not isinstance(model, str) or not model:
        raise ValueError("embedding_identity requires a non-empty model")

    algorithm = identity.get("algorithm")
    dimensions = identity.get("dimensions")
    if algorithm is not None and not isinstance(algorithm, str):
        raise ValueError("embedding_identity algorithm must be a string")
    if dimensions is not None and (
        not isinstance(dimensions, int) or isinstance(dimensions, bool) or dimensions <= 0
    ):
        raise ValueError("embedding_identity dimensions must be a positive integer")
    return EmbeddingIdentity(
        schema_version=schema_version,
        backend=backend,
        model=model,
        algorithm=algorithm,
        dimensions=dimensions,
    )


def derive_physical_collection_name(
    base_collection: str,
    embedding_identity: EmbeddingIdentityInput,
) -> str:
    """由逻辑 collection 名与 embedding 身份派生安全、稳定的物理名。"""
    identity = _coerce_embedding_identity(embedding_identity)
    encoded_identity = json.dumps(
        identity.as_dict(),
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    identity_hash = hashlib.sha256(encoded_identity).hexdigest()[:16]
    return f"{base_collection}__embedding_{identity_hash}"


class SsvQdrantStore:
    """事件与规则向量的 Qdrant 读写；本地嵌入模式起步，可切服务模式。"""

    def __init__(
        self,
        path: str | os.PathLike[str] | None = None,
        *,
        url: str | None = None,
        api_key: str | None = None,
        event_collection: str = DEFAULT_EVENT_COLLECTION,
        rule_collection: str = DEFAULT_RULE_COLLECTION,
        embedding_identity: EmbeddingIdentityInput | None = None,
    ) -> None:
        self._embedding_identity = _coerce_embedding_identity(
            embedding_identity
            if embedding_identity is not None
            else resolve_embedding_identity()
        )
        self._event_collection = derive_physical_collection_name(
            event_collection,
            self._embedding_identity,
        )
        self._rule_collection = derive_physical_collection_name(
            rule_collection,
            self._embedding_identity,
        )
        environment_url = os.getenv("SSV_QDRANT_URL")
        resolved_api_key = (
            api_key if api_key is not None else os.getenv("SSV_QDRANT_API_KEY")
        )
        if url is not None:
            self._client = QdrantClient(url=url, api_key=resolved_api_key)
        elif path is not None:
            local_path = Path(path)
            local_path.mkdir(parents=True, exist_ok=True)
            self._client = QdrantClient(path=str(local_path))
        elif environment_url:
            self._client = QdrantClient(
                url=environment_url,
                api_key=resolved_api_key,
            )
        else:
            local_path = Path(_default_qdrant_path())
            local_path.mkdir(parents=True, exist_ok=True)
            self._client = QdrantClient(path=str(local_path))

    @property
    def event_collection(self) -> str:
        return self._event_collection

    @property
    def rule_collection(self) -> str:
        return self._rule_collection

    @property
    def embedding_identity(self) -> EmbeddingIdentity:
        return self._embedding_identity

    def close(self) -> None:
        self._client.close()

    def __enter__(self) -> "SsvQdrantStore":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def ensure_collections(self, vector_size: int) -> None:
        """幂等创建事件与规则集合。"""
        for name in (self._event_collection, self._rule_collection):
            if self._client.collection_exists(name):
                actual_size = self._existing_vector_size(name)
                if actual_size != vector_size:
                    raise ValueError(
                        f"Qdrant collection {name!r} vector size mismatch: "
                        f"expected={vector_size}, actual={actual_size}"
                    )
                continue
            self._client.create_collection(
                collection_name=name,
                vectors_config=models.VectorParams(
                    size=vector_size,
                    distance=models.Distance.COSINE,
                ),
            )

    def _existing_vector_size(self, collection_name: str) -> int:
        collection = self._client.get_collection(collection_name)
        vectors = collection.config.params.vectors
        if isinstance(vectors, dict):
            raise ValueError(
                f"Qdrant collection {collection_name!r} uses named vectors; "
                "expected a single unnamed vector"
            )
        if not isinstance(vectors, models.VectorParams):
            raise ValueError(
                f"Qdrant collection {collection_name!r} has unsupported "
                f"vector configuration: {type(vectors).__name__}"
            )
        return vectors.size

    def upsert_event_vector(
        self,
        event_id: str,
        vector: list[float],
        payload: dict[str, Any] | None = None,
    ) -> None:
        """写入一条事件向量。"""
        payload = {**(payload or {}), "event_id": event_id}
        self._upsert(self._event_collection, event_id, vector, payload)

    def upsert_rule_vector(
        self,
        chunk_id: str,
        vector: list[float],
        payload: dict[str, Any] | None = None,
    ) -> None:
        """写入一条规则 chunk 向量。"""
        payload = {**(payload or {}), "chunk_id": chunk_id}
        self._upsert(self._rule_collection, chunk_id, vector, payload)

    def search_events(
        self,
        query_vector: list[float],
        *,
        top_k: int = 10,
        filters: dict[str, Any] | None = None,
    ) -> list[dict[str, Any]]:
        """按向量检索事件，返回带 score 与 payload 的结果。"""
        return self._search(self._event_collection, query_vector, top_k, filters)

    def search_rules(
        self,
        query_vector: list[float],
        *,
        top_k: int = 5,
        filters: dict[str, Any] | None = None,
    ) -> list[dict[str, Any]]:
        """按向量检索规则 chunk。"""
        return self._search(self._rule_collection, query_vector, top_k, filters)

    def _upsert(
        self,
        collection: str,
        point_id: str,
        vector: list[float],
        payload: dict[str, Any] | None,
    ) -> None:
        self.ensure_collections(len(vector))
        point = models.PointStruct(
            id=_point_id(point_id),
            vector=vector,
            payload=payload or {},
        )
        self._client.upsert(
            collection_name=collection,
            points=[point],
        )

    def _search(
        self,
        collection: str,
        query_vector: list[float],
        top_k: int,
        filters: dict[str, Any] | None,
    ) -> list[dict[str, Any]]:
        self.ensure_collections(len(query_vector))
        response = self._client.query_points(
            collection_name=collection,
            query=query_vector,
            limit=top_k,
            query_filter=_build_filter(filters),
        )
        hits: list[dict[str, Any]] = []
        for hit in response.points:
            item = dict(hit.payload or {})
            item["id"] = hit.id
            item["score"] = hit.score
            hits.append(item)
        return hits
