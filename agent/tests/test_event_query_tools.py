from __future__ import annotations

import json
from pathlib import Path

from ssv_agent.event_store import EventLedger
from ssv_agent.event_store.sqlite_store import SsvEventStore
from ssv_agent.review_context import ReviewContext
from ssv_agent.tools.get_event import get_event_tool
from ssv_agent.tools.get_review_result import get_review_result_tool
from ssv_agent.tools.list_events import list_events_tool


def _seed(store: SsvEventStore) -> None:
    store.insert_event(
        "1-0",
        source="camera-1",
        timestamp_ms=1000,
        frame_id=1,
        event_type="detection",
    )
    store.insert_detections(
        "1-0",
        [{"class_name": "person", "class_id": 0, "confidence": 0.9}],
    )
    store.insert_result(
        "1-0",
        verdict="uncertain",
        confidence=0.5,
        evidence_status="missing",
        explanation="no evidence",
    )


def test_get_event_tool(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("SSV_EVENT_DB_PATH", str(tmp_path / "events.db"))
    with SsvEventStore() as store:
        _seed(store)

    out = json.loads(get_event_tool.invoke({"event_id": "1-0"}))
    assert out["found"] is True
    assert out["source"] == "camera-1"

    out = json.loads(get_event_tool.invoke({"event_id": "missing"}))
    assert out["found"] is False


def test_get_event_tool_returns_authoritative_case_without_host_paths(
    tmp_path: Path,
    monkeypatch,
) -> None:
    db_path = tmp_path / "events.db"
    monkeypatch.setenv("SSV_EVENT_DB_PATH", str(db_path))
    monkeypatch.setenv("SSV_EVIDENCE_ROOTS", json.dumps([str(tmp_path)]))
    frame = tmp_path / "private-frame.jpg"
    frame.write_bytes(b"frame")
    with EventLedger(db_path, evidence_roots=[str(tmp_path)]) as ledger:
        ledger.record(
            ReviewContext(
                event_id="case-1",
                source="camera-1",
                timestamp_ms=1000,
                frame_id=1,
                frame_path=str(frame),
            )
        )

    out = json.loads(get_event_tool.invoke({"event_id": "case-1"}))

    assert out["found"] is True
    assert out["event_id"] == "case-1"
    assert out["evidence"][0]["evidence_id"]
    assert "path" not in out["evidence"][0]
    assert str(frame) not in json.dumps(out)


def test_list_events_tool(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("SSV_EVENT_DB_PATH", str(tmp_path / "events.db"))
    with SsvEventStore() as store:
        _seed(store)

    out = json.loads(list_events_tool.invoke({"source": "camera-1"}))
    assert out["total"] == 1

    out = json.loads(list_events_tool.invoke({"class_name": "helmet"}))
    assert out["total"] == 0


def test_get_review_result_tool(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("SSV_EVENT_DB_PATH", str(tmp_path / "events.db"))
    with SsvEventStore() as store:
        _seed(store)

    out = json.loads(get_review_result_tool.invoke({"event_id": "1-0"}))
    assert out["found"] is True
    assert out["verdict"] == "uncertain"
