"""可离线运行的 BAAI/bge-m3 embedding 后端。"""

from __future__ import annotations

import importlib
import os
from collections.abc import Callable
from typing import Any

from ssv_agent.embedding.provider import EmbeddingProvider
from ssv_agent.embedding.registry import register_backend

ModelFactory = Callable[[str], Any]


class BgeM3EmbeddingProvider(EmbeddingProvider):
    """按首次 embedding 懒加载 sentence-transformers 模型。"""

    backend_name = "bge_m3"

    def __init__(
        self,
        model_name: str | None = None,
        model_factory: ModelFactory | None = None,
        local_files_only: bool = True,
    ) -> None:
        self._model_name = (
            model_name
            or os.getenv("SSV_EMBEDDING_MODEL")
            or os.getenv("SSV_BGE_M3_MODEL")
            or "BAAI/bge-m3"
        )
        self._model_factory = model_factory
        self._local_files_only = local_files_only
        self._model: Any | None = None

    async def embed_texts(self, texts: list[str]) -> list[list[float]]:
        if not texts:
            return []
        vectors = self._load_model().encode(texts, normalize_embeddings=True)
        return [list(map(float, vector)) for vector in vectors]

    async def embed_query(self, query: str) -> list[float]:
        vectors = await self.embed_texts([query])
        return vectors[0]

    def _load_model(self) -> Any:
        if self._model is None:
            if self._model_factory is not None:
                self._model = self._model_factory(self._model_name)
            else:
                try:
                    module = importlib.import_module("sentence_transformers")
                except ImportError as exc:
                    raise RuntimeError(
                        "bge_m3 requires the optional sentence-transformers dependency"
                    ) from exc
                self._model = module.SentenceTransformer(
                    self._model_name,
                    local_files_only=self._local_files_only,
                )
        return self._model


register_backend("bge_m3", BgeM3EmbeddingProvider)
