"""供同源 Web 前端读取的 EventLedger HTTP API。"""

from __future__ import annotations

from typing import Literal

from fastapi import FastAPI, Query, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel
from starlette.exceptions import HTTPException as StarletteHTTPException

from ssv_agent.config import SsvConfig
from ssv_agent.event_store.readonly import (
    EvidenceNotFound,
    EvidenceNotImage,
    EvidenceUnavailable,
    EventNotFound,
    ReadOnlyEventStore,
    StoreUnavailable,
)
from ssv_agent.runtime import resolve_agent_path


class AnalysisTaskResponse(BaseModel):
    kind: Literal["review", "evidence_extract"] | None
    state: Literal[
        "not_scheduled",
        "queued",
        "waiting",
        "processing",
        "lease_expired",
        "completed",
        "failed",
    ]
    error_code: str | None
    error_message: str | None
    failed_at: int | None


class EventSummary(BaseModel):
    event_id: str
    source: str
    timestamp_ms: int
    event_type: str | None
    severity: str | None
    status: str
    current_revision: int | None
    verdict: str | None
    confidence: float | None
    analysis_task: AnalysisTaskResponse


class OverviewResponse(BaseModel):
    total_events: int
    pending_events: int
    manual_review_events: int
    completed_events: int
    violation_events: int
    unavailable_evidence_count: int
    recent_events: list[EventSummary]


class EventsPageResponse(BaseModel):
    items: list[EventSummary]
    total: int
    page: int
    page_size: int


class DetectionResponse(BaseModel):
    class_name: str
    class_id: int
    confidence: float
    track_id: int
    track_state: str | None
    occluded: bool


class ReviewHistoryItem(BaseModel):
    event_id: str
    review_id: str
    revision: int
    created_ms: int
    verdict: str
    confidence: float
    evidence_status: str
    explanation: str
    evidence_ids: list[str]


class EvidenceResponse(BaseModel):
    event_id: str
    evidence_id: str
    kind: Literal["frame", "clip"]
    status: Literal["available", "unavailable"]
    mime_type: str | None
    size: int | None
    coverage: str | None = None


class EventDetailResponse(BaseModel):
    event: EventSummary
    detections: list[DetectionResponse]
    review_history: list[ReviewHistoryItem]
    evidence: list[EvidenceResponse]


class ApiError(Exception):
    def __init__(self, status_code: int, code: str, message: str) -> None:
        self.status_code = status_code
        self.code = code
        self.message = message


def create_app(config: SsvConfig) -> FastAPI:
    """创建只读 HTTP 应用；构造期间不连接数据库、Redis 或后台 worker。"""
    app = FastAPI(
        title="SSV Agent Read-only API",
        docs_url=None,
        redoc_url=None,
        openapi_url=None,
    )
    event_store = ReadOnlyEventStore(
        resolve_agent_path(config.agent.event_db_path),
        config.agent.evidence_roots,
    )
    app.state.event_store = event_store

    @app.exception_handler(ApiError)
    async def api_error_handler(_request: Request, exc: ApiError) -> JSONResponse:
        return _error_response(exc.status_code, exc.code, exc.message)

    @app.exception_handler(RequestValidationError)
    async def invalid_query_handler(
        _request: Request, _exc: RequestValidationError
    ) -> JSONResponse:
        return _error_response(422, "invalid_query", "查询参数无效")

    @app.exception_handler(StoreUnavailable)
    async def store_error_handler(_request: Request, _exc: StoreUnavailable) -> JSONResponse:
        return _error_response(503, "read_store_unavailable", "事件账本暂时不可读")

    @app.exception_handler(EventNotFound)
    async def event_not_found_handler(_request: Request, _exc: EventNotFound) -> JSONResponse:
        return _error_response(404, "event_not_found", "事件不存在")

    @app.exception_handler(EvidenceNotFound)
    async def evidence_not_found_handler(_request: Request, _exc: EvidenceNotFound) -> JSONResponse:
        return _error_response(404, "evidence_not_found", "事件证据不存在")

    @app.exception_handler(EvidenceNotImage)
    async def evidence_not_image_handler(_request: Request, _exc: EvidenceNotImage) -> JSONResponse:
        return _error_response(415, "evidence_not_image", "该证据不是可读取的图片")

    @app.exception_handler(EvidenceUnavailable)
    async def evidence_unavailable_handler(
        _request: Request, _exc: EvidenceUnavailable
    ) -> JSONResponse:
        return _error_response(410, "evidence_unavailable", "图片证据当前不可用")

    @app.exception_handler(StarletteHTTPException)
    async def http_error_handler(_request: Request, exc: StarletteHTTPException) -> JSONResponse:
        if exc.status_code == 404:
            return _error_response(404, "route_not_found", "请求资源不存在")
        if exc.status_code == 405:
            return _error_response(405, "method_not_allowed", "请求方法不受支持")
        return _error_response(500, "internal_error", "服务器内部错误")

    @app.exception_handler(Exception)
    async def internal_error_handler(_request: Request, _exc: Exception) -> JSONResponse:
        return _error_response(500, "internal_error", "服务器内部错误")

    @app.get("/api/v1/overview", response_model=OverviewResponse)
    def get_overview() -> dict[str, object]:
        return event_store.overview()

    @app.get("/api/v1/events", response_model=EventsPageResponse)
    def list_events(
        query: str | None = Query(default=None),
        status: Literal["pending", "manual_review", "completed"] | None = Query(default=None),
        page: int = Query(default=1, ge=1),
        page_size: int = Query(default=20, ge=1, le=100),
    ) -> dict[str, object]:
        return event_store.list_events(
            query=query,
            status=status,
            page=page,
            page_size=page_size,
        )

    @app.get("/api/v1/events/{event_id}", response_model=EventDetailResponse)
    def get_event_detail(event_id: str) -> dict[str, object]:
        detail = event_store.get_event_detail(event_id)
        if detail is None:
            raise ApiError(404, "event_not_found", "事件不存在")
        return detail

    @app.get("/api/v1/events/{event_id}/evidence/{evidence_id}/image")
    def get_evidence_image(event_id: str, evidence_id: str) -> Response:
        image = event_store.read_frame(event_id, evidence_id)
        return Response(
            content=image.data,
            media_type=image.content_type,
            headers={
                "X-Content-Type-Options": "nosniff",
                "Cache-Control": "private, no-store",
            },
        )

    return app


def _error_response(status_code: int, code: str, message: str) -> JSONResponse:
    return JSONResponse(
        status_code=status_code,
        content={"error": {"code": code, "message": message}},
    )
