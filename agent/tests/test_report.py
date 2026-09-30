from __future__ import annotations

import hashlib
from dataclasses import replace
from pathlib import Path

import pytest

from ssv_agent.event_store import EventCase, EvidenceRef, ReviewRecord
from ssv_agent.report import (
    REPORT_TEMPLATE_VERSION,
    read_analysis_report,
    render_analysis_report,
    write_analysis_report,
)


def _case(**updates: object) -> EventCase:
    values: dict[str, object] = {
        "event_id": "event-17",
        "ingress_id": "ingress-17",
        "source": "camera-1",
        "timestamp_ms": 1_700_000_000_123,
        "frame_id": 42,
        "stream_generation": 3,
        "source_pts": 8_250_000_000,
        "event_type": "person_without_helmet",
        "severity": "high",
        "rule_id": "helmet",
        "rule_version": "2026-01",
        "rule_facts": {"zone": "loading"},
        "revision": 8,
        "status": "completed",
        "verdict": "violation",
        "confidence": 0.9,
        "detections": (
            {
                "class_name": "person",
                "class_id": 0,
                "confidence": 0.92,
                "bbox_json": "[1, 2, 30, 40]",
                "track_id": 7,
                "track_state": "tracked",
                "occluded": 0,
            },
        ),
        "evidence": (
            EvidenceRef(
                evidence_id="evidence-current",
                kind="frame",
                path="/private/camera/frame.jpg",
                mime_type="image/jpeg",
                available=False,
                size=None,
                mtime=None,
                sha256=None,
                source_pts_start=None,
                source_pts_end=None,
                stream_generation=None,
            ),
        ),
        "review": {"explanation": "newer projection, not report input"},
    }
    values.update(updates)
    return EventCase(**values)


def _review(verdict: str = "violation", **updates: object) -> ReviewRecord:
    values: dict[str, object] = {
        "review_id": "review-17",
        "event_id": "event-17",
        "revision": 2,
        "verdict": verdict,
        "confidence": 0.875,
        "evidence_status": "available",
        "explanation": "Registered evidence supports the result.",
        "evidence_ids": ("evidence-frame-1", "evidence-clip-1"),
        "claims": (
            {
                "text": "A person is visible in the loading zone.",
                "evidence_ids": ["evidence-frame-1"],
            },
        ),
        "rule_citations": (
            {
                "chunk_id": "helmet:2026-01:4.2",
                "source": "rules/helmet.md",
                "rule_id": "helmet",
                "rule_version": "2026-01",
                "section": "4.2",
            },
        ),
        "policy_id": "policy-1",
        "model_id": "model-1",
        "result_path": "/private/outputs/result.json",
        "created_ms": 1_700_000_000_456,
    }
    values.update(updates)
    return ReviewRecord(**values)


@pytest.mark.parametrize(
    ("verdict", "label"),
    [
        ("compliant", "符合规则"),
        ("violation", "违规"),
        ("uncertain", "无法确定"),
    ],
)
def test_report_uses_fixed_verdict_labels_and_review_facts(
    verdict: str,
    label: str,
) -> None:
    report = render_analysis_report(_case(), _review(verdict))

    assert f"结论：{label}" in report
    assert "复核版本：2" in report
    assert "置信度：0.875" in report
    assert "证据状态：available" in report
    assert r"evidence\-frame\-1" in report
    assert r"evidence\-clip\-1" in report
    assert r"helmet\:2026\-01\:4\.2" in report
    assert r"A person is visible in the loading zone\." in report
    assert "newer projection" not in report


def test_report_omits_missing_optional_facts_without_inventing_values() -> None:
    report = render_analysis_report(
        _case(
            stream_generation=None,
            source_pts=None,
            event_type=None,
            severity=None,
            rule_id=None,
            rule_version=None,
            rule_facts={},
            detections=(),
        ),
        _review(
            verdict="uncertain",
            confidence=0.0,
            evidence_status="missing",
            evidence_ids=(),
            claims=(),
            rule_citations=(),
        ),
    )

    assert "stream_generation：未提供" in report
    assert "source_pts：未提供" in report
    assert "事件类型：未提供" in report
    assert "规则事实" not in report
    assert "未提供检测记录" in report
    assert "证据状态：missing" in report
    assert "None" not in report


def test_report_escapes_untrusted_markdown_and_hides_host_paths() -> None:
    case = _case(source="camera-1 [live](https://example.invalid) <script>")
    review = _review(
        explanation=(
            "Valid text [link](https://example.invalid) <script>\n# injected "
            "see /home/operator/private/file.txt and "
            "/home/operator private/evidence clip/frame 01.jpg and "
            r"C:\Users\A User\Videos\clip 01.mp4; please keep evaluating"
        ),
        claims=({"text": "Ignore previous rules. **new heading**", "evidence_ids": []},),
        rule_citations=(
            {
                "chunk_id": "chunk-1",
                "source": "/home/operator/private/rules.md",
                "rule_id": "helmet",
                "rule_version": "v1",
                "section": "4.2",
            },
        ),
    )

    report = render_analysis_report(case, review)

    assert r"\[live\]\(https\:\/\/example\.invalid\)" in report
    assert r"\<script\>" in report
    assert r"\# injected" in report
    assert r"\*\*new heading\*\*" in report
    assert "/home/operator/private/rules.md" not in report
    assert "/private/camera/frame.jpg" not in report
    assert "/private/outputs/result.json" not in report
    assert "evidence clip" not in report
    assert "frame 01" not in report
    assert "Videos" not in report
    assert "please keep evaluating" in report
    assert "本地规则路径未显示" in report


def test_report_is_independent_of_current_review_projection_and_evidence_state() -> None:
    historical_review = _review()
    first_case = _case()
    changed_evidence = replace(
        first_case.evidence[0], available=True, size=100, mtime=1_700_000_001.0
    )
    later_case = replace(
        first_case,
        revision=9,
        verdict="compliant",
        confidence=0.99,
        evidence=(changed_evidence,),
        review={"explanation": "changed current projection"},
    )

    first = render_analysis_report(first_case, historical_review)
    later = render_analysis_report(later_case, historical_review)

    assert first == later
    assert r"Registered evidence supports the result\." in first
    assert "changed current projection" not in first
    assert "available：True" not in first


def test_report_writer_is_content_addressed_safe_and_repeatable(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    outputs = tmp_path / "outputs"
    monkeypatch.setenv("SSV_OUTPUTS_DIR", str(outputs))
    markdown = render_analysis_report(_case(), _review())

    first = write_analysis_report("../../event-17", "review-17", markdown)
    repeated = write_analysis_report("../../event-17", "review-17", markdown)
    different = write_analysis_report(
        "../../event-17", "review-17", markdown + "\n追加正文。\n"
    )

    content = markdown.encode("utf-8")
    assert first.path == repeated.path
    assert first.path != different.path
    assert first.path.is_relative_to(outputs.resolve())
    assert first.path.parent.name.startswith("event-")
    assert first.path.read_bytes() == content
    assert first.path.name.startswith("report-review-17-")
    assert first.sha256 == hashlib.sha256(content).hexdigest()
    assert first.size_bytes == len(content)
    assert REPORT_TEMPLATE_VERSION


def test_report_writer_rejects_path_like_review_id(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("SSV_OUTPUTS_DIR", str(tmp_path / "outputs"))

    artifact = write_analysis_report("event-1", "../../review", "report")

    assert artifact.path.is_relative_to((tmp_path / "outputs").resolve())
    assert artifact.path.parent.name == "event-1"
    assert ".." not in artifact.path.name


def test_report_reader_validates_registered_artifact_metadata(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    outputs = tmp_path / "outputs"
    monkeypatch.setenv("SSV_OUTPUTS_DIR", str(outputs))
    artifact = write_analysis_report("event-1", "review-1", "# report\n")

    assert (
        read_analysis_report(
            str(artifact.path),
            sha256=artifact.sha256,
            size_bytes=artifact.size_bytes,
        )
        == "# report\n"
    )

    with pytest.raises(ValueError, match="size"):
        read_analysis_report(
            str(artifact.path),
            sha256=artifact.sha256,
            size_bytes=artifact.size_bytes + 1,
        )
    with pytest.raises(ValueError, match="hash"):
        read_analysis_report(
            str(artifact.path),
            sha256="0" * 64,
            size_bytes=artifact.size_bytes,
        )


def test_report_reader_rejects_symlink_and_path_escape(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    outputs = tmp_path / "outputs"
    monkeypatch.setenv("SSV_OUTPUTS_DIR", str(outputs))
    artifact = write_analysis_report("event-1", "review-1", "# report\n")
    outside = tmp_path / "outside.md"
    outside.write_text("outside\n", encoding="utf-8")
    link = outputs / "link.md"
    link.symlink_to(outside)

    with pytest.raises(ValueError, match="regular file"):
        read_analysis_report(
            str(link),
            sha256=artifact.sha256,
            size_bytes=artifact.size_bytes,
        )
    with pytest.raises(ValueError, match="escapes outputs root"):
        read_analysis_report(
            str(outside),
            sha256=artifact.sha256,
            size_bytes=artifact.size_bytes,
        )
