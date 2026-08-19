"""确定性哈希向量后端，用于测试与无外部 API 环境。"""

from __future__ import annotations

import hashlib
import math

from ssv_agent.embedding.provider import EmbeddingProvider
from ssv_agent.embedding.registry import register_backend


def _hash_vector(text: str, dims: int) -> list[float]:
    """从文本生成确定性且归一化的向量。"""
    seed = hashlib.sha256(text.encode("utf-8")).digest()
    values: list[int] = []
    while len(values) < dims:
        seed = hashlib.sha256(seed).digest()
        values.extend(seed)
    raw = [(value / 255.0) * 2 - 1 for value in values[:dims]]
    norm = math.sqrt(sum(value * value for value in raw)) or 1.0
    return [value / norm for value in raw]


class MockEmbeddingProvider(EmbeddingProvider):
    """确定性 mock embedding：同文本同向量。"""

    backend_name = "mock"

    def __init__(self, dims: int = 64) -> None:
        self.dims = dims

    async def embed_texts(self, texts: list[str]) -> list[list[float]]:
        return [_hash_vector(text, self.dims) for text in texts]

    async def embed_query(self, query: str) -> list[float]:
        return _hash_vector(query, self.dims)


register_backend("mock", MockEmbeddingProvider)
