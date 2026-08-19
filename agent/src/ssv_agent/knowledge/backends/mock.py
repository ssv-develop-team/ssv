"""Mock 知识后端：固定检索结果，入库未实现。"""

from __future__ import annotations

from typing import Any

from ssv_agent.knowledge.ingester import Ingester
from ssv_agent.knowledge.registry import register_backend
from ssv_agent.knowledge.retriever import Retriever
from ssv_agent.knowledge.schema import Chunk, IngestResult, RetrievalResult


class MockRetriever(Retriever):
    """返回固定样例 chunk，供工具链路测试。"""

    backend_name = "mock"

    async def retrieve(
        self,
        query: str,
        *,
        top_k: int = 5,
        filters: dict[str, Any] | None = None,
    ) -> RetrievalResult:
        chunk = Chunk(
            chunk_id="mock:rule-001",
            content="进入作业区域必须佩戴安全帽。",
            score=1.0,
            metadata={
                "source": "mock 样例",
                "rule_id": "helmet-001",
                "section": "示例",
            },
        )
        return RetrievalResult(
            chunks=[chunk],
            query=query,
            backend=self.backend_name,
            success=True,
        )


class MockIngester(Ingester):
    """占位入库：返回未实现。"""

    backend_name = "mock"

    async def ingest(self, document: Any) -> IngestResult:
        return IngestResult(
            success=False,
            error_message="not implemented",
        )


register_backend("mock", MockRetriever, MockIngester)
