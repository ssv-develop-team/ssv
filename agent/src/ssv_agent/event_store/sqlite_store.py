"""基于 SQLite 的事件与复核结果存储。"""

from __future__ import annotations

import json
import os
import sqlite3
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator

from ssv_agent.event_store.schema import EventQuery, migrate_schema


def _default_db_path() -> str:
    """返回默认 SQLite 路径：环境变量优先，其次 data/events.db。"""
    return os.getenv("SSV_EVENT_DB_PATH", "data/events.db")


class SsvEventStore:
    """事件、检测、证据与复核结果的 SQLite 读写。"""

    def __init__(self, db_path: str | os.PathLike[str] | None = None) -> None:
        self._path = Path(db_path or _default_db_path())
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._conn = sqlite3.connect(self._path, timeout=5.0)
        self._conn.row_factory = sqlite3.Row
        self._conn.execute("PRAGMA foreign_keys = ON")
        self._conn.execute("PRAGMA busy_timeout = 5000")
        self._conn.execute("PRAGMA journal_mode = WAL")
        migrate_schema(self._conn)
        self._conn.commit()

    def close(self) -> None:
        self._conn.close()

    def __enter__(self) -> "SsvEventStore":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    @contextmanager
    def transaction(self, *, immediate: bool = False) -> Iterator[sqlite3.Connection]:
        """提供账本使用的短事务；异常时回滚所有权威写入。"""
        self._conn.execute("BEGIN IMMEDIATE" if immediate else "BEGIN")
        try:
            yield self._conn
        except Exception:
            self._conn.rollback()
            raise
        else:
            self._conn.commit()

    def insert_event(
        self,
        event_id: str,
        *,
        source: str,
        timestamp_ms: int,
        frame_id: int,
        event_type: str | None = None,
        severity: str | None = None,
        status: str = "pending",
        verdict: str | None = None,
        confidence: float | None = None,
        result_path: str | None = None,
        created_ms: int | None = None,
    ) -> None:
        """写入事件；已存在则忽略（保留既有状态与结果）。"""
        self._conn.execute(
            """
            INSERT OR IGNORE INTO events (
                event_id, source, timestamp_ms, frame_id, event_type, severity,
                status, verdict, confidence, result_path, created_ms
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                event_id,
                source,
                timestamp_ms,
                frame_id,
                event_type,
                severity,
                status,
                verdict,
                confidence,
                result_path,
                created_ms if created_ms is not None else int(time.time() * 1000),
            ),
        )
        self._conn.commit()

    def insert_detections(self, event_id: str, detections: list[dict[str, Any]]) -> None:
        """写入事件下的检测列表；重复行忽略。"""
        rows = [
            (
                event_id,
                d["class_name"],
                int(d["class_id"]),
                float(d["confidence"]),
                json.dumps(d.get("bbox", []), ensure_ascii=False),
                int(d.get("track_id", -1)),
                d.get("track_state"),
                int(bool(d.get("occluded", False))),
            )
            for d in detections
        ]
        self._conn.executemany(
            """
            INSERT OR IGNORE INTO detections (
                event_id, class_name, class_id, confidence, bbox_json,
                track_id, track_state, occluded
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """,
            rows,
        )
        self._conn.commit()

    def insert_evidence(
        self,
        event_id: str,
        *,
        kind: str,
        path: str,
        available: bool = False,
        size: int | None = None,
        mtime: float | None = None,
    ) -> None:
        """写入证据条目（只记路径与元信息，不存文件）。"""
        self._conn.execute(
            """
            INSERT OR IGNORE INTO evidence (
                event_id, kind, path, available, size, mtime
            ) VALUES (?, ?, ?, ?, ?, ?)
            """,
            (event_id, kind, path, int(bool(available)), size, mtime),
        )
        self._conn.commit()

    def insert_result(
        self,
        event_id: str,
        *,
        verdict: str,
        confidence: float | None,
        evidence_status: str,
        explanation: str | None,
        parsed_at_ms: int | None = None,
        status: str = "completed",
        result_path: str | None = None,
    ) -> None:
        """写入复核结果，并同步事件状态。"""
        now_ms = parsed_at_ms if parsed_at_ms is not None else int(time.time() * 1000)
        self._conn.execute(
            """
            INSERT INTO review_results (
                event_id, verdict, confidence, evidence_status, explanation, parsed_at_ms
            ) VALUES (?, ?, ?, ?, ?, ?)
            ON CONFLICT(event_id) DO UPDATE SET
                verdict = excluded.verdict,
                confidence = excluded.confidence,
                evidence_status = excluded.evidence_status,
                explanation = excluded.explanation,
                parsed_at_ms = excluded.parsed_at_ms
            """,
            (event_id, verdict, confidence, evidence_status, explanation, now_ms),
        )
        self._conn.execute(
            "UPDATE events SET status = ?, verdict = ?, confidence = ?, result_path = ? "
            "WHERE event_id = ?",
            (status, verdict, confidence, result_path, event_id),
        )
        self._conn.commit()

    def get_event(self, event_id: str) -> dict[str, Any] | None:
        """返回事件详情（含检测与证据元信息），不存在返回 None。"""
        row = self._conn.execute(
            "SELECT * FROM events WHERE event_id = ?", (event_id,)
        ).fetchone()
        if row is None:
            return None
        detections = self._conn.execute(
            "SELECT * FROM detections WHERE event_id = ? ORDER BY id", (event_id,)
        ).fetchall()
        evidence = self._conn.execute(
            "SELECT * FROM evidence WHERE event_id = ? ORDER BY id", (event_id,)
        ).fetchall()
        return {
            "event": dict(row),
            "detections": [dict(r) for r in detections],
            "evidence": [dict(r) for r in evidence],
        }

    def get_result(self, event_id: str) -> dict[str, Any] | None:
        """返回复核结果，不存在返回 None。"""
        row = self._conn.execute(
            "SELECT * FROM review_results WHERE event_id = ?", (event_id,)
        ).fetchone()
        return dict(row) if row is not None else None

    def query_events(self, query: EventQuery) -> list[dict[str, Any]]:
        """按过滤条件查询事件，按时间倒序。"""
        sql = "SELECT e.* FROM events e WHERE 1=1"
        params: list[Any] = []
        if query.source is not None:
            sql += " AND e.source = ?"
            params.append(query.source)
        if query.start_ms is not None:
            sql += " AND e.timestamp_ms >= ?"
            params.append(query.start_ms)
        if query.end_ms is not None:
            sql += " AND e.timestamp_ms <= ?"
            params.append(query.end_ms)
        if query.event_type is not None:
            sql += " AND e.event_type = ?"
            params.append(query.event_type)
        if query.status is not None:
            sql += " AND e.status = ?"
            params.append(query.status)
        if query.verdict is not None:
            sql += " AND e.verdict = ?"
            params.append(query.verdict)
        if query.class_name is not None:
            sql += (
                " AND EXISTS ("
                "SELECT 1 FROM detections d "
                "WHERE d.event_id = e.event_id AND d.class_name = ?)"
            )
            params.append(query.class_name)
        sql += " ORDER BY e.timestamp_ms DESC LIMIT ? OFFSET ?"
        params.extend([query.limit, query.offset])
        rows = self._conn.execute(sql, params).fetchall()
        return [dict(r) for r in rows]
