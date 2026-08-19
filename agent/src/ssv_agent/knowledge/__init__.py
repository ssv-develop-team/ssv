"""规则知识检索与入库抽象。"""

from ssv_agent.knowledge.ingester import Ingester
from ssv_agent.knowledge.registry import get_ingester, get_retriever, register_backend
from ssv_agent.knowledge.retriever import Retriever
from ssv_agent.knowledge.schema import Chunk, IngestResult, RetrievalResult

__all__ = [
    "Chunk",
    "IngestResult",
    "Ingester",
    "RetrievalResult",
    "Retriever",
    "get_ingester",
    "get_retriever",
    "register_backend",
]
