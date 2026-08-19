"""Embedding 数据模型。"""

from __future__ import annotations

from pydantic import BaseModel, Field


class EmbeddingResult(BaseModel):
    """一次 embedding 调用的结果。"""

    texts: list[str] = Field(default_factory=list)
    vectors: list[list[float]] = Field(default_factory=list)
    backend: str = ""
    success: bool = True
    error_message: str | None = None
