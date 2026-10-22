from __future__ import annotations

import hashlib
import io
import os
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
from typing import Iterator

import pytest
from minio.error import S3Error

from ssv_agent.artifact_storage import (
    ArtifactAccessError,
    ArtifactIntegrityError,
    ArtifactNotFoundError,
    ArtifactReader,
    ArtifactStoreUnavailableError,
    build_object_key,
)
from ssv_agent.event_store import (
    ArtifactKind,
    EventLedger,
    StagedArtifact,
    StorageBackend,
)
from ssv_agent.event_store.ledger import ArtifactRecord
from ssv_agent.minio_store import MinioObjectStore
from ssv_agent.review_context import ReviewContext


def _context() -> ReviewContext:
    return ReviewContext.model_validate(
        {
            "event_id": "evt-1",
            "source": "camera-1",
            "timestamp_ms": 1_700_000_000_000,
            "frame_id": 42,
            "event_type": "person_without_helmet",
            "severity": "high",
            "detections": [],
        }
    )


def _record(data: bytes, *, mime_type: str = "application/json") -> ArtifactRecord:
    sha256 = hashlib.sha256(data).hexdigest()
    artifact_id = "artifact-1"
    return ArtifactRecord(
        artifact_id=artifact_id,
        event_id="evt-1",
        kind=ArtifactKind.REVIEW_JSON,
        storage_backend=StorageBackend.MINIO,
        bucket="ssv-artifacts",
        object_key=build_object_key(
            "ssv/v1", artifact_id, ArtifactKind.REVIEW_JSON, sha256, mime_type, event_id="evt-1"
        ),
        sha256=sha256,
        size_bytes=len(data),
        mime_type=mime_type,
        staging_path=None,
        created_ms=1,
        uploaded_ms=None,
        local_cleaned_ms=None,
    )


class FakeObjectStore:
    def __init__(self) -> None:
        self.objects: dict[str, tuple[bytes, str]] = {}
        self.opened_keys: list[str] = []
        self.unavailable = False

    def ensure_uploaded(self, artifact: ArtifactRecord, source: io.BytesIO) -> None:
        if self.unavailable:
            raise ArtifactStoreUnavailableError("offline")
        data = source.read()
        existing = self.objects.get(artifact.object_key)
        if existing is not None:
            if existing[0] != data:
                raise ArtifactIntegrityError("conflicting object")
            return
        self.objects[artifact.object_key] = (data, artifact.mime_type)

    @contextmanager
    def open(self, artifact: ArtifactRecord) -> Iterator[io.BytesIO]:
        if self.unavailable:
            raise ArtifactStoreUnavailableError("offline")
        entry = self.objects.get(artifact.object_key)
        if entry is None:
            raise ArtifactNotFoundError("missing object")
        self.opened_keys.append(artifact.object_key)
        yield io.BytesIO(entry[0])


@pytest.fixture()
def storage(tmp_path: Path) -> tuple[EventLedger, FakeObjectStore, ArtifactReader, Path]:
    ledger = EventLedger(tmp_path / "events.db")
    ledger.record(_context())
    outputs = tmp_path / "outputs"
    outputs.mkdir()
    staging = outputs / "staging" / "artifact-1"
    staging.mkdir(parents=True)
    store = FakeObjectStore()
    reader = ArtifactReader(
        ledger,
        store,
        staging_roots=(outputs,),
        legacy_roots=(outputs,),
    )
    return ledger, store, reader, staging


def _register(
    ledger: EventLedger,
    staging: Path,
    data: bytes,
    *,
    kind: ArtifactKind = ArtifactKind.REVIEW_JSON,
    mime_type: str = "application/json",
) -> ArtifactRecord:
    path = staging / ("review.json" if kind == ArtifactKind.REVIEW_JSON else "frame.jpg")
    path.write_bytes(data)
    sha256 = hashlib.sha256(data).hexdigest()
    staged = StagedArtifact(
        artifact_id="artifact-1",
        event_id="evt-1",
        kind=kind,
        path=str(path),
        sha256=sha256,
        size_bytes=len(data),
        mime_type=mime_type,
    )
    return ledger.register_artifact(
        staged,
        bucket="ssv-artifacts",
        object_key=build_object_key(
            "ssv/v1", "artifact-1", kind, sha256, mime_type, event_id="evt-1"
        ),
    )


def _complete_upload(ledger: EventLedger) -> None:
    job = ledger.claim_artifact_upload_job("uploader", lease_ms=30_000)
    assert job is not None
    ledger.complete_artifact_upload_job(job, "uploader")


def test_uploaded_artifact_reads_remote_even_while_staging_file_remains(
    storage: tuple[EventLedger, FakeObjectStore, ArtifactReader, Path],
) -> None:
    ledger, store, reader, staging = storage
    data = b"remote-payload"
    artifact = _register(ledger, staging, data)
    store.objects[artifact.object_key] = (data, artifact.mime_type)
    _complete_upload(ledger)

    with reader.open(artifact.artifact_id) as stream:
        assert stream.read() == data

    assert store.opened_keys == [artifact.object_key]
    assert (staging / "review.json").exists()


def test_pending_artifact_reads_verified_staging(
    storage: tuple[EventLedger, FakeObjectStore, ArtifactReader, Path],
) -> None:
    ledger, _store, reader, staging = storage
    data = b"staged-payload"
    artifact = _register(ledger, staging, data)

    with reader.open(artifact.artifact_id) as stream:
        assert stream.read() == data


def test_failed_upload_still_reads_registered_staging(
    storage: tuple[EventLedger, FakeObjectStore, ArtifactReader, Path],
) -> None:
    ledger, _store, reader, staging = storage
    data = b"staged-payload"
    artifact = _register(ledger, staging, data)
    job = ledger.claim_artifact_upload_job("uploader", lease_ms=30_000)
    assert job is not None
    ledger.fail_artifact_upload_job(
        job, "uploader", error="offline", max_retries=1, retry_delay_ms=0
    )

    with reader.open(artifact.artifact_id) as stream:
        assert stream.read() == data


def test_uploaded_artifact_does_not_fall_back_to_staging(
    storage: tuple[EventLedger, FakeObjectStore, ArtifactReader, Path],
) -> None:
    ledger, store, reader, staging = storage
    data = b"remote-payload"
    artifact = _register(ledger, staging, data)
    store.objects[artifact.object_key] = (data, artifact.mime_type)
    _complete_upload(ledger)
    store.unavailable = True

    with pytest.raises(ArtifactStoreUnavailableError):
        with reader.open(artifact.artifact_id):
            pass


def test_staging_tampering_is_detected(
    storage: tuple[EventLedger, FakeObjectStore, ArtifactReader, Path],
) -> None:
    ledger, _store, reader, staging = storage
    artifact = _register(ledger, staging, b"original")
    (staging / "review.json").write_bytes(b"tampered")

    with pytest.raises(ArtifactIntegrityError):
        with reader.open(artifact.artifact_id):
            pass


def test_staging_outside_allowed_roots_is_rejected(
    tmp_path: Path,
    storage: tuple[EventLedger, FakeObjectStore, ArtifactReader, Path],
) -> None:
    ledger, _store, reader, _staging = storage
    outside = tmp_path / "outside"
    outside.mkdir()
    data = b"outside"
    artifact = _register(ledger, outside, data)

    with pytest.raises(ArtifactAccessError):
        with reader.open(artifact.artifact_id):
            pass


def test_symlinked_staging_is_rejected(
    storage: tuple[EventLedger, FakeObjectStore, ArtifactReader, Path],
) -> None:
    ledger, _store, reader, staging = storage
    data = b"linked"
    artifact = _register(ledger, staging, data)
    target = staging / "review.json"
    real = staging / "real.json"
    target.rename(real)
    os.symlink(real, target)

    with pytest.raises(ArtifactAccessError):
        with reader.open(artifact.artifact_id):
            pass


def test_legacy_path_requires_matching_sha_and_inside_roots(
    storage: tuple[EventLedger, FakeObjectStore, ArtifactReader, Path],
) -> None:
    _ledger, _store, reader, staging = storage
    path = staging / "legacy.json"
    path.write_bytes(b"legacy")
    sha256 = hashlib.sha256(b"legacy").hexdigest()

    with reader.open(None, legacy_path=str(path), expected_sha256=sha256, expected_size=6) as stream:
        assert stream.read() == b"legacy"

    with pytest.raises(ArtifactIntegrityError):
        with reader.open(None, legacy_path=str(path), expected_sha256="b" * 64):
            pass


def test_object_key_is_deterministic_and_type_specific() -> None:
    sha256 = "a" * 64

    key = build_object_key(
        "ssv/v1", "artifact-1", ArtifactKind.EVIDENCE_FRAME, sha256, "image/jpeg", event_id="evt-1"
    )

    assert key == (
        f"ssv/v1/events/{hashlib.sha256(b'evt-1').hexdigest()}"
        f"/evidence_frame/artifact-1/{sha256}.jpg"
    )


class FakeStat:
    def __init__(self, content_type: str) -> None:
        self.content_type = content_type


class FakeResponse:
    def __init__(self, data: bytes, content_type: str) -> None:
        self._buffer = io.BytesIO(data)
        self.headers = {"Content-Type": content_type}
        self.closed = False
        self.released = False

    def read(self, size: int = -1) -> bytes:
        return self._buffer.read(size)

    def close(self) -> None:
        self.closed = True

    def release_conn(self) -> None:
        self.released = True


class FakeMinioClient:
    def __init__(self) -> None:
        self.objects: dict[str, tuple[bytes, str]] = {}
        self.put_calls = 0
        self.make_bucket_calls = 0
        self.bucket_exists = True
        self.failure: Exception | None = None

    def stat_object(self, bucket: str, key: str) -> FakeStat:
        if self.failure is not None:
            raise self.failure
        if not self.bucket_exists:
            raise _s3_error("NoSuchBucket")
        if key not in self.objects:
            raise _s3_error("NoSuchKey")
        return FakeStat(self.objects[key][1])

    def get_object(self, bucket: str, key: str) -> FakeResponse:
        if self.failure is not None:
            raise self.failure
        if not self.bucket_exists:
            raise _s3_error("NoSuchBucket")
        if key not in self.objects:
            raise _s3_error("NoSuchKey")
        return FakeResponse(*self.objects[key])

    def make_bucket(self, bucket: str) -> None:
        if self.failure is not None:
            raise self.failure
        self.make_bucket_calls += 1
        self.bucket_exists = True

    def put_object(self, bucket: str, key: str, data: io.BytesIO, *, length: int,
                   content_type: str) -> SimpleNamespace:
        if self.failure is not None:
            raise self.failure
        self.put_calls += 1
        self.objects[key] = (data.read(), content_type)
        return SimpleNamespace(etag="etag")


def _s3_error(code: str) -> S3Error:
    return S3Error(code, code, "resource", "request", "host", None)


def _minio_store(client: FakeMinioClient) -> MinioObjectStore:
    from ssv_agent.config import MinioStorageConfig

    return MinioObjectStore(MinioStorageConfig(), client=client)


def test_ensure_uploaded_writes_then_verifies_remote_bytes() -> None:
    client = FakeMinioClient()
    artifact = _record(b"payload")
    store = _minio_store(client)

    store.ensure_uploaded(artifact, io.BytesIO(b"payload"))

    assert client.put_calls == 1
    assert client.objects[artifact.object_key] == (b"payload", artifact.mime_type)


def test_existing_identical_object_is_reused_without_put() -> None:
    client = FakeMinioClient()
    artifact = _record(b"payload")
    client.objects[artifact.object_key] = (b"payload", artifact.mime_type)

    _minio_store(client).ensure_uploaded(artifact, io.BytesIO(b"payload"))

    assert client.put_calls == 0


def test_existing_object_with_wrong_sha_is_never_replaced() -> None:
    client = FakeMinioClient()
    artifact = _record(b"payload")
    wrong = b"other-content"
    client.objects[artifact.object_key] = (wrong, artifact.mime_type)

    with pytest.raises(ArtifactIntegrityError):
        _minio_store(client).ensure_uploaded(artifact, io.BytesIO(b"payload"))

    assert client.put_calls == 0
    assert client.objects[artifact.object_key][0] == wrong


def test_content_type_mismatch_is_rejected() -> None:
    client = FakeMinioClient()
    artifact = _record(b"payload")
    client.objects[artifact.object_key] = (b"payload", "text/plain")

    with pytest.raises(ArtifactIntegrityError):
        _minio_store(client).ensure_uploaded(artifact, io.BytesIO(b"payload"))


def test_connection_failure_maps_to_store_unavailable() -> None:
    client = FakeMinioClient()
    client.failure = OSError("connection refused")

    with pytest.raises(ArtifactStoreUnavailableError):
        _minio_store(client).ensure_uploaded(_record(b"payload"), io.BytesIO(b"payload"))

    with pytest.raises(ArtifactStoreUnavailableError):
        with _minio_store(client).open(_record(b"payload")):
            pass


def test_missing_object_maps_to_not_found_on_read() -> None:
    client = FakeMinioClient()
    artifact = _record(b"payload")

    with pytest.raises(ArtifactNotFoundError):
        with _minio_store(client).open(artifact):
            pass


def test_remote_read_releases_response_stream() -> None:
    client = FakeMinioClient()
    artifact = _record(b"payload")
    captured: list[FakeResponse] = []

    original = client.get_object

    def tracking_get(bucket: str, key: str) -> FakeResponse:
        response = original(bucket, key)
        captured.append(response)
        return response

    client.get_object = tracking_get  # type: ignore[method-assign]
    client.objects[artifact.object_key] = (b"payload", artifact.mime_type)

    with _minio_store(client).open(artifact) as stream:
        assert stream.read() == b"payload"

    assert captured[0].closed is True
    assert captured[0].released is True


def test_missing_bucket_is_created_before_first_upload() -> None:
    client = FakeMinioClient()
    client.bucket_exists = False
    artifact = _record(b"payload")

    _minio_store(client).ensure_uploaded(artifact, io.BytesIO(b"payload"))

    assert client.make_bucket_calls == 1
    assert client.put_calls == 1
    assert client.objects[artifact.object_key][0] == b"payload"
