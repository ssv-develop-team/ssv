from __future__ import annotations

import json
from pathlib import Path

from deerflow.config.paths import VIRTUAL_PATH_PREFIX

from ssv_agent.event_store import EventLedger, JobKind
from ssv_agent.result import ReviewResult
from ssv_agent.review_context import ReviewContext
from ssv_agent.report import write_analysis_report
from ssv_agent.tools.get_event import get_event_tool
from ssv_agent.tools.get_report import get_report_tool


def _context(event_id: str = "case-1") -> ReviewContext:
    return ReviewContext(
        event_id=event_id,
        source="camera-1",
        timestamp_ms=1_700_000_000_000,
        frame_id=42,
        event_type="person_without_helmet",
        detections=[
            {
                "class": "person",
                "class_id": 0,
                "confidence": 0.92,
                "bbox": [1, 2, 3, 4],
                "track_id": 7,
            }
        ],
    )


def _review() -> ReviewResult:
    return ReviewResult(
        verdict="uncertain",
        confidence=0.0,
        evidence_status="missing",
        explanation="report fixture",
    )


def _register_report(tmp_path: Path, monkeypatch) -> tuple[str, str, Path]:
    db_path = tmp_path / "events.db"
    outputs = tmp_path / "outputs"
    monkeypatch.setenv("SSV_EVENT_DB_PATH", str(db_path))
    monkeypatch.setenv("SSV_OUTPUTS_DIR", str(outputs))
    with EventLedger(db_path) as ledger:
        ledger.record(_context())
        review_job = ledger.claim_job(JobKind.REVIEW, "reviewer", lease_ms=10_000)
        assert review_job is not None
        revision = ledger.complete_review_job(
            review_job,
            "reviewer",
            _review(),
            "review.json",
        )
        review = ledger.get_review_record("case-1", revision)
        assert review is not None
        report_job = ledger.claim_job(JobKind.REPORT, "reporter", lease_ms=10_000)
        assert report_job is not None
        artifact = write_analysis_report(
            "case-1",
            review.review_id,
            "# Report\n\nGenerated from the ledger.\n",
        )
        ledger.complete_report_job(
            report_job,
            "reporter",
            artifact,
            "report-v1",
        )
    return review.review_id, artifact.sha256, outputs


def test_get_report_returns_latest_report_content_virtual_path_and_evidence_refs(
    tmp_path: Path,
    monkeypatch,
) -> None:
    review_id, digest, _ = _register_report(tmp_path, monkeypatch)
    thread_outputs = tmp_path / "thread-outputs"
    thread_outputs.mkdir()
    monkeypatch.setattr(
        "ssv_agent.tools.get_report.get_thread_data",
        lambda _runtime: {"outputs_path": str(thread_outputs)},
    )

    payload = json.loads(get_report_tool.func(None, "case-1"))

    assert payload["found"] is True
    assert payload["review_id"] == review_id
    assert payload["sha256"] == digest
    assert payload["content"].startswith("# Report")
    assert payload["virtual_path"].startswith(f"{VIRTUAL_PATH_PREFIX}/outputs/reports/")
    copied = thread_outputs / Path(payload["virtual_path"]).relative_to(
        f"{VIRTUAL_PATH_PREFIX}/outputs"
    )
    assert copied.read_text(encoding="utf-8").startswith("# Report")
    assert payload["evidence_refs"] == []
    assert str(tmp_path) not in json.dumps(payload)


def test_get_report_selects_specific_review_and_can_omit_content(
    tmp_path: Path,
    monkeypatch,
) -> None:
    review_id, _, _ = _register_report(tmp_path, monkeypatch)
    monkeypatch.setattr(
        "ssv_agent.tools.get_report.get_thread_data",
        lambda _runtime: {},
    )

    payload = json.loads(
        get_report_tool.func(
            None,
            "case-1",
            review_id=review_id,
            include_content=False,
        )
    )

    assert payload["found"] is True
    assert payload["review_id"] == review_id
    assert "content" not in payload
    assert payload["virtual_path"] is None


def test_get_report_hides_missing_and_corrupt_artifacts(
    tmp_path: Path,
    monkeypatch,
) -> None:
    review_id, _, outputs = _register_report(tmp_path, monkeypatch)
    monkeypatch.setattr(
        "ssv_agent.tools.get_report.get_thread_data",
        lambda _runtime: {},
    )
    with EventLedger(tmp_path / "events.db") as ledger:
        report = ledger.get_report_for_review("case-1", review_id)
    assert report is not None
    Path(report.artifact_path).write_text("tampered\n", encoding="utf-8")

    payload = json.loads(get_report_tool.func(None, "case-1", review_id=review_id))

    assert payload == {
        "found": False,
        "event_id": "case-1",
        "reason": "report artifact unavailable",
        "virtual_path": None,
        "evidence_refs": [],
    }
    assert outputs.exists()


def test_get_event_includes_report_metadata_without_artifact_path(
    tmp_path: Path,
    monkeypatch,
) -> None:
    review_id, _, _ = _register_report(tmp_path, monkeypatch)

    payload = json.loads(get_event_tool.invoke({"event_id": "case-1"}))

    assert payload["reports"][0]["review_id"] == review_id
    assert "artifact_path" not in json.dumps(payload)
    assert str(tmp_path) not in json.dumps(payload)
