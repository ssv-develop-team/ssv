"""Embedding provider 抽象接口。"""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import ClassVar


class EmbeddingProvider(ABC):
    """把文本转成向量的统一接口。"""

    backend_name: ClassVar[str]

    @abstractmethod
    async def embed_texts(self, texts: list[str]) -> list[list[float]]:
        """批量生成文本向量。"""

    @abstractmethod
    async def embed_query(self, query: str) -> list[float]:
        """生成查询向量。"""

    async def health_check(self) -> bool:
        return True
