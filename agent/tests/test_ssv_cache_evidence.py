from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest

from ssv_agent.config import EvidenceCacheConfig, RecordingEvidenceConfig
from ssv_agent.event_store import EventCase
from ssv_agent.evidence_provider import EvidenceExtractionError
from ssv_agent.ssv_cache_evidence import SsvCacheEvidenceExtractor


@dataclass(frozen=True)
class RecordedCall:
    args: tuple[str, ...]
    kwargs: dict[str, Any]


class FakeCommandRunner:
    def __init__(self) -> None:
        self.calls: list[RecordedCall] = []

    def run(self, args: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
        self.calls.append(RecordedCall(tuple(args), kwargs))
        Path(args[-1]).write_bytes(f"derived:{Path(args[-1]).name}".encode())
        return subprocess.CompletedProcess(args, 0, stdout="", stderr="")


def _case(
    *,
    event_id: str = "event-1",
    source_pts: int | None = 10_000_000_000,
    stream_generation: int | None = 7,
    evidence_window_start: int | None = None,
    evidence_window_end: int | None = None,
) -> EventCase:
    return EventCase(
        event_id=event_id,
        ingress_id=None,
        source="camera-1",
        timestamp_ms=1_700_000_000_000,
        frame_id=42,
        stream_generation=stream_generation,
        source_pts=source_pts,
        event_type="person_without_helmet",
        severity="high",
        rule_id=None,
        rule_version=None,
        rule_facts={},
        revision=0,
        status="pending",
        verdict=None,
        confidence=None,
        detections=(),
        evidence=(),
        review=None,
        evidence_window_start=evidence_window_start,
        evidence_window_end=evidence_window_end,
    )


def _extractor(
    evidence_root: Path,
    cache_root: Path,
    *,
    runner: FakeCommandRunner | None = None,
) -> SsvCacheEvidenceExtractor:
    evidence_root.mkdir(parents=True, exist_ok=True)
    cache_root.mkdir(parents=True, exist_ok=True)
    return SsvCacheEvidenceExtractor(
        RecordingEvidenceConfig(enabled=True),
        EvidenceCacheConfig(enabled=True, directory=str(cache_root)),
        [str(evidence_root)],
        runner=runner,
    )


def _write_segment(
    cache_root: Path,
    *,
    name: str,
    start: int,
    end: int,
    generation: int = 7,
    source: str = "camera-1",
    finalized: bool = True,
    include_timing: bool = True,
) -> Path:
    source_dir = cache_root / source
    source_dir.mkdir(parents=True, exist_ok=True)
    media = source_dir / name
    media.write_bytes(b"encoded-h264")
    payload: dict[str, object] = {
        "schema": "ssv.evidence-segment.v1",
        "source": source,
        "stream_generation": generation,
        "source_pts_start": start,
        "source_pts_end": end,
        "wall_clock_start_ms": 1_700_000_000_000,
        "wall_clock_end_ms": 1_700_000_010_000,
        "media": name,
        "finalized": finalized,
    }
    if not include_timing:
        payload.pop("source_pts_start")
        payload.pop("source_pts_end")
    media.with_suffix(".json").write_text(
        json.dumps(payload),
        encoding="utf-8",
    )
    return media


def _write_media_without_sidecar(cache_root: Path, name: str) -> Path:
    source_dir = cache_root / "camera-1"
    source_dir.mkdir(parents=True, exist_ok=True)
    media = source_dir / name
    media.write_bytes(b"active-h264")
    return media


def test_extracts_complete_same_generation_pts_window(tmp_path: Path) -> None:
    evidence_root = tmp_path / "evidence"
    cache_root = evidence_root / "cache"
    _write_segment(
        cache_root,
        name="segment-00000000.mp4",
        start=5_000_000_000,
        end=10_000_000_000,
    )
    _write_segment(
        cache_root,
        name="segment-00000001.mp4",
        start=10_000_000_000,
        end=15_000_000_000,
    )
    runner = FakeCommandRunner()

    artifacts = _extractor(evidence_root, cache_root, runner=runner).extract(_case())

    assert [artifact.kind for artifact in artifacts] == [
        "clip",
        "frame",
        "frame",
        "frame",
    ]
    assert all(artifact.path.parent == evidence_root / "derived" / "event-1" for artifact in artifacts)
    assert all(artifact.source_pts_start == 7_500_000_000 for artifact in artifacts)
    assert all(artifact.source_pts_end == 12_500_000_000 for artifact in artifacts)
    assert all(artifact.stream_generation == 7 for artifact in artifacts)
    assert len(runner.calls) == 4

    manifest = json.loads(
        (evidence_root / "derived" / "event-1" / "manifest.json").read_text()
    )
    assert manifest["inputs"] == [
        "segment-00000000.mp4",
        "segment-00000001.mp4",
    ]
    assert manifest["requested_source_pts"] == [7_500_000_000, 12_500_000_000]
    assert manifest["actual_source_pts"] == [5_000_000_000, 15_000_000_000]


def test_extract_uses_closed_episode_window_instead_of_single_event_window(
    tmp_path: Path,
) -> None:
    evidence_root = tmp_path / "evidence"
    cache_root = evidence_root / "cache"
    _write_segment(
        cache_root,
        name="segment-00000000.mp4",
        start=0,
        end=10_000_000_000,
    )
    _write_segment(
        cache_root,
        name="segment-00000001.mp4",
        start=10_000_000_000,
        end=20_000_000_000,
    )
    runner = FakeCommandRunner()

    artifacts = _extractor(evidence_root, cache_root, runner=runner).extract(
        _case(
            evidence_window_start=2_000_000_000,
            evidence_window_end=18_000_000_000,
        )
    )

    assert all(artifact.source_pts_start == 2_000_000_000 for artifact in artifacts)
    assert all(artifact.source_pts_end == 18_000_000_000 for artifact in artifacts)
    manifest = json.loads(
        (evidence_root / "derived" / "event-1" / "manifest.json").read_text()
    )
    assert manifest["requested_source_pts"] == [2_000_000_000, 18_000_000_000]


def test_extract_retries_when_segment_sidecar_is_missing(tmp_path: Path) -> None:
    evidence_root = tmp_path / "evidence"
    cache_root = evidence_root / "cache"
    _write_media_without_sidecar(cache_root, "segment-00000000.mp4")

    with pytest.raises(EvidenceExtractionError, match="cache window") as error:
        _extractor(evidence_root, cache_root).extract(_case())

    assert error.value.retryable is True


def test_extract_does_not_use_active_segment_without_finalized_sidecar(tmp_path: Path) -> None:
    evidence_root = tmp_path / "evidence"
    cache_root = evidence_root / "cache"
    _write_segment(
        cache_root,
        name="segment-00000000.mp4",
        start=5_000_000_000,
        end=10_000_000_000,
        finalized=False,
    )
    _write_segment(
        cache_root,
        name="segment-00000001.mp4",
        start=10_000_000_000,
        end=15_000_000_000,
    )

    with pytest.raises(EvidenceExtractionError, match="cache window") as error:
        _extractor(evidence_root, cache_root).extract(_case())

    assert error.value.retryable is True


@pytest.mark.parametrize("field", ["source_pts", "stream_generation"])
def test_extract_rejects_event_without_exact_timing_anchor(
    tmp_path: Path,
    field: str,
) -> None:
    evidence_root = tmp_path / "evidence"
    cache_root = evidence_root / "cache"
    runner = FakeCommandRunner()
    case = _case(**{field: None})

    with pytest.raises(EvidenceExtractionError, match="exact source PTS") as error:
        _extractor(evidence_root, cache_root, runner=runner).extract(case)

    assert error.value.retryable is False
    assert runner.calls == []


def test_extract_does_not_join_segments_from_different_generations(tmp_path: Path) -> None:
    evidence_root = tmp_path / "evidence"
    cache_root = evidence_root / "cache"
    _write_segment(
        cache_root,
        name="segment-00000000.mp4",
        start=5_000_000_000,
        end=10_000_000_000,
        generation=7,
    )
    _write_segment(
        cache_root,
        name="segment-00000001.mp4",
        start=10_000_000_000,
        end=15_000_000_000,
        generation=8,
    )

    with pytest.raises(EvidenceExtractionError, match="cache window") as error:
        _extractor(evidence_root, cache_root).extract(_case())

    assert error.value.retryable is True


def test_extract_rejects_cache_directory_outside_evidence_root(tmp_path: Path) -> None:
    evidence_root = tmp_path / "evidence"
    cache_root = tmp_path / "outside-cache"
    cache_root.mkdir()

    with pytest.raises(EvidenceExtractionError, match="outside evidence roots") as error:
        _extractor(evidence_root, cache_root).extract(_case())

    assert error.value.retryable is False


def test_extract_rejects_symlinked_cache_source_directory(tmp_path: Path) -> None:
    evidence_root = tmp_path / "evidence"
    cache_root = evidence_root / "cache"
    cache_root.mkdir(parents=True)
    outside = tmp_path / "outside"
    outside.mkdir()
    (cache_root / "camera-1").symlink_to(outside, target_is_directory=True)

    with pytest.raises(EvidenceExtractionError, match="source directory") as error:
        _extractor(evidence_root, cache_root).extract(_case())

    assert error.value.retryable is False


def test_extract_rejects_symlinked_cache_segment(tmp_path: Path) -> None:
    evidence_root = tmp_path / "evidence"
    cache_root = evidence_root / "cache"
    source_dir = cache_root / "camera-1"
    source_dir.mkdir(parents=True)
    outside = tmp_path / "outside.mp4"
    outside.write_bytes(b"outside")
    (source_dir / "segment-00000000.mp4").symlink_to(outside)
    (source_dir / "segment-00000000.json").write_text(
        json.dumps(
            {
                "schema": "ssv.evidence-segment.v1",
                "source": "camera-1",
                "stream_generation": 7,
                "source_pts_start": 5_000_000_000,
                "source_pts_end": 10_000_000_000,
                "media": "segment-00000000.mp4",
                "finalized": True,
            }
        ),
        encoding="utf-8",
    )

    with pytest.raises(EvidenceExtractionError, match="cache file") as error:
        _extractor(evidence_root, cache_root).extract(_case())

    assert error.value.retryable is False


def test_extract_rejects_dangling_recovery_symlink(tmp_path: Path) -> None:
    evidence_root = tmp_path / "evidence"
    cache_root = evidence_root / "cache"
    derived_parent = evidence_root / "derived"
    derived_parent.mkdir(parents=True)
    (derived_parent / ".event-1.recovery").symlink_to(
        tmp_path / "missing-recovery-directory",
        target_is_directory=True,
    )

    with pytest.raises(EvidenceExtractionError, match="derived output is unsafe") as error:
        _extractor(evidence_root, cache_root).extract(_case())

    assert error.value.retryable is False


def test_extract_reuses_complete_derived_artifacts_idempotently(tmp_path: Path) -> None:
    evidence_root = tmp_path / "evidence"
    cache_root = evidence_root / "cache"
    _write_segment(
        cache_root,
        name="segment-00000000.mp4",
        start=5_000_000_000,
        end=10_000_000_000,
    )
    _write_segment(
        cache_root,
        name="segment-00000001.mp4",
        start=10_000_000_000,
        end=15_000_000_000,
    )
    first_runner = FakeCommandRunner()
    second_runner = FakeCommandRunner()
    first = _extractor(evidence_root, cache_root, runner=first_runner).extract(_case())

    second = _extractor(evidence_root, cache_root, runner=second_runner).extract(_case())

    assert second == first
    assert len(first_runner.calls) == 4
    assert second_runner.calls == []


def test_extract_ignores_cache_segment_without_exact_pts_metadata(tmp_path: Path) -> None:
    evidence_root = tmp_path / "evidence"
    cache_root = evidence_root / "cache"
    _write_segment(
        cache_root,
        name="segment-00000000.mp4",
        start=5_000_000_000,
        end=15_000_000_000,
        include_timing=False,
    )

    with pytest.raises(EvidenceExtractionError, match="cache window") as error:
        _extractor(evidence_root, cache_root).extract(_case())

    assert error.value.retryable is True
