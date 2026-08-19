"""Embedding provider 抽象与后端。"""

from ssv_agent.embedding.provider import EmbeddingProvider
from ssv_agent.embedding.registry import get_provider, register_backend

__all__ = ["EmbeddingProvider", "get_provider", "register_backend"]
