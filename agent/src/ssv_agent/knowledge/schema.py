"""规则知识的数据模型。"""

from __future__ import annotations

from typing import Any

from pydantic import BaseModel, Field


class Chunk(BaseModel):
    """一条可检索的知识片段。"""

    chunk_id: str
    content: str
    score: float
    metadata: dict[str, Any] = Field(
        default_factory=dict,
        description="约定必须含 source / rule_id / section",
    )


class RetrievalResult(BaseModel):
    """一次检索的结果。"""

    chunks: list[Chunk] = Field(default_factory=list)
    query: str = ""
    backend: str = ""
    success: bool = True
    error_message: str | None = None
    summary: str | None = None


class IngestResult(BaseModel):
    """一次入库操作的结果。"""

    document_id: str = ""
    chunks_count: int = 0
    success: bool = True
    error_message: str | None = None
