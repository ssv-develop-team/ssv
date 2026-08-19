"""知识检索抽象接口。"""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Any, ClassVar

from ssv_agent.knowledge.schema import RetrievalResult


class Retriever(ABC):
    """知识检索后端接口。"""

    backend_name: ClassVar[str]

    @abstractmethod
    async def retrieve(
        self,
        query: str,
        *,
        top_k: int = 5,
        filters: dict[str, Any] | None = None,
    ) -> RetrievalResult:
        """按查询与过滤条件返回知识片段。"""

    async def health_check(self) -> bool:
        return True
