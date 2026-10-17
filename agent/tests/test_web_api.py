from __future__ import annotations

import hashlib
import json
import sqlite3
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient
from PIL import Image

import ssv_agent.event_store.readonly as readonly_module
from ssv_agent.config import SsvConfig
from ssv_agent.event_store.sqlite_store import SsvEventStore
from ssv_agent.event_store.readonly import ReadOnlyEventStore
from ssv_agent.web_api import create_app


@dataclass(frozen=True)
class SeededApi:
    app: FastAPI
    config: SsvConfig
    db_path: Path
    evidence_root: Path
    image_bytes: bytes


def _png_bytes() -> bytes:
    from io import BytesIO

    buffer = BytesIO()
    Image.new("RGB", (2, 2), color=(24, 96, 160)).save(buffer, format="PNG")
    return buffer.getvalue()


def _insert_event(
    connection: sqlite3.Connection,
    *,
    event_id: str,
    source: str,
    timestamp_ms: int,
    event_type: str,
    status: str,
) -> None:
    connection.execute(
        """
        INSERT INTO events (
            event_id, source, timestamp_ms, frame_id, event_type, severity,
            status, created_ms
        ) VALUES (?, ?, ?, 1, ?, 'high', ?, ?)
        """,
        (event_id, source, timestamp_ms, event_type, status, timestamp_ms),
    )


def _insert_job(
    connection: sqlite3.Connection,
    *,
    event_id: str,
    kind: str,
    state: str,
    available_at_ms: int,
    lease_expires_ms: int | None = None,
) -> None:
    connection.execute(
        """
        INSERT INTO durable_jobs (
            kind, entity_id, entity_revision, state, attempts, lease_owner,
            lease_expires_ms, available_at_ms, created_ms, updated_ms
        ) VALUES (?, ?, 0, ?, 0, NULL, ?, ?, 1, 1)
        """,
        (kind, event_id, state, lease_expires_ms, available_at_ms),
    )


def _insert_review(
    connection: sqlite3.Connection,
    *,
    event_id: str,
    review_id: str,
    revision: int,
    created_ms: int,
    verdict: str,
    confidence: float,
    evidence_ids: list[str],
) -> None:
    connection.execute(
        """
        INSERT INTO reviews (
            review_id, event_id, revision, verdict, confidence, evidence_status,
            explanation, evidence_ids_json, claims_json, created_ms
        ) VALUES (?, ?, ?, ?, ?, 'complete', ?, ?, '[]', ?)
        """,
        (
            review_id,
            event_id,
            revision,
            verdict,
            confidence,
            f"review {revision}",
            json.dumps(evidence_ids),
            created_ms,
        ),
    )


def _register_evidence(
    connection: sqlite3.Connection,
    *,
    event_id: str,
    evidence_id: str | None,
    kind: str,
    path: Path,
    mime_type: str | None,
    available: bool = True,
    size: int | None = None,
    mtime: float | None = None,
    sha256: str | None = None,
    coverage: str = "anchor_only",
) -> None:
    if path.exists():
        metadata = path.stat()
        if size is None:
            size = metadata.st_size
        if mtime is None:
            mtime = metadata.st_mtime
        if sha256 is None and path.is_file():
            sha256 = hashlib.sha256(path.read_bytes()).hexdigest()
    connection.execute(
        """
        INSERT INTO evidence (
            event_id, evidence_id, kind, path, mime_type, available, size, mtime,
            sha256, coverage
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (
            event_id,
            evidence_id,
            kind,
            str(path),
            mime_type,
            int(available),
            size,
            mtime,
            sha256,
            coverage,
        ),
    )


@pytest.fixture
def seeded_api(tmp_path: Path) -> SeededApi:
    evidence_root = tmp_path / "evidence"
    evidence_root.mkdir()
    db_path = tmp_path / "events.db"
    event_store = SsvEventStore(db_path)
    event_store.close()

    valid_png = _png_bytes()
    valid_path = evidence_root / "valid.png"
    valid_path.write_bytes(valid_png)
    legacy_path = evidence_root / "legacy.png"
    legacy_path.write_bytes(valid_png)
    mime_mismatch_path = evidence_root / "mime-mismatch.png"
    mime_mismatch_path.write_bytes(valid_png)
    unavailable_path = evidence_root / "unavailable.png"
    unavailable_path.write_bytes(valid_png)
    clip_path = evidence_root / "clip.mp4"
    clip_path.write_bytes(b"not returned as video")
    text_path = evidence_root / "note.txt"
    text_path.write_text("not an image", encoding="utf-8")
    bad_image_path = evidence_root / "bad.png"
    bad_image_path.write_bytes(b"not a png")
    mismatch_path = evidence_root / "mismatch.png"
    mismatch_path.write_bytes(valid_png)
    directory_path = evidence_root / "directory"
    directory_path.mkdir()

    outside_path = tmp_path / "outside.png"
    outside_path.write_bytes(valid_png)
    traversal_path = evidence_root / ".." / outside_path.name
    symlink_path = evidence_root / "escape.png"
    symlink_path.symlink_to(outside_path)
    missing_path = evidence_root / "missing.png"
    missing_path.write_bytes(valid_png)

    connection = sqlite3.connect(db_path)
    connection.row_factory = sqlite3.Row
    _insert_event(
        connection,
        event_id="Event-A",
        source="Cam-A",
        timestamp_ms=2000,
        event_type="helmet",
        status="manual_review",
    )
    _insert_event(
        connection,
        event_id="Event-B",
        source="Cam-B",
        timestamp_ms=3000,
        event_type="ppe",
        status="completed",
    )
    _insert_event(
        connection,
        event_id="Event-C",
        source="Cam-C",
        timestamp_ms=3000,
        event_type="ppe",
        status="pending",
    )
    _insert_event(
        connection,
        event_id="Event-D",
        source="Cam-D",
        timestamp_ms=1000,
        event_type="helmet",
        status="pending",
    )
    _insert_review(
        connection,
        event_id="Event-A",
        review_id="review-a-2",
        revision=2,
        created_ms=9000,
        verdict="violation",
        confidence=0.51,
        evidence_ids=["shared-id"],
    )
    _insert_review(
        connection,
        event_id="Event-A",
        review_id="review-a-5",
        revision=5,
        created_ms=1000,
        verdict="compliant",
        confidence=0.83,
        evidence_ids=["frame-valid", "missing-reference"],
    )
    _insert_review(
        connection,
        event_id="Event-B",
        review_id="review-b-1",
        revision=1,
        created_ms=3000,
        verdict="violation",
        confidence=0.94,
        evidence_ids=["shared-id"],
    )
    connection.execute(
        """
        INSERT INTO detections (
            event_id, class_name, class_id, confidence, bbox_json, track_id, track_state, occluded
        ) VALUES ('Event-A', 'person', 0, 0.91, '[10,20,30,40]', 7, 'matched', 1)
        """
    )

    _register_evidence(
        connection,
        event_id="Event-A",
        evidence_id="frame-valid",
        kind="frame",
        path=valid_path,
        mime_type="image/png",
    )
    _register_evidence(
        connection,
        event_id="Event-A",
        evidence_id="clip-a",
        kind="clip",
        path=clip_path,
        mime_type="video/mp4",
    )
    _register_evidence(
        connection,
        event_id="Event-A",
        evidence_id="plain-a",
        kind="frame",
        path=text_path,
        mime_type="text/plain",
    )
    _register_evidence(
        connection,
        event_id="Event-A",
        evidence_id="unavailable-a",
        kind="frame",
        path=unavailable_path,
        mime_type="image/png",
        available=False,
    )
    _register_evidence(
        connection,
        event_id="Event-A",
        evidence_id="missing-a",
        kind="frame",
        path=missing_path,
        mime_type="image/png",
    )
    missing_path.unlink()
    _register_evidence(
        connection,
        event_id="Event-A",
        evidence_id="outside-a",
        kind="frame",
        path=outside_path,
        mime_type="image/png",
    )
    _register_evidence(
        connection,
        event_id="Event-A",
        evidence_id="traversal-a",
        kind="frame",
        path=traversal_path,
        mime_type="image/png",
    )
    _register_evidence(
        connection,
        event_id="Event-A",
        evidence_id="escape-a",
        kind="frame",
        path=symlink_path,
        mime_type="image/png",
    )
    _register_evidence(
        connection,
        event_id="Event-A",
        evidence_id="bad-content-a",
        kind="frame",
        path=bad_image_path,
        mime_type="image/png",
    )
    _register_evidence(
        connection,
        event_id="Event-A",
        evidence_id="size-mismatch-a",
        kind="frame",
        path=mismatch_path,
        mime_type="image/png",
        size=len(valid_png) + 1,
    )
    _register_evidence(
        connection,
        event_id="Event-A",
        evidence_id="mime-mismatch-a",
        kind="frame",
        path=mime_mismatch_path,
        mime_type="image/jpeg",
    )
    _register_evidence(
        connection,
        event_id="Event-A",
        evidence_id="directory-a",
        kind="frame",
        path=directory_path,
        mime_type="image/png",
    )
    _register_evidence(
        connection,
        event_id="Event-A",
        evidence_id=None,
        kind="frame",
        path=legacy_path,
        mime_type="image/png",
    )
    _register_evidence(
        connection,
        event_id="Event-B",
        evidence_id="shared-id",
        kind="frame",
        path=valid_path,
        mime_type="image/png",
    )
    connection.commit()
    connection.close()

    config = SsvConfig.model_validate(
        {
            "agent": {
                "event_db_path": str(db_path),
                "evidence_roots": [str(evidence_root)],
            },
            "evidence_cache": {"directory": str(evidence_root / "cache")},
        }
    )
    return SeededApi(
        app=create_app(config),
        config=config,
        db_path=db_path,
        evidence_root=evidence_root,
        image_bytes=valid_png,
    )


def _client(app: FastAPI, *, raise_server_exceptions: bool = True) -> TestClient:
    return TestClient(app, raise_server_exceptions=raise_server_exceptions)


def test_app_exposes_only_the_four_read_only_get_routes(seeded_api: SeededApi) -> None:
    routes = {
        route.path: route.methods for route in seeded_api.app.routes if hasattr(route, "methods")
    }

    assert routes == {
        "/api/v1/overview": {"GET"},
        "/api/v1/events": {"GET"},
        "/api/v1/events/{event_id}": {"GET"},
        "/api/v1/events/{event_id}/evidence/{evidence_id}/image": {"GET"},
    }


def test_overview_counts_persisted_events_and_unavailable_evidence(seeded_api: SeededApi) -> None:
    with _client(seeded_api.app) as client:
        response = client.get("/api/v1/overview")

    assert response.status_code == 200
    body = response.json()
    assert body["total_events"] == 4
    assert body["pending_events"] == 2
    assert body["manual_review_events"] == 1
    assert body["completed_events"] == 1
    assert body["violation_events"] == 1
    assert body["unavailable_evidence_count"] == 1
    assert [event["event_id"] for event in body["recent_events"]] == [
        "Event-B",
        "Event-C",
        "Event-A",
        "Event-D",
    ]
    assert "redis" not in response.text.casefold()


def test_overview_limits_recent_events_to_eight(seeded_api: SeededApi) -> None:
    connection = sqlite3.connect(seeded_api.db_path)
    for index in range(9):
        _insert_event(
            connection,
            event_id=f"Extra-{index:02}",
            source="Extra-Cam",
            timestamp_ms=4000 + index,
            event_type="test",
            status="completed",
        )
    connection.commit()
    connection.close()

    with _client(seeded_api.app) as client:
        recent = client.get("/api/v1/overview").json()["recent_events"]

    assert len(recent) == 8
    assert [event["event_id"] for event in recent] == [
        "Extra-08",
        "Extra-07",
        "Extra-06",
        "Extra-05",
        "Extra-04",
        "Extra-03",
        "Extra-02",
        "Extra-01",
    ]


def test_events_filters_case_insensitively_and_uses_stable_pagination(
    seeded_api: SeededApi,
) -> None:
    with _client(seeded_api.app) as client:
        by_id = client.get("/api/v1/events", params={"query": "event-a"}).json()
        by_source = client.get("/api/v1/events", params={"query": "CAM-B"}).json()
        by_type = client.get("/api/v1/events", params={"query": "HELMET"}).json()
        pending = client.get("/api/v1/events", params={"status": "pending"}).json()
        page_one = client.get("/api/v1/events", params={"page_size": 2}).json()
        page_two = client.get("/api/v1/events", params={"page": 2, "page_size": 2}).json()

    assert [item["event_id"] for item in by_id["items"]] == ["Event-A"]
    assert by_source["total"] == 1
    assert by_source["items"][0]["event_id"] == "Event-B"
    assert by_type["total"] == 2
    assert [item["event_id"] for item in pending["items"]] == ["Event-C", "Event-D"]
    assert page_one == {
        "items": page_one["items"],
        "total": 4,
        "page": 1,
        "page_size": 2,
    }
    assert [item["event_id"] for item in page_one["items"]] == ["Event-B", "Event-C"]
    assert [item["event_id"] for item in page_two["items"]] == ["Event-A", "Event-D"]


def test_events_escapes_like_wildcards_and_rejects_sql_injection(seeded_api: SeededApi) -> None:
    with _client(seeded_api.app) as client:
        wildcard = client.get("/api/v1/events", params={"query": "%"}).json()
        injection = client.get("/api/v1/events", params={"query": "' OR 1=1 --"}).json()

    assert wildcard["total"] == 0
    assert injection["total"] == 0
    assert injection["items"] == []


@pytest.mark.parametrize(
    "params",
    [
        {"status": "unknown"},
        {"page": 0},
        {"page_size": 0},
        {"page_size": 101},
        {"page": "not-a-number"},
    ],
)
def test_events_query_validation_uses_uniform_error_body(
    seeded_api: SeededApi, params: dict[str, object]
) -> None:
    with _client(seeded_api.app) as client:
        response = client.get("/api/v1/events", params=params)

    assert response.status_code == 422
    assert response.json() == {"error": {"code": "invalid_query", "message": "查询参数无效"}}


def test_event_detail_uses_revision_and_event_scoped_evidence_ids(seeded_api: SeededApi) -> None:
    with _client(seeded_api.app) as client:
        response = client.get("/api/v1/events/Event-A")

    assert response.status_code == 200
    body = response.json()
    assert body["event"] == {
        "event_id": "Event-A",
        "source": "Cam-A",
        "timestamp_ms": 2000,
        "event_type": "helmet",
        "severity": "high",
        "status": "manual_review",
        "current_revision": 5,
        "verdict": "compliant",
        "confidence": 0.83,
        "analysis_task": {"kind": "review", "state": "completed"},
    }
    assert [review["revision"] for review in body["review_history"]] == [2, 5]
    assert [review["review_id"] for review in body["review_history"]] == [
        "review-a-2",
        "review-a-5",
    ]
    assert body["review_history"][0]["confidence"] == 0.51
    assert body["review_history"][1]["confidence"] == 0.83
    assert body["review_history"][0]["event_id"] == "Event-A"
    assert body["review_history"][0]["evidence_ids"] == ["shared-id"]
    assert body["review_history"][1]["evidence_ids"] == [
        "frame-valid",
        "missing-reference",
    ]
    with _client(seeded_api.app) as client:
        no_review = client.get("/api/v1/events/Event-C").json()["event"]
    assert no_review["current_revision"] is None
    assert no_review["verdict"] is None
    assert no_review["confidence"] is None
    assert no_review["analysis_task"] == {"kind": None, "state": "not_scheduled"}
    evidence_ids = {item["evidence_id"] for item in body["evidence"]}
    assert "frame-valid" in evidence_ids
    assert "clip-a" in evidence_ids
    assert "shared-id" not in evidence_ids
    assert "missing-reference" not in evidence_ids
    assert all("path" not in item for item in body["evidence"])
    assert seeded_api.evidence_root.as_posix() not in response.text
    assert body["detections"] == [
        {
            "class_name": "person",
            "class_id": 0,
            "confidence": 0.91,
            "track_id": 7,
            "track_state": "matched",
            "occluded": True,
        }
    ]
    assert "bbox_json" not in response.text


def test_event_summaries_expose_durable_analysis_task_states(
    seeded_api: SeededApi,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(readonly_module.time, "time_ns", lambda: 5_000_000_000)
    connection = sqlite3.connect(seeded_api.db_path)
    _insert_event(
        connection,
        event_id="Event-E",
        source="Cam-E",
        timestamp_ms=500,
        event_type="ppe",
        status="pending",
    )
    _insert_event(
        connection,
        event_id="Event-F",
        source="Cam-F",
        timestamp_ms=400,
        event_type="ppe",
        status="pending",
    )
    _insert_event(
        connection,
        event_id="Event-G",
        source="Cam-G",
        timestamp_ms=300,
        event_type="ppe",
        status="pending",
    )
    _insert_job(
        connection,
        event_id="Event-A",
        kind="review",
        state="pending",
        available_at_ms=4_999,
    )
    _insert_job(
        connection,
        event_id="Event-B",
        kind="review",
        state="processing",
        available_at_ms=1,
        lease_expires_ms=5_001,
    )
    _insert_job(
        connection,
        event_id="Event-C",
        kind="evidence_extract",
        state="pending",
        available_at_ms=5_001,
    )
    _insert_job(
        connection,
        event_id="Event-D",
        kind="review",
        state="processing",
        available_at_ms=1,
        lease_expires_ms=4_999,
    )
    _insert_job(
        connection,
        event_id="Event-E",
        kind="evidence_extract",
        state="completed",
        available_at_ms=1,
    )
    _insert_job(
        connection,
        event_id="Event-F",
        kind="review",
        state="dead",
        available_at_ms=1,
    )
    connection.commit()
    connection.close()

    with _client(seeded_api.app) as client:
        tasks = {
            event_id: client.get("/api/v1/events", params={"query": event_id}).json()["items"][0][
                "analysis_task"
            ]
            for event_id in (
                "Event-A",
                "Event-B",
                "Event-C",
                "Event-D",
                "Event-E",
                "Event-F",
                "Event-G",
            )
        }

    assert tasks == {
        "Event-A": {"kind": "review", "state": "queued"},
        "Event-B": {"kind": "review", "state": "processing"},
        "Event-C": {"kind": "evidence_extract", "state": "waiting"},
        "Event-D": {"kind": "review", "state": "lease_expired"},
        "Event-E": {"kind": "evidence_extract", "state": "completed"},
        "Event-F": {"kind": "review", "state": "failed"},
        "Event-G": {"kind": None, "state": "not_scheduled"},
    }


@pytest.mark.parametrize(
    ("evidence_id", "status", "code"),
    [
        ("clip-a", 415, "evidence_not_image"),
        ("plain-a", 415, "evidence_not_image"),
        ("unavailable-a", 410, "evidence_unavailable"),
        ("missing-a", 410, "evidence_unavailable"),
        ("outside-a", 410, "evidence_unavailable"),
        ("traversal-a", 410, "evidence_unavailable"),
        ("escape-a", 410, "evidence_unavailable"),
        ("bad-content-a", 410, "evidence_unavailable"),
        ("size-mismatch-a", 410, "evidence_unavailable"),
        ("mime-mismatch-a", 410, "evidence_unavailable"),
        ("directory-a", 410, "evidence_unavailable"),
    ],
)
def test_image_endpoint_rejects_unavailable_or_non_image_evidence(
    seeded_api: SeededApi, evidence_id: str, status: int, code: str
) -> None:
    with _client(seeded_api.app) as client:
        response = client.get(f"/api/v1/events/Event-A/evidence/{evidence_id}/image")

    assert response.status_code == status
    assert response.json()["error"]["code"] == code
    assert str(seeded_api.evidence_root) not in response.text


def test_image_endpoint_returns_only_event_associated_frame_with_safe_headers(
    seeded_api: SeededApi,
) -> None:
    with _client(seeded_api.app) as client:
        response = client.get("/api/v1/events/Event-A/evidence/frame-valid/image")
        wrong_event = client.get("/api/v1/events/Event-A/evidence/shared-id/image")
        right_event = client.get("/api/v1/events/Event-B/evidence/shared-id/image")

    assert response.status_code == 200
    assert response.content == seeded_api.image_bytes
    assert response.headers["content-type"] == "image/png"
    assert response.headers["x-content-type-options"] == "nosniff"
    assert response.headers["cache-control"] == "private, no-store"
    assert wrong_event.status_code == 404
    assert wrong_event.json()["error"]["code"] == "evidence_not_found"
    assert right_event.status_code == 200


@pytest.mark.parametrize(
    ("path", "status", "code"),
    [
        ("/api/v1/events/unknown", 404, "event_not_found"),
        ("/api/v1/events/unknown/evidence/missing/image", 404, "event_not_found"),
        ("/api/v1/events/Event-A/evidence/missing/image", 404, "evidence_not_found"),
    ],
)
def test_missing_event_and_evidence_use_uniform_error_body(
    seeded_api: SeededApi, path: str, status: int, code: str
) -> None:
    with _client(seeded_api.app) as client:
        response = client.get(path)

    assert response.status_code == status
    assert response.json()["error"]["code"] == code
    assert set(response.json()) == {"error"}
    assert str(seeded_api.db_path) not in response.text


def test_unreadable_database_returns_503_without_path_details(tmp_path: Path) -> None:
    evidence_root = tmp_path / "evidence"
    evidence_root.mkdir()
    config = SsvConfig.model_validate(
        {
            "agent": {
                "event_db_path": str(tmp_path / "missing.db"),
                "evidence_roots": [str(evidence_root)],
            },
            "evidence_cache": {"directory": str(evidence_root / "cache")},
        }
    )

    with _client(create_app(config)) as client:
        response = client.get("/api/v1/overview")

    assert response.status_code == 503
    assert response.json() == {
        "error": {"code": "read_store_unavailable", "message": "事件账本暂时不可读"}
    }
    assert str(tmp_path) not in response.text


def test_unexpected_errors_hide_exception_details(seeded_api: SeededApi) -> None:
    seeded_api.app.state.event_store.overview = lambda: (_ for _ in ()).throw(
        RuntimeError(f"SQL secret /host/path {seeded_api.db_path}")
    )
    with _client(seeded_api.app, raise_server_exceptions=False) as client:
        response = client.get("/api/v1/overview")

    assert response.status_code == 500
    assert response.json() == {"error": {"code": "internal_error", "message": "服务器内部错误"}}
    assert str(seeded_api.db_path) not in response.text
    assert "secret" not in response.text


def test_api_uses_read_only_connections_without_migrations_or_database_writes(
    seeded_api: SeededApi, monkeypatch: pytest.MonkeyPatch
) -> None:
    from ssv_agent.event_store import ledger, schema, sqlite_store
    from ssv_agent.event_store import readonly

    def forbidden(*_args: Any, **_kwargs: Any) -> None:
        raise AssertionError("write-capable ledger construction or migration was used")

    monkeypatch.setattr(ledger.EventLedger, "__init__", forbidden)
    monkeypatch.setattr(sqlite_store.SsvEventStore, "__init__", forbidden)
    monkeypatch.setattr(sqlite_store, "migrate_schema", forbidden)
    monkeypatch.setattr(schema, "migrate_schema", forbidden)
    real_connect = readonly.sqlite3.connect
    connection_settings: list[tuple[str, bool]] = []

    def record_connect(database: str, *args: Any, **kwargs: Any) -> sqlite3.Connection:
        connection_settings.append((database, bool(kwargs.get("uri"))))
        return real_connect(database, *args, **kwargs)

    monkeypatch.setattr(readonly.sqlite3, "connect", record_connect)

    app = create_app(seeded_api.config)
    before = seeded_api.db_path.read_bytes()
    read_store: ReadOnlyEventStore = app.state.event_store
    with read_store._connection() as connection:
        assert connection.execute("PRAGMA query_only").fetchone()[0] == 1
        with pytest.raises(sqlite3.OperationalError):
            connection.execute("UPDATE events SET status = 'pending' WHERE event_id = 'Event-D'")

    with _client(app) as client:
        assert client.get("/api/v1/overview").status_code == 200
        assert client.get("/api/v1/events").status_code == 200
        assert client.get("/api/v1/events/Event-A").status_code == 200

    assert seeded_api.db_path.read_bytes() == before
    assert connection_settings
    assert all("mode=ro" in database and use_uri for database, use_uri in connection_settings)
