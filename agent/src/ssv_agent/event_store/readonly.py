"""EventLedger 的只读 Web 查询适配层。"""

from __future__ import annotations

import hashlib
import json
import os
import sqlite3
import stat
import struct
import time
from contextlib import contextmanager
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
from typing import Any, Iterator

from PIL import Image, UnidentifiedImageError

from ssv_agent.event_store.ledger import EvidenceRef, ReviewRecord, _normalise_evidence_roots

_IMAGE_MIME_BY_FORMAT = {
    "BMP": "image/bmp",
    "GIF": "image/gif",
    "JPEG": "image/jpeg",
    "MPO": "image/jpeg",
    "PNG": "image/png",
    "TIFF": "image/tiff",
    "WEBP": "image/webp",
}
_IMAGE_MIME_ALIASES = {"image/jpg": "image/jpeg", "image/x-png": "image/png"}
_SQLITE_MAX_INTEGER = 2**63 - 1


class StoreUnavailable(RuntimeError):
    """只读 SQLite 账本当前不可访问。"""


class EventNotFound(LookupError):
    """事件账本中没有该 event_id。"""


class EvidenceNotFound(LookupError):
    """事件下没有该稳定 evidence_id。"""


class EvidenceNotImage(RuntimeError):
    """登记证据属于 clip 或未登记为图片。"""


class EvidenceUnavailable(RuntimeError):
    """登记图片当前不可读取或未通过安全校验。"""


@dataclass(frozen=True)
class ImagePayload:
    data: bytes
    content_type: str


class ReadOnlyEventStore:
    """按请求打开 SQLite `mode=ro` 连接，不运行 EventLedger schema migration。"""

    def __init__(self, db_path: str | os.PathLike[str], evidence_roots: list[str]) -> None:
        self._db_path = Path(db_path).expanduser().resolve(strict=False)
        self._evidence_roots = _normalise_evidence_roots(evidence_roots, source="evidence_roots")

    @contextmanager
    def _connection(self) -> Iterator[sqlite3.Connection]:
        connection: sqlite3.Connection | None = None
        try:
            uri = f"{self._db_path.as_uri()}?mode=ro"
            connection = sqlite3.connect(uri, uri=True, timeout=5.0)
            connection.row_factory = sqlite3.Row
            connection.create_function(
                "CASEFOLD",
                1,
                lambda value: value.casefold() if value is not None else "",
                deterministic=True,
            )
            connection.execute("PRAGMA query_only = ON")
            connection.execute("PRAGMA busy_timeout = 5000")
            connection.execute("BEGIN")
            yield connection
        except sqlite3.Error as exc:
            raise StoreUnavailable from exc
        finally:
            if connection is not None:
                connection.close()

    def overview(self) -> dict[str, Any]:
        with self._connection() as connection:
            counts = connection.execute(
                """
                WITH latest_review AS (
                    SELECT reviews.event_id, reviews.verdict
                    FROM reviews
                    JOIN (
                        SELECT event_id, MAX(revision) AS revision
                        FROM reviews
                        GROUP BY event_id
                    ) AS latest
                        ON latest.event_id = reviews.event_id
                        AND latest.revision = reviews.revision
                )
                SELECT
                    COUNT(*) AS total_events,
                    SUM(CASE WHEN e.status = 'pending' THEN 1 ELSE 0 END) AS pending_events,
                    SUM(CASE WHEN e.status = 'manual_review' THEN 1 ELSE 0 END)
                        AS manual_review_events,
                    SUM(CASE WHEN e.status = 'completed' THEN 1 ELSE 0 END)
                        AS completed_events,
                    SUM(CASE WHEN latest_review.verdict = 'violation' THEN 1 ELSE 0 END)
                        AS violation_events
                FROM events AS e
                LEFT JOIN latest_review ON latest_review.event_id = e.event_id
                """
            ).fetchone()
            unavailable_evidence_count = connection.execute(
                "SELECT COUNT(*) FROM evidence WHERE available = 0"
            ).fetchone()[0]
            recent_rows = connection.execute(
                self._event_select(connection)
                + " ORDER BY e.timestamp_ms DESC, e.event_id ASC LIMIT 8"
            ).fetchall()

        return {
            "total_events": counts["total_events"] or 0,
            "pending_events": counts["pending_events"] or 0,
            "manual_review_events": counts["manual_review_events"] or 0,
            "completed_events": counts["completed_events"] or 0,
            "violation_events": counts["violation_events"] or 0,
            "unavailable_evidence_count": unavailable_evidence_count,
            "recent_events": [self._event_summary(row) for row in recent_rows],
        }

    def list_events(
        self,
        *,
        query: str | None,
        status: str | None,
        page: int,
        page_size: int,
    ) -> dict[str, Any]:
        conditions: list[str] = []
        parameters: list[object] = []
        if query:
            escaped_query = (
                query.casefold().replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")
            )
            pattern = f"%{escaped_query}%"
            conditions.append(
                "(CASEFOLD(COALESCE(e.event_id, '')) LIKE ? ESCAPE '\\' "
                "OR CASEFOLD(COALESCE(e.source, '')) LIKE ? ESCAPE '\\' "
                "OR CASEFOLD(COALESCE(e.event_type, '')) LIKE ? ESCAPE '\\')"
            )
            parameters.extend((pattern, pattern, pattern))
        if status is not None:
            conditions.append("e.status = ?")
            parameters.append(status)
        offset = min((page - 1) * page_size, _SQLITE_MAX_INTEGER)

        with self._connection() as connection:
            where_clause = f" WHERE {' AND '.join(conditions)}" if conditions else ""
            total = connection.execute(
                f"SELECT COUNT(*) FROM events AS e{where_clause}", parameters
            ).fetchone()[0]
            rows = connection.execute(
                self._event_select(connection)
                + where_clause
                + " ORDER BY e.timestamp_ms DESC, e.event_id ASC LIMIT ? OFFSET ?",
                [*parameters, page_size, offset],
            ).fetchall()

        return {
            "items": [self._event_summary(row) for row in rows],
            "total": total,
            "page": page,
            "page_size": page_size,
        }

    def get_event_detail(self, event_id: str) -> dict[str, Any] | None:
        with self._connection() as connection:
            event = connection.execute(
                self._event_select(connection) + " WHERE e.event_id = ?", (event_id,)
            ).fetchone()
            if event is None:
                return None
            detections = connection.execute(
                """
                SELECT class_name, class_id, confidence, track_id, track_state, occluded
                FROM detections
                WHERE event_id = ?
                ORDER BY id ASC
                """,
                (event_id,),
            ).fetchall()
            review_rows = connection.execute(
                """
                SELECT event_id, review_id, revision, created_ms, verdict, confidence,
                    evidence_status, explanation, evidence_ids_json, claims_json,
                    rule_citations_json, policy_id, model_id, result_path
                FROM reviews
                WHERE event_id = ?
                ORDER BY revision ASC
                """,
                (event_id,),
            ).fetchall()
            evidence_columns = self._evidence_columns(connection)
            evidence_rows = connection.execute(
                f"""
                SELECT event_id, evidence_id, kind, path, available, mime_type, size,
                    mtime, sha256, source_pts_start, source_pts_end, stream_generation,
                    {evidence_columns}
                FROM evidence
                WHERE event_id = ? AND evidence_id IS NOT NULL AND evidence_id != ''
                ORDER BY id ASC
                """,
                (event_id,),
            ).fetchall()

        reviews = [self._review_record(row) for row in review_rows]
        evidence_refs = [self._evidence_ref(row) for row in evidence_rows]
        return {
            "event": self._event_summary(event),
            "detections": [
                {
                    "class_name": row["class_name"],
                    "class_id": row["class_id"],
                    "confidence": row["confidence"],
                    "track_id": row["track_id"],
                    "track_state": row["track_state"],
                    "occluded": bool(row["occluded"]),
                }
                for row in detections
            ],
            "review_history": [
                {
                    "event_id": review.event_id,
                    "review_id": review.review_id,
                    "revision": review.revision,
                    "created_ms": review.created_ms,
                    "verdict": review.verdict,
                    "confidence": review.confidence,
                    "evidence_status": review.evidence_status,
                    "explanation": review.explanation,
                    "evidence_ids": list(review.evidence_ids),
                }
                for review in reviews
            ],
            "evidence": [
                {
                    "event_id": event_id,
                    "evidence_id": evidence.evidence_id,
                    "kind": evidence.kind,
                    "status": "available" if evidence.available else "unavailable",
                    "mime_type": evidence.mime_type,
                    "size": evidence.size,
                    **({"coverage": evidence.coverage} if row["coverage"] is not None else {}),
                }
                for row, evidence in zip(evidence_rows, evidence_refs, strict=True)
            ],
        }

    def read_frame(self, event_id: str, evidence_id: str) -> ImagePayload:
        with self._connection() as connection:
            event = connection.execute(
                "SELECT 1 FROM events WHERE event_id = ?", (event_id,)
            ).fetchone()
            if event is None:
                raise EventNotFound
            evidence_columns = self._evidence_columns(connection)
            evidence_rows = connection.execute(
                f"""
                SELECT event_id, evidence_id, kind, path, mime_type, available,
                    size, mtime, sha256, source_pts_start, source_pts_end,
                    stream_generation, {evidence_columns}
                FROM evidence
                WHERE event_id = ? AND evidence_id = ?
                """,
                (event_id, evidence_id),
            ).fetchmany(2)

        if not evidence_rows:
            raise EvidenceNotFound
        if len(evidence_rows) != 1:
            raise EvidenceUnavailable
        evidence = self._evidence_ref(evidence_rows[0])
        if evidence.kind != "frame":
            raise EvidenceNotImage
        declared_mime = self._declared_image_mime(evidence.mime_type)
        if not evidence.available:
            raise EvidenceUnavailable

        data, metadata = self._read_registered_file(evidence.path)
        if evidence.size is not None and evidence.size != metadata.st_size:
            raise EvidenceUnavailable
        if evidence.mtime is not None and evidence.mtime != metadata.st_mtime:
            raise EvidenceUnavailable
        if evidence.sha256 is not None:
            if hashlib.sha256(data).hexdigest() != evidence.sha256:
                raise EvidenceUnavailable

        content_type = self._validate_image(data, declared_mime)
        return ImagePayload(data=data, content_type=content_type)

    @staticmethod
    def _event_summary(row: sqlite3.Row) -> dict[str, Any]:
        return {
            "event_id": row["event_id"],
            "source": row["source"],
            "timestamp_ms": row["timestamp_ms"],
            "event_type": row["event_type"],
            "severity": row["severity"],
            "status": row["status"],
            "current_revision": row["current_revision"],
            "verdict": row["verdict"],
            "confidence": row["confidence"],
            "analysis_task": ReadOnlyEventStore._analysis_task(row),
        }

    @staticmethod
    def _event_select(connection: sqlite3.Connection) -> str:
        """按只读 schema 能力选择事件与持久任务字段。"""
        has_jobs = (
            connection.execute(
                "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'durable_jobs'"
            ).fetchone()
            is not None
        )
        job_fields = (
            "analysis_job.kind AS analysis_job_kind, "
            "analysis_job.state AS analysis_job_state, "
            "analysis_job.available_at_ms AS analysis_available_at_ms, "
            "analysis_job.lease_expires_ms AS analysis_lease_expires_ms"
            if has_jobs
            else "NULL AS analysis_job_kind, NULL AS analysis_job_state, "
            "NULL AS analysis_available_at_ms, NULL AS analysis_lease_expires_ms"
        )
        job_join = (
            """
            LEFT JOIN durable_jobs AS analysis_job
                ON analysis_job.job_id = (
                    SELECT candidate.job_id
                    FROM durable_jobs AS candidate
                    WHERE candidate.entity_id = e.event_id
                        AND candidate.kind IN ('review', 'evidence_extract')
                    ORDER BY candidate.entity_revision DESC,
                        CASE candidate.kind WHEN 'review' THEN 0 ELSE 1 END,
                        candidate.job_id DESC
                    LIMIT 1
                )
            """
            if has_jobs
            else ""
        )
        return f"""
            SELECT
                e.event_id,
                e.source,
                e.timestamp_ms,
                e.event_type,
                e.severity,
                e.status,
                latest.revision AS current_revision,
                latest.verdict AS verdict,
                latest.confidence AS confidence,
                {job_fields}
            FROM events AS e
            LEFT JOIN reviews AS latest
                ON latest.event_id = e.event_id
                AND latest.revision = (
                    SELECT MAX(history.revision)
                    FROM reviews AS history
                    WHERE history.event_id = e.event_id
                )
            {job_join}
        """

    @staticmethod
    def _analysis_task(row: sqlite3.Row) -> dict[str, str | None]:
        kind = row["analysis_job_kind"]
        state = row["analysis_job_state"]
        if kind is None or state is None:
            if row["current_revision"] is not None or row["status"] == "completed":
                return {"kind": "review", "state": "completed"}
            return {"kind": None, "state": "not_scheduled"}

        if state == "pending":
            available_at_ms = row["analysis_available_at_ms"]
            display_state = (
                "waiting"
                if available_at_ms is not None and available_at_ms > time.time_ns() // 1_000_000
                else "queued"
            )
        elif state == "processing":
            lease_expires_ms = row["analysis_lease_expires_ms"]
            display_state = (
                "processing"
                if lease_expires_ms is not None and lease_expires_ms > time.time_ns() // 1_000_000
                else "lease_expired"
            )
        elif state == "completed":
            display_state = "completed"
        elif state == "dead":
            display_state = "failed"
        else:  # pragma: no cover - durable_jobs constrains its state values
            display_state = "failed"
        return {"kind": kind, "state": display_state}

    @staticmethod
    def _decode_evidence_ids(value: str) -> list[str]:
        decoded = json.loads(value)
        if not isinstance(decoded, list) or not all(isinstance(item, str) for item in decoded):
            raise ValueError("invalid stored evidence references")
        return decoded

    @staticmethod
    def _evidence_columns(connection: sqlite3.Connection) -> str:
        """coverage 尚未登记的账本可读取；此处只检查 schema，不运行迁移。"""
        names = {row["name"] for row in connection.execute("PRAGMA table_info(evidence)")}
        return "coverage" if "coverage" in names else "NULL AS coverage"

    @staticmethod
    def _evidence_ref(row: sqlite3.Row) -> EvidenceRef:
        return EvidenceRef(
            evidence_id=row["evidence_id"],
            kind=row["kind"],
            path=row["path"],
            mime_type=row["mime_type"],
            available=bool(row["available"]),
            size=row["size"],
            mtime=row["mtime"],
            sha256=row["sha256"],
            source_pts_start=row["source_pts_start"],
            source_pts_end=row["source_pts_end"],
            stream_generation=row["stream_generation"],
            coverage=row["coverage"] or "unknown",
        )

    @classmethod
    def _review_record(cls, row: sqlite3.Row) -> ReviewRecord:
        return ReviewRecord(
            event_id=row["event_id"],
            review_id=row["review_id"],
            revision=row["revision"],
            created_ms=row["created_ms"],
            verdict=row["verdict"],
            confidence=row["confidence"],
            evidence_status=row["evidence_status"],
            explanation=row["explanation"],
            evidence_ids=tuple(cls._decode_evidence_ids(row["evidence_ids_json"])),
            claims=tuple(json.loads(row["claims_json"])),
            rule_citations=tuple(json.loads(row["rule_citations_json"])),
            policy_id=row["policy_id"],
            model_id=row["model_id"],
            result_path=row["result_path"],
        )

    @staticmethod
    def _declared_image_mime(mime_type: str | None) -> str | None:
        if mime_type is None:
            return None
        normalized = mime_type.split(";", 1)[0].strip().casefold()
        normalized = _IMAGE_MIME_ALIASES.get(normalized, normalized)
        if not normalized.startswith("image/"):
            raise EvidenceNotImage
        return normalized

    def _read_registered_file(self, stored_path: str) -> tuple[bytes, os.stat_result]:
        try:
            candidate = Path(stored_path)
            if not candidate.is_absolute() or ".." in candidate.parts:
                raise EvidenceUnavailable
            resolved = candidate.resolve(strict=True)
        except EvidenceUnavailable:
            raise
        except (OSError, RuntimeError, TypeError, ValueError) as exc:
            raise EvidenceUnavailable from exc

        for root in self._evidence_roots:
            try:
                relative_path = resolved.relative_to(root)
            except ValueError:
                continue
            if not relative_path.parts:
                raise EvidenceUnavailable
            return self._read_without_symlinks(root, relative_path)
        raise EvidenceUnavailable

    @staticmethod
    def _read_without_symlinks(root: Path, relative_path: Path) -> tuple[bytes, os.stat_result]:
        directory_flag = getattr(os, "O_DIRECTORY", 0)
        nofollow_flag = getattr(os, "O_NOFOLLOW", 0)
        close_on_exec_flag = getattr(os, "O_CLOEXEC", 0)
        directory_fd: int | None = None
        current_fd: int | None = None
        file_fd: int | None = None
        try:
            directory_fd = os.open(root.anchor, os.O_RDONLY | directory_flag | close_on_exec_flag)
            current_fd = directory_fd
            for part in (*root.parts[1:], *relative_path.parts[:-1]):
                next_fd = os.open(
                    part,
                    os.O_RDONLY | directory_flag | nofollow_flag | close_on_exec_flag,
                    dir_fd=current_fd,
                )
                if current_fd != directory_fd:
                    os.close(current_fd)
                current_fd = next_fd

            file_fd = os.open(
                relative_path.parts[-1],
                os.O_RDONLY | nofollow_flag | close_on_exec_flag | getattr(os, "O_NONBLOCK", 0),
                dir_fd=current_fd,
            )
            before = os.fstat(file_fd)
            if not stat.S_ISREG(before.st_mode):
                raise EvidenceUnavailable
            with os.fdopen(file_fd, "rb", closefd=False) as file_handle:
                data = file_handle.read()
            after = os.fstat(file_fd)
            if (
                before.st_size != after.st_size
                or before.st_mtime_ns != after.st_mtime_ns
                or len(data) != after.st_size
            ):
                raise EvidenceUnavailable
            return data, after
        except EvidenceUnavailable:
            raise
        except (OSError, RuntimeError, ValueError) as exc:
            raise EvidenceUnavailable from exc
        finally:
            if file_fd is not None:
                os.close(file_fd)
            if current_fd is not None and current_fd != directory_fd:
                os.close(current_fd)
            if directory_fd is not None:
                os.close(directory_fd)

    @staticmethod
    def _validate_image(data: bytes, declared_mime: str | None) -> str:
        try:
            with Image.open(BytesIO(data)) as image:
                detected_mime = _IMAGE_MIME_BY_FORMAT.get(image.format or "")
                image.verify()
            with Image.open(BytesIO(data)) as image:
                image.load()
        except UnidentifiedImageError as exc:
            raise EvidenceUnavailable from exc
        except (
            EOFError,
            Image.DecompressionBombError,
            OSError,
            struct.error,
            SyntaxError,
            ValueError,
        ) as exc:
            raise EvidenceUnavailable from exc
        if detected_mime is None:
            raise EvidenceNotImage
        if declared_mime is not None and detected_mime != declared_mime:
            raise EvidenceUnavailable
        return detected_mime
