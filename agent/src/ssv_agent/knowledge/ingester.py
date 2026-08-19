"""知识入库抽象接口。"""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Any, ClassVar

from ssv_agent.knowledge.schema import IngestResult


class Ingester(ABC):
    """知识入库后端接口。"""

    backend_name: ClassVar[str]

    @abstractmethod
    async def ingest(self, document: Any) -> IngestResult:
        """把一份文档写入知识库。"""
