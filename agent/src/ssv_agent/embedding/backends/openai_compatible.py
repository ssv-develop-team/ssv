"""OpenAI 兼容 embedding API 后端。"""

from __future__ import annotations

import os

from openai import AsyncOpenAI

from ssv_agent.embedding.provider import EmbeddingProvider
from ssv_agent.embedding.registry import register_backend


class OpenAICompatibleEmbeddingProvider(EmbeddingProvider):
    """通过 OpenAI 兼容接口生成向量；配置来自环境变量。"""

    backend_name = "openai_compatible"

    def __init__(
        self,
        model: str | None = None,
        base_url: str | None = None,
        api_key: str | None = None,
    ) -> None:
        self._model = model or os.getenv("SSV_EMBEDDING_MODEL", "text-embedding-3-small")
        self._client = AsyncOpenAI(
            api_key=api_key or os.getenv("SSV_EMBEDDING_API_KEY"),
            base_url=base_url or os.getenv("SSV_EMBEDDING_BASE_URL"),
        )

    async def embed_texts(self, texts: list[str]) -> list[list[float]]:
        response = await self._client.embeddings.create(model=self._model, input=texts)
        return [item.embedding for item in response.data]

    async def embed_query(self, query: str) -> list[float]:
        vectors = await self.embed_texts([query])
        return vectors[0]


register_backend("openai_compatible", OpenAICompatibleEmbeddingProvider)
