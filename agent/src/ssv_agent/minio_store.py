"""MinIO/S3 对象适配；隔离 SDK 细节并校验真实字节。"""

from __future__ import annotations

import hashlib
import io
import os
import tempfile
from contextlib import contextmanager
from typing import Any, IO, Iterator
from urllib.parse import urlparse

import urllib3
from minio import Minio

from ssv_agent.artifact_storage import (
    ArtifactIntegrityError,
    ArtifactNotFoundError,
    ArtifactStoreUnavailableError,
)
from ssv_agent.config import MinioStorageConfig
from ssv_agent.event_store.ledger import ArtifactRecord

_CHUNK_SIZE = 1024 * 1024
_SPOOL_LIMIT_BYTES = 8 * 1024 * 1024
_MISSING_CODES = frozenset({"NoSuchKey", "NoSuchObject", "NoSuchVersion"})
_BUCKET_EXISTS_CODES = frozenset({"BucketAlreadyOwnedByYou", "BucketAlreadyExists"})
_INTERNAL_RETRIES = 2


class _BucketMissing(Exception):
    """bucket 尚未创建；首次上传时按需创建。"""


class MinioObjectStore:
    """以不可变对象键写入并校验产物内容。"""

    def __init__(
        self,
        config: MinioStorageConfig,
        *,
        client: Any | None = None,
    ) -> None:
        self._config = config
        self._client = client if client is not None else _build_client(config)

    def ensure_uploaded(self, artifact: ArtifactRecord, source: IO[bytes]) -> None:
        """对象不存在则上传；已存在则读取并校验，绝不覆盖不同内容。"""
        try:
            existing = self._stat(artifact)
            bucket_missing = False
        except _BucketMissing:
            existing = None
            bucket_missing = True
        if existing is not None:
            remote = self._read_all(artifact)
            self._verify(artifact, remote, existing.content_type)
            return

        data = source.read()
        self._verify(artifact, data, artifact.mime_type)
        if bucket_missing:
            self._ensure_bucket(artifact.bucket)
        try:
            self._put(artifact, data)
        except Exception as exc:
            if getattr(exc, "code", None) == "NoSuchBucket":
                # 并发或首次启动场景：bucket 可能在上传瞬间才创建完成。
                self._ensure_bucket(artifact.bucket)
                self._put(artifact, data)
            else:
                raise _translate(exc) from exc

        uploaded = self._stat(artifact)
        if uploaded is None:
            raise ArtifactStoreUnavailableError("uploaded object is not visible")
        remote = self._read_all(artifact)
        self._verify(artifact, remote, uploaded.content_type)

    @contextmanager
    def open(self, artifact: ArtifactRecord) -> Iterator[IO[bytes]]:
        """读取远端对象并在交给调用方前校验真实字节。"""
        try:
            response = self._client.get_object(artifact.bucket, artifact.object_key)
        except Exception as exc:
            raise _translate(exc) from exc

        try:
            with tempfile.SpooledTemporaryFile(
                max_size=_SPOOL_LIMIT_BYTES, mode="w+b"
            ) as buffer:
                while True:
                    chunk = response.read(_CHUNK_SIZE)
                    if not chunk:
                        break
                    buffer.write(chunk)
                buffer.seek(0)
                data = buffer.read()
                content_type = response.headers.get("Content-Type")
        finally:
            response.close()
            response.release_conn()

        self._verify(artifact, data, content_type)
        with tempfile.SpooledTemporaryFile(max_size=_SPOOL_LIMIT_BYTES, mode="w+b") as buffer:
            buffer.write(data)
            buffer.seek(0)
            yield buffer

    def close(self) -> None:
        """释放在途连接池；不删除任何远端对象。"""
        http = getattr(self._client, "_http", None)
        clear = getattr(http, "clear", None)
        if callable(clear):
            clear()

    def _put(self, artifact: ArtifactRecord, data: bytes) -> None:
        self._client.put_object(
            artifact.bucket,
            artifact.object_key,
            io.BytesIO(data),
            length=len(data),
            content_type=artifact.mime_type,
        )

    def _ensure_bucket(self, bucket: str) -> None:
        """幂等创建私有 bucket；对象权限由部署凭据决定。"""
        try:
            self._client.make_bucket(bucket)
        except Exception as exc:
            if getattr(exc, "code", None) not in _BUCKET_EXISTS_CODES:
                raise _translate(exc) from exc

    def _stat(self, artifact: ArtifactRecord) -> Any | None:
        try:
            return self._client.stat_object(artifact.bucket, artifact.object_key)
        except Exception as exc:
            code = getattr(exc, "code", None)
            if code in _MISSING_CODES:
                return None
            if code == "NoSuchBucket":
                raise _BucketMissing from exc
            raise _translate(exc) from exc

    def _read_all(self, artifact: ArtifactRecord) -> bytes:
        try:
            response = self._client.get_object(artifact.bucket, artifact.object_key)
        except Exception as exc:
            raise _translate(exc) from exc
        try:
            return response.read()
        except Exception as exc:
            raise ArtifactStoreUnavailableError("object read failed") from exc
        finally:
            response.close()
            response.release_conn()

    @staticmethod
    def _verify(artifact: ArtifactRecord, data: bytes, content_type: str | None) -> None:
        if len(data) != artifact.size_bytes:
            raise ArtifactIntegrityError("remote object size does not match registration")
        if hashlib.sha256(data).hexdigest() != artifact.sha256:
            raise ArtifactIntegrityError("remote object sha256 does not match registration")
        actual_mime = (content_type or "").split(";")[0].strip().lower()
        if actual_mime != artifact.mime_type.lower():
            raise ArtifactIntegrityError("remote object content type does not match registration")


def _build_client(config: MinioStorageConfig) -> Minio:
    access_key = os.environ.get(config.access_key_env)
    secret_key = os.environ.get(config.secret_key_env)
    if not access_key or not secret_key:
        raise ArtifactStoreUnavailableError(
            "MinIO credentials are not available in the configured environment variables"
        )
    parsed = urlparse(config.endpoint)
    http_client = urllib3.PoolManager(
        timeout=urllib3.Timeout(
            connect=config.connect_timeout_seconds,
            read=config.read_timeout_seconds,
        ),
        retries=urllib3.Retry(total=_INTERNAL_RETRIES, backoff_factor=0.2),
    )
    return Minio(
        parsed.netloc,
        access_key=access_key,
        secret_key=secret_key,
        secure=parsed.scheme == "https",
        http_client=http_client,
    )


def _translate(exc: Exception) -> Exception:
    if getattr(exc, "code", None) in _MISSING_CODES:
        return ArtifactNotFoundError("object does not exist")
    return ArtifactStoreUnavailableError("object store is temporarily unavailable")
