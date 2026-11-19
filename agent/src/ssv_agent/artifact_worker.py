"""独立产物上传与本地暂存清理 worker。"""

from __future__ import annotations

import hashlib
import os
import stat
from pathlib import Path
from threading import Event, Lock, Thread

import structlog

from ssv_agent.artifact_storage import (
    ArtifactAccessError,
    ArtifactIntegrityError,
    ArtifactNotFoundError,
    ArtifactObjectStore,
    ArtifactReader,
    ArtifactStoreUnavailableError,
)
from ssv_agent.event_store import ArtifactRecord, ArtifactUploadJob, EventLedger, LeaseLostError
from ssv_agent.workers import LedgerFactory, _PollingWorker

logger = structlog.get_logger()

_CHUNK_SIZE = 1024 * 1024


class _ArtifactHeartbeat:
    """在独立连接上持续续租单个上传 claim。"""

    def __init__(
        self,
        *,
        ledger_factory: LedgerFactory,
        job: ArtifactUploadJob,
        worker_id: str,
        lease_ms: int,
    ) -> None:
        self._ledger_factory = ledger_factory
        self._job = job
        self._worker_id = worker_id
        self._lease_ms = lease_ms
        self._interval_seconds = lease_ms / 3_000
        self._stopping = Event()
        self._lost = Event()
        self._failure: BaseException | None = None
        self._failure_lock = Lock()
        self._started = False
        self._thread = Thread(
            target=self._run,
            name=f"ssv-artifact-heartbeat-{job.job_id}",
            daemon=True,
        )

    def start(self) -> None:
        self._thread.start()
        self._started = True

    def checkpoint(self) -> None:
        self._raise_if_unhealthy()
        try:
            self._renew()
        except LeaseLostError:
            self._lost.set()
            raise
        except BaseException as exc:
            self._failure = exc
            raise
        self._raise_if_unhealthy()

    def stop_and_join(self) -> None:
        self._stopping.set()
        if self._started:
            self._thread.join()

    def _run(self) -> None:
        while not self._stopping.wait(self._interval_seconds):
            try:
                self._renew()
            except LeaseLostError:
                self._lost.set()
                return
            except BaseException as exc:
                with self._failure_lock:
                    self._failure = exc
                return

    def _renew(self) -> None:
        with self._ledger_factory() as ledger:
            ledger.renew_artifact_upload_job(self._job, self._worker_id, self._lease_ms)

    def _raise_if_unhealthy(self) -> None:
        if self._lost.is_set():
            raise LeaseLostError(
                f"lost lease for artifact upload job {self._job.job_id}"
            )
        with self._failure_lock:
            failure = self._failure
        if failure is not None:
            raise LeaseLostError(
                f"lease heartbeat failed for artifact upload job {self._job.job_id}"
            ) from failure


class ArtifactUploadWorker(_PollingWorker):
    """领取上传任务、校验远端字节并清理本地暂存。"""

    def __init__(
        self,
        *,
        ledger_factory: LedgerFactory,
        store: ArtifactObjectStore,
        worker_id: str,
        lease_ms: int,
        max_retries: int,
        retry_delay_ms: int,
        poll_interval_seconds: float = 1.0,
        staging_roots: tuple[Path, ...] = (),
    ) -> None:
        super().__init__(
            worker_id=worker_id,
            poll_interval_seconds=poll_interval_seconds,
            job_kind="artifact_upload",
        )
        self._ledger_factory = ledger_factory
        self._store = store
        self._lease_ms = lease_ms
        self._max_retries = max_retries
        self._retry_delay_ms = retry_delay_ms
        self._staging_roots = tuple(path.resolve(strict=False) for path in staging_roots)

    def run_once(self) -> bool:
        """处理一条上传任务或一项待恢复的本地清理。"""
        with self._ledger_factory() as ledger:
            job = ledger.claim_artifact_upload_job(self._worker_id, self._lease_ms)
            if job is None:
                return self._recover_cleanup(ledger)

            artifact = ledger.get_artifact(job.artifact_id)
            if artifact is None:
                self._fail(
                    ledger,
                    job,
                    error="artifact registration is missing",
                    error_code="artifact_missing",
                    error_message="产物登记缺失，请检查账本。",
                    retryable=False,
                )
                return True

            heartbeat: _ArtifactHeartbeat | None = None
            try:
                if artifact.uploaded_ms is None and not artifact.staging_path:
                    raise ArtifactNotFoundError("artifact has no staging source")
                heartbeat = _ArtifactHeartbeat(
                    ledger_factory=self._ledger_factory,
                    job=job,
                    worker_id=self._worker_id,
                    lease_ms=self._lease_ms,
                )
                heartbeat.start()
                heartbeat.checkpoint()
                self._upload(ledger, artifact, job)
                heartbeat.stop_and_join()
                ledger.complete_artifact_upload_job(job, self._worker_id)
                if self._cleanup(ledger, artifact):
                    logger.info(
                        "artifact upload completed",
                        event_id=artifact.event_id,
                        artifact_id=artifact.artifact_id,
                        job_id=job.job_id,
                        kind=artifact.kind.value,
                    )
            except LeaseLostError:
                logger.info(
                    "artifact upload lease lost",
                    artifact_id=artifact.artifact_id,
                    job_id=job.job_id,
                    worker_id=self._worker_id,
                )
            except Exception as exc:
                error_code, error_message, retryable = _classify(exc)
                self._fail(
                    ledger,
                    job,
                    error=str(exc),
                    error_code=error_code,
                    error_message=error_message,
                    retryable=retryable,
                )
            finally:
                if heartbeat is not None:
                    heartbeat.stop_and_join()
            return True

    def _upload(
        self,
        ledger: EventLedger,
        artifact: ArtifactRecord,
        job: ArtifactUploadJob,
    ) -> None:
        reader = ArtifactReader(
            ledger,
            self._store,
            staging_roots=self._staging_roots,
            legacy_roots=self._staging_roots,
        )
        with reader.open(artifact.artifact_id) as source:
            self._store.ensure_uploaded(artifact, source)

    def _recover_cleanup(self, ledger: EventLedger) -> bool:
        pending = ledger.list_artifacts_pending_cleanup()
        handled = False
        for artifact in pending:
            # 单个失败文件不阻塞其它已上传产物的清理。
            if self._cleanup(ledger, artifact):
                handled = True
        return handled

    def _cleanup(self, ledger: EventLedger, artifact: ArtifactRecord) -> bool:
        if not artifact.staging_path:
            return ledger.mark_artifact_cleaned(artifact.artifact_id)
        try:
            _remove_verified_staging(artifact, self._staging_roots)
        except FileNotFoundError:
            pass
        except Exception as exc:
            logger.warning(
                "artifact_cleanup_failed",
                artifact_id=artifact.artifact_id,
                event_id=artifact.event_id,
                error=type(exc).__name__,
            )
            return False
        ledger.mark_artifact_cleaned(artifact.artifact_id)
        return True

    def _fail(
        self,
        ledger: EventLedger,
        job: ArtifactUploadJob,
        *,
        error: str,
        error_code: str,
        error_message: str,
        retryable: bool,
    ) -> None:
        try:
            state = ledger.fail_artifact_upload_job(
                job,
                self._worker_id,
                error=error,
                max_retries=self._max_retries if retryable else job.attempts,
                retry_delay_ms=self._retry_delay_ms if retryable else 0,
                error_code=error_code,
                error_message=error_message,
            )
        except LeaseLostError:
            logger.info(
                "artifact upload lease lost",
                artifact_id=job.artifact_id,
                job_id=job.job_id,
                worker_id=self._worker_id,
            )
            return
        logger.warning(
            "artifact upload failed",
            artifact_id=job.artifact_id,
            job_id=job.job_id,
            state=state.value,
            attempts=job.attempts,
            error_code=error_code,
        )

    def _poll_error(self, exc: Exception) -> str:
        return type(exc).__name__


def _classify(exc: Exception) -> tuple[str, str, bool]:
    """返回受控错误码、用户消息和是否可重试。"""
    if isinstance(exc, ArtifactIntegrityError):
        return "artifact_integrity_error", "产物完整性校验失败。", False
    if isinstance(exc, ArtifactNotFoundError):
        return "artifact_missing", "产物暂存文件缺失。", False
    if isinstance(exc, ArtifactAccessError):
        return "artifact_access_error", "产物暂存路径不可安全读取。", False
    if isinstance(exc, ArtifactStoreUnavailableError):
        return "artifact_store_unavailable", "对象存储暂不可用，稍后重试。", True
    return "artifact_upload_failed", "产物上传失败，请稍后重试。", True


def _remove_verified_staging(
    artifact: ArtifactRecord,
    roots: tuple[Path, ...],
) -> None:
    """只删除当前产物登记、位于允许根内且摘要仍匹配的普通文件。"""
    path = Path(artifact.staging_path or "")
    if not roots:
        raise ArtifactAccessError("no allowed staging roots are configured")
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags)
    try:
        details = os.fstat(fd)
        if not stat.S_ISREG(details.st_mode):
            raise ArtifactAccessError("staging source is not a regular file")
        resolved = Path(os.path.realpath(path))
        if not any(resolved == root or root in resolved.parents for root in roots):
            raise ArtifactAccessError("staging source is outside the allowed roots")
        digest = hashlib.sha256()
        while True:
            chunk = os.read(fd, _CHUNK_SIZE)
            if not chunk:
                break
            digest.update(chunk)
        if digest.hexdigest() != artifact.sha256:
            raise ArtifactIntegrityError("staging content changed before cleanup")
    finally:
        os.close(fd)
    os.unlink(path)
