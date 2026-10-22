from __future__ import annotations

import hashlib
import io
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator

import pytest

from ssv_agent.artifact_storage import (
    ArtifactIntegrityError,
    ArtifactStoreUnavailableError,
)
from ssv_agent.artifact_worker import ArtifactUploadWorker
from ssv_agent.config import ArtifactStorageConfig
from ssv_agent.event_store import (
    ArtifactKind,
    ArtifactRecord,
    EventLedger,
    JobState,
    StagedArtifact,
)
from ssv_agent.result import ReviewResult
from ssv_agent.review_context import ReviewContext

pytest_plugins = ()


class FakeStore:
    def __init__(self) -> None:
        self.objects: dict[str, bytes] = {}
        self.failure: Exception | None = None
        self.closed = False

    def ensure_uploaded(self, artifact: ArtifactRecord, source: io.BytesIO) -> None:
        if self.failure is not None:
            raise self.failure
        data = source.read()
        existing = self.objects.get(artifact.object_key)
        if existing is not None and existing != data:
            raise ArtifactIntegrityError("conflicting object")
        self.objects[artifact.object_key] = data

    @contextmanager
    def open(self, artifact: ArtifactRecord) -> Iterator[io.BytesIO]:
        if self.failure is not None:
            raise self.failure
        yield io.BytesIO(self.objects[artifact.object_key])

    def close(self) -> None:
        self.closed = True


def _storage(monkeypatch: pytest.MonkeyPatch) -> ArtifactStorageConfig:
    monkeypatch.setenv("SSV_MINIO_ACCESS_KEY", "access")
    monkeypatch.setenv("SSV_MINIO_SECRET_KEY", "secret")
    return ArtifactStorageConfig(backend="minio")


def _setup(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> tuple[Path, ArtifactStorageConfig, Path, FakeStore, ArtifactRecord]:
    storage = _storage(monkeypatch)
    outputs = tmp_path / "outputs"
    staging_dir = outputs / "case-1" / "staging" / "artifact-1"
    staging_dir.mkdir(parents=True)
    data = b"review-json"
    staging = staging_dir / "review.json"
    staging.write_bytes(data)
    sha256 = hashlib.sha256(data).hexdigest()
    db_path = tmp_path / "events.db"
    store = FakeStore()
    with EventLedger(db_path, artifact_storage=storage, outputs_dir=outputs) as ledger:
        ledger.record(
            ReviewContext(
                event_id="case-1",
                ingress_id="1-0",
                source="camera-1",
                timestamp_ms=1000,
                frame_id=4,
            )
        )
        artifact = ledger.register_artifact(
            StagedArtifact(
                artifact_id="artifact-1",
                event_id="case-1",
                kind=ArtifactKind.REVIEW_JSON,
                path=str(staging),
                sha256=sha256,
                size_bytes=len(data),
                mime_type="application/json",
            ),
            bucket="ssv-artifacts",
            object_key=f"ssv/v1/events/token/review_json/artifact-1/{sha256}.json",
        )
    return db_path, storage, outputs, store, artifact


def _worker(
    db_path: Path,
    storage: ArtifactStorageConfig,
    outputs: Path,
    store: FakeStore,
    *,
    max_retries: int = 3,
) -> ArtifactUploadWorker:
    return ArtifactUploadWorker(
        ledger_factory=lambda: EventLedger(
            db_path, artifact_storage=storage, outputs_dir=outputs
        ),
        store=store,
        worker_id="uploader",
        lease_ms=30_000,
        max_retries=max_retries,
        retry_delay_ms=0,
        poll_interval_seconds=0.01,
        staging_roots=(outputs,),
    )


def test_minio_outage_retries_upload_without_touching_other_workers(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_path, storage, outputs, store, artifact = _setup(tmp_path, monkeypatch)
    store.failure = ArtifactStoreUnavailableError("offline")

    assert _worker(db_path, storage, outputs, store).run_once() is True

    with EventLedger(db_path, artifact_storage=storage) as ledger:
        job = ledger.get_artifact_upload_job(artifact.artifact_id)
        assert job is not None
        assert job.state is JobState.PENDING
        assert job.attempts == 1
        assert job.error_code == "artifact_store_unavailable"
        assert ledger.get_artifact(artifact.artifact_id).uploaded_ms is None


def test_upload_completes_then_removes_verified_staging(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_path, storage, outputs, store, artifact = _setup(tmp_path, monkeypatch)

    assert _worker(db_path, storage, outputs, store).run_once() is True

    with EventLedger(db_path, artifact_storage=storage) as ledger:
        job = ledger.get_artifact_upload_job(artifact.artifact_id)
        stored = ledger.get_artifact(artifact.artifact_id)
        assert job is not None and job.state is JobState.COMPLETED
        assert stored is not None
        assert stored.uploaded_ms is not None
        assert stored.staging_path is None
        assert stored.local_cleaned_ms is not None
    assert store.objects[artifact.object_key] == b"review-json"
    assert not (outputs / "case-1" / "staging" / "artifact-1" / "review.json").exists()


def test_uploaded_staging_cleanup_recovers_after_restart(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_path, storage, outputs, store, artifact = _setup(tmp_path, monkeypatch)
    staged = Path(artifact.staging_path or "")
    with EventLedger(db_path, artifact_storage=storage) as ledger:
        job = ledger.claim_artifact_upload_job("uploader", lease_ms=30_000)
        assert job is not None
        ledger.complete_artifact_upload_job(job, "uploader")
    staged.unlink()

    assert _worker(db_path, storage, outputs, store).run_once() is True

    with EventLedger(db_path, artifact_storage=storage) as ledger:
        cleaned = ledger.get_artifact(artifact.artifact_id)
    assert cleaned is not None and cleaned.local_cleaned_ms is not None


def test_tampered_staging_fails_without_retry(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_path, storage, outputs, store, artifact = _setup(tmp_path, monkeypatch)
    Path(artifact.staging_path or "").write_bytes(b"tampered")

    assert _worker(db_path, storage, outputs, store, max_retries=3).run_once() is True

    with EventLedger(db_path, artifact_storage=storage) as ledger:
        job = ledger.get_artifact_upload_job(artifact.artifact_id)
        stored = ledger.get_artifact(artifact.artifact_id)
    assert job is not None and job.state is JobState.DEAD
    assert job.error_code == "artifact_integrity_error"
    assert stored is not None and stored.uploaded_ms is None


def test_missing_staging_source_fails_without_retry(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_path, storage, outputs, store, artifact = _setup(tmp_path, monkeypatch)
    Path(artifact.staging_path or "").unlink()

    assert _worker(db_path, storage, outputs, store).run_once() is True

    with EventLedger(db_path, artifact_storage=storage) as ledger:
        job = ledger.get_artifact_upload_job(artifact.artifact_id)
    assert job is not None and job.state is JobState.DEAD
    assert job.error_code == "artifact_missing"


def _write_cli_config(tmp_path: Path, db_path: Path) -> Path:
    path = tmp_path / "ssv.yaml"
    path.write_text(
        'version: "2.0"\n'
        "agent:\n"
        f'  event_db_path: "{db_path}"\n'
        "  evidence_roots: []\n",
        encoding="utf-8",
    )
    return path


def _invoke_cli(monkeypatch, argv: list[str]) -> None:
    import sys

    from ssv_agent import cli

    monkeypatch.setattr(cli, "find_dotenv", lambda: "")
    monkeypatch.setattr(cli, "load_dotenv", lambda _: False)
    monkeypatch.setattr(sys, "argv", argv)
    cli.main()


def test_artifact_retry_only_requeues_dead_job(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_path, storage, outputs, store, artifact = _setup(tmp_path, monkeypatch)
    with EventLedger(db_path, artifact_storage=storage) as ledger:
        job = ledger.claim_artifact_upload_job("uploader", lease_ms=30_000)
        assert job is not None
        ledger.fail_artifact_upload_job(
            job,
            "uploader",
            error="offline",
            max_retries=1,
            retry_delay_ms=0,
            error_code="artifact_store_unavailable",
        )
    config_path = _write_cli_config(tmp_path, db_path)

    _invoke_cli(
        monkeypatch,
        [
            "ssv-agent",
            "--config",
            str(config_path),
            "artifacts",
            "retry",
            "--artifact-id",
            artifact.artifact_id,
        ],
    )

    with EventLedger(db_path, artifact_storage=storage) as ledger:
        retried = ledger.get_artifact_upload_job(artifact.artifact_id)
    assert retried is not None
    assert retried.state is JobState.PENDING
    assert retried.attempts == 0


def test_artifact_retry_rejects_pending_job(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_path, storage, outputs, store, artifact = _setup(tmp_path, monkeypatch)
    config_path = _write_cli_config(tmp_path, db_path)

    with pytest.raises(SystemExit):
        _invoke_cli(
            monkeypatch,
            [
                "ssv-agent",
                "--config",
                str(config_path),
                "artifacts",
                "retry",
                "--artifact-id",
                artifact.artifact_id,
            ],
        )

    with EventLedger(db_path, artifact_storage=storage) as ledger:
        job = ledger.get_artifact_upload_job(artifact.artifact_id)
    assert job is not None and job.state is JobState.PENDING


def _recording_artifacts(root: Path):
    from ssv_agent.evidence_provider import EvidenceArtifact

    derived = root / "derived" / "case-1"
    derived.mkdir(parents=True)
    artifacts = []
    for kind, name, mime in (
        ("clip", "context.mp4", "video/mp4"),
        ("frame", "frame-01.jpg", "image/jpeg"),
        ("frame", "frame-02.jpg", "image/jpeg"),
        ("frame", "frame-03.jpg", "image/jpeg"),
    ):
        path = derived / name
        path.write_bytes(name.encode("utf-8"))
        artifacts.append(
            EvidenceArtifact(
                kind=kind,
                path=path,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                mime_type=mime,
                size=path.stat().st_size,
                mtime=path.stat().st_mtime,
            )
        )
    return tuple(artifacts)


def _recording_case(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    from ssv_agent.config import RecordingEvidenceConfig
    from ssv_agent.event_store import JobKind

    storage = _storage(monkeypatch)
    outputs = tmp_path / "outputs"
    evidence_root = tmp_path / "evidence"
    evidence_root.mkdir()
    db_path = tmp_path / "events.db"
    with EventLedger(
        db_path,
        evidence_roots=[str(evidence_root)],
        recording_evidence=RecordingEvidenceConfig(),
        artifact_storage=storage,
        outputs_dir=outputs,
    ) as ledger:
        ledger.record(
            ReviewContext(
                event_id="case-1",
                ingress_id="1-0",
                source="camera-1",
                timestamp_ms=1000,
                frame_id=4,
            )
        )
        job = ledger.claim_job(JobKind.EVIDENCE_EXTRACT, "extractor", lease_ms=30_000)
        assert job is not None
        ledger.complete_evidence_extract_job(
            job, "extractor", _recording_artifacts(evidence_root)
        )
    return db_path, storage, outputs, evidence_root


def test_upload_worker_uploads_extractor_frames_under_evidence_roots(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_path, storage, outputs, evidence_root = _recording_case(tmp_path, monkeypatch)
    store = FakeStore()
    worker = ArtifactUploadWorker(
        ledger_factory=lambda: EventLedger(
            db_path, artifact_storage=storage, outputs_dir=outputs
        ),
        store=store,
        worker_id="uploader",
        lease_ms=30_000,
        max_retries=3,
        retry_delay_ms=0,
        poll_interval_seconds=0.01,
        staging_roots=(outputs, evidence_root),
    )

    for _ in range(3):
        assert worker.run_once() is True

    with EventLedger(db_path, artifact_storage=storage) as ledger:
        artifacts = ledger.list_artifacts()
        jobs = [ledger.get_artifact_upload_job(item.artifact_id) for item in artifacts]
    assert len(artifacts) == 3
    assert all(item.uploaded_ms is not None for item in artifacts)
    assert all(item.local_cleaned_ms is not None for item in artifacts)
    assert all(item.staging_path is None for item in artifacts)
    assert all(job is not None and job.state is JobState.COMPLETED for job in jobs)
    assert len(store.objects) == 3
    derived = evidence_root / "derived" / "case-1"
    assert (derived / "context.mp4").exists()
    assert not (derived / "frame-01.jpg").exists()


def test_review_reads_staging_frame_under_evidence_root_when_minio_is_down(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from ssv_agent.workers import ReviewWorker

    db_path, storage, outputs, evidence_root = _recording_case(tmp_path, monkeypatch)
    monkeypatch.setenv("SSV_OUTPUTS_DIR", str(outputs))

    class OfflineStore:
        def ensure_uploaded(self, artifact, source) -> None:
            raise AssertionError("review must not upload")

        def open(self, artifact):
            raise AssertionError("MinIO is offline")

    observed: dict[str, bytes] = {}

    def runner(context, _rules=None):
        observed["bytes"] = Path(context.frame_path or "").read_bytes()
        return ReviewResult(
            verdict="uncertain",
            confidence=0.0,
            evidence_status="missing",
            explanation="staging fixture",
        ).model_dump_json()

    worker = ReviewWorker(
        ledger_factory=lambda: EventLedger(
            db_path,
            evidence_roots=[str(evidence_root)],
            artifact_storage=storage,
            outputs_dir=outputs,
        ),
        runner=runner,
        worker_id="reviewer",
        lease_ms=30_000,
        max_retries=3,
        retry_delay_ms=0,
        artifact_store=OfflineStore(),
    )

    assert worker.run_once() is True

    assert observed["bytes"] == b"frame-01.jpg"


def test_cleanup_failure_does_not_block_other_artifacts(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    import ssv_agent.artifact_worker as worker_module

    db_path, storage, outputs, store, first = _setup(tmp_path, monkeypatch)
    second_dir = outputs / "case-1" / "staging" / "artifact-2"
    second_dir.mkdir(parents=True)
    second_path = second_dir / "review.json"
    second_path.write_bytes(b"second-payload")
    with EventLedger(db_path, artifact_storage=storage, outputs_dir=outputs) as ledger:
        second = ledger.register_artifact(
            StagedArtifact(
                artifact_id="artifact-2",
                event_id="case-1",
                kind=ArtifactKind.REVIEW_JSON,
                path=str(second_path),
                sha256=hashlib.sha256(b"second-payload").hexdigest(),
                size_bytes=len(b"second-payload"),
                mime_type="application/json",
            ),
            bucket="ssv-artifacts",
            object_key="ssv/v1/events/token/review_json/artifact-2/bbbb.json",
        )
        for _ in range(2):
            job = ledger.claim_artifact_upload_job("uploader", lease_ms=30_000)
            assert job is not None
            ledger.complete_artifact_upload_job(job, "uploader")

    real_remove = worker_module._remove_verified_staging

    def flaky_remove(artifact, roots) -> None:
        if artifact.artifact_id == first.artifact_id:
            raise PermissionError("immutable")
        real_remove(artifact, roots)

    monkeypatch.setattr(worker_module, "_remove_verified_staging", flaky_remove)
    worker = _worker(db_path, storage, outputs, store)

    assert worker.run_once() is True

    with EventLedger(db_path, artifact_storage=storage) as ledger:
        blocked = ledger.get_artifact(first.artifact_id)
        cleaned = ledger.get_artifact(second.artifact_id)
    assert blocked is not None and blocked.local_cleaned_ms is None
    assert cleaned is not None and cleaned.local_cleaned_ms is not None
