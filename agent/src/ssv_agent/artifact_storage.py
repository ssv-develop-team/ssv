"""产物存储的统一读取边界与稳定对象键。"""

from __future__ import annotations

import hashlib
import os
import stat
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import ContextManager, IO, Iterator, Protocol

from ssv_agent.config import SsvConfig
from ssv_agent.event_store.ledger import ArtifactKind, ArtifactRecord, EventLedger

_CHUNK_SIZE = 1024 * 1024
_SPOOL_LIMIT_BYTES = 8 * 1024 * 1024
_ARTIFACT_EXTENSIONS = {
    ArtifactKind.REVIEW_JSON: "json",
    ArtifactKind.REPORT_MARKDOWN: "md",
}
_IMAGE_EXTENSIONS = {"image/jpeg": "jpg", "image/png": "png", "image/webp": "webp"}


class ArtifactStorageError(Exception):
    """产物读取或写入失败的基类。"""


class ArtifactStoreUnavailableError(ArtifactStorageError):
    """远端对象存储暂时不可访问。"""


class ArtifactNotFoundError(ArtifactStorageError):
    """已登记的本地暂存或远端对象确定不存在。"""


class ArtifactIntegrityError(ArtifactStorageError):
    """产物的大小、摘要或内容类型与登记不一致。"""


class ArtifactAccessError(ArtifactStorageError):
    """产物路径不安全或不在允许根目录内。"""


class ArtifactObjectStore(Protocol):
    """产物对象存储的最小边界。"""

    def ensure_uploaded(self, artifact: ArtifactRecord, source: IO[bytes]) -> None: ...

    def open(self, artifact: ArtifactRecord) -> ContextManager[IO[bytes]]: ...


def object_extension(kind: ArtifactKind, mime_type: str) -> str:
    """返回对象键使用的固定扩展名。"""
    if kind == ArtifactKind.EVIDENCE_FRAME:
        extension = _IMAGE_EXTENSIONS.get(mime_type.lower())
        if extension is None:
            raise ArtifactIntegrityError(f"unsupported frame mime type: {mime_type}")
        return extension
    return _ARTIFACT_EXTENSIONS[kind]


def build_object_key(prefix: str, artifact_id: str, kind: ArtifactKind, sha256: str,
                     mime_type: str, *, event_id: str) -> str:
    """按不可变规则构建对象键。"""
    event_token = hashlib.sha256(event_id.encode("utf-8")).hexdigest()
    extension = object_extension(kind, mime_type)
    return f"{prefix}/events/{event_token}/{kind.value}/{artifact_id}/{sha256}.{extension}"


def create_artifact_reader(ledger: EventLedger, config: SsvConfig) -> "ArtifactReader":
    """按当前部署配置构造读取边界；客户端在首次远端访问时才建立连接。"""
    from ssv_agent.minio_store import MinioObjectStore

    outputs_root = Path(os.getenv("SSV_OUTPUTS_DIR", "outputs")).resolve(strict=False)
    roots = (outputs_root, *ledger.evidence_roots)
    store = MinioObjectStore(config.agent.storage.minio)
    return ArtifactReader(
        ledger,
        store,
        staging_roots=roots,
        legacy_roots=roots,
    )


class ArtifactReader:
    """按产物登记、上传状态和历史契约读取可校验字节。"""

    def __init__(
        self,
        ledger: EventLedger,
        store: ArtifactObjectStore,
        *,
        staging_roots: tuple[Path, ...],
        legacy_roots: tuple[Path, ...],
    ) -> None:
        self._ledger = ledger
        self._store = store
        self._staging_roots = tuple(path.resolve(strict=False) for path in staging_roots)
        self._legacy_roots = tuple(path.resolve(strict=False) for path in legacy_roots)

    @contextmanager
    def open(
        self,
        artifact_id: str | None,
        *,
        legacy_path: str | None = None,
        expected_sha256: str | None = None,
        expected_size: int | None = None,
    ) -> Iterator[IO[bytes]]:
        """上下文内有效；调用方不得在上下文外继续使用该流。"""
        if artifact_id is None:
            if not legacy_path:
                raise ArtifactNotFoundError("no artifact or legacy path is registered")
            with self._open_local(
                legacy_path,
                roots=self._legacy_roots,
                expected_sha256=expected_sha256,
                expected_size=expected_size,
            ) as stream:
                yield stream
            return

        artifact = self._ledger.get_artifact(artifact_id)
        if artifact is None:
            raise ArtifactNotFoundError(f"unknown artifact: {artifact_id}")

        if artifact.uploaded_ms is not None:
            with self._open_remote(artifact) as stream:
                yield stream
            return

        if artifact.staging_path:
            with self._open_local(
                artifact.staging_path,
                roots=self._staging_roots,
                expected_sha256=artifact.sha256,
                expected_size=artifact.size_bytes,
            ) as stream:
                yield stream
            return

        raise ArtifactStoreUnavailableError(
            f"artifact {artifact_id} has no readable staging or uploaded object"
        )

    @contextmanager
    def _open_remote(self, artifact: ArtifactRecord) -> Iterator[IO[bytes]]:
        with self._store.open(artifact) as stream:
            with tempfile.SpooledTemporaryFile(
                max_size=_SPOOL_LIMIT_BYTES, mode="w+b"
            ) as buffer:
                digest = hashlib.sha256()
                total = 0
                while True:
                    chunk = stream.read(_CHUNK_SIZE)
                    if not chunk:
                        break
                    digest.update(chunk)
                    total += len(chunk)
                    buffer.write(chunk)
                self._verify(artifact.sha256, artifact.size_bytes, digest.hexdigest(), total)
                buffer.seek(0)
                yield buffer

    @contextmanager
    def _open_local(
        self,
        path: str,
        *,
        roots: tuple[Path, ...],
        expected_sha256: str | None,
        expected_size: int | None,
    ) -> Iterator[IO[bytes]]:
        fd, _ = _open_regular_file(path, roots)
        with os.fdopen(fd, "rb") as raw, tempfile.SpooledTemporaryFile(
            max_size=_SPOOL_LIMIT_BYTES, mode="w+b"
        ) as buffer:
            digest = hashlib.sha256()
            total = 0
            while True:
                chunk = raw.read(_CHUNK_SIZE)
                if not chunk:
                    break
                digest.update(chunk)
                total += len(chunk)
                buffer.write(chunk)
            self._verify(expected_sha256, expected_size, digest.hexdigest(), total)
            buffer.seek(0)
            yield buffer

    @staticmethod
    def _verify(
        expected_sha256: str | None,
        expected_size: int | None,
        actual_sha256: str,
        actual_size: int,
    ) -> None:
        if expected_size is not None and actual_size != expected_size:
            raise ArtifactIntegrityError("artifact size does not match its registration")
        if expected_sha256 is not None and actual_sha256 != expected_sha256:
            raise ArtifactIntegrityError("artifact sha256 does not match its registration")


def _open_regular_file(path: str, roots: tuple[Path, ...]) -> tuple[int, Path]:
    if not roots:
        raise ArtifactAccessError("no allowed artifact roots are configured")
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except FileNotFoundError as exc:
        raise ArtifactNotFoundError(f"artifact source not found: {path}") from exc
    except OSError as exc:
        raise ArtifactAccessError("artifact source cannot be opened safely") from exc
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise ArtifactAccessError("artifact source is not a regular file")
        resolved = Path(os.path.realpath(path))
        if not any(resolved == root or root in resolved.parents for root in roots):
            raise ArtifactAccessError("artifact source is outside the allowed roots")
        return fd, resolved
    except Exception:
        os.close(fd)
        raise
