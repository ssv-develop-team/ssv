"""事件证据链的事务边界。"""

from __future__ import annotations

import hashlib
import json
import mimetypes
import os
import stat
import time
import uuid
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path
from typing import TYPE_CHECKING, Any

from ssv_agent.config import RecordingEvidenceConfig
from ssv_agent.event_store.sqlite_store import SsvEventStore

if TYPE_CHECKING:
    from ssv_agent.event_episode import EpisodePolicy
    from ssv_agent.event_episode import EpisodeTransition
    from ssv_agent.evidence_provider import EvidenceArtifact
    from ssv_agent.report import ReportArtifact
from ssv_agent.review_context import Detection, ReviewContext


class JobKind(StrEnum):
    """账本可持久化的异步工作类型。"""

    REVIEW = "review"
    INDEX = "index"
    EVIDENCE_EXTRACT = "evidence_extract"
    REPORT = "report"


class JobState(StrEnum):
    """持久任务的可见状态。"""

    PENDING = "pending"
    PROCESSING = "processing"
    COMPLETED = "completed"
    DEAD = "dead"


class EpisodeState(StrEnum):
    """检测事件 episode 的生命周期状态。"""

    OPEN = "open"
    LOST_GRACE = "lost_grace"
    CLOSED = "closed"


class EpisodeCloseReason(StrEnum):
    """episode 被关闭时的可审计原因。"""

    DEAD = "dead"
    EXPLICIT_END = "explicit_end"
    LOST_TIMEOUT = "lost_timeout"
    SILENCE_TIMEOUT = "silence_timeout"
    MERGE_GAP = "merge_gap"
    GENERATION_CHANGE = "generation_change"
    MAX_DURATION = "max_duration"


class LeaseLostError(RuntimeError):
    """任务已被其他 worker 接管或本 worker 的 lease 已失效。"""


@dataclass(frozen=True)
class EvidenceRef:
    """只指向登记证据的稳定引用。"""

    evidence_id: str
    kind: str
    path: str
    mime_type: str | None
    available: bool
    size: int | None
    mtime: float | None
    sha256: str | None
    source_pts_start: int | None
    source_pts_end: int | None
    stream_generation: int | None


@dataclass(frozen=True)
class EventCase:
    """从账本重建出的权威案件快照。"""

    event_id: str
    ingress_id: str | None
    source: str
    timestamp_ms: int
    frame_id: int
    stream_generation: int | None
    source_pts: int | None
    event_type: str | None
    severity: str | None
    rule_id: str | None
    rule_version: str | None
    rule_facts: dict[str, Any]
    revision: int
    status: str
    verdict: str | None
    confidence: float | None
    detections: tuple[dict[str, Any], ...]
    evidence: tuple[EvidenceRef, ...]
    review: dict[str, Any] | None
    episode_id: str | None = None
    episode_state: EpisodeState | None = None
    episode_close_reason: EpisodeCloseReason | None = None
    episode_start_pts: int | None = None
    episode_last_seen_pts: int | None = None
    episode_end_pts: int | None = None
    evidence_window_start: int | None = None
    evidence_window_end: int | None = None
    event_phase: str | None = None

    def to_review_context(self) -> ReviewContext:
        """从权威案件重建模型可读的输入，不暴露未登记的新路径。"""
        detections: list[Detection] = []
        for detection in self.detections:
            copied = dict(detection)
            copied["bbox"] = _json_value(copied.pop("bbox_json", "[]"), [])
            detections.append(Detection.model_validate(copied))
        frame = next((item.path for item in self.evidence if item.kind == "frame"), None)
        clip = next((item.path for item in self.evidence if item.kind == "clip"), None)
        return ReviewContext(
            event_id=self.event_id,
            ingress_id=self.ingress_id,
            source=self.source,
            timestamp_ms=self.timestamp_ms,
            frame_id=self.frame_id,
            stream_generation=self.stream_generation,
            source_pts=self.source_pts,
            event_type=self.event_type,
            event_phase=self.event_phase,
            severity=self.severity,
            rule_id=self.rule_id,
            rule_version=self.rule_version,
            rule_facts=self.rule_facts,
            detections=detections,
            frame_path=frame,
            clip_path=clip,
            evidence_ids=[item.evidence_id for item in self.evidence if item.available],
        )


@dataclass(frozen=True)
class DurableJob:
    """一条可租约、可重试的持久任务。"""

    job_id: int
    kind: JobKind
    entity_id: str
    entity_revision: int
    state: JobState
    attempts: int
    lease_owner: str | None
    lease_expires_ms: int | None
    available_at_ms: int
    last_error: str | None


@dataclass(frozen=True)
class ReviewRecord:
    """append-only 复核历史中的一个精确 revision。"""

    review_id: str
    event_id: str
    revision: int
    verdict: str
    confidence: float
    evidence_status: str
    explanation: str
    evidence_ids: tuple[str, ...]
    claims: tuple[dict[str, Any], ...]
    rule_citations: tuple[dict[str, Any], ...]
    policy_id: str | None
    model_id: str | None
    result_path: str | None
    created_ms: int


@dataclass(frozen=True)
class ReportRecord:
    """已登记报告 artifact 的不可变元数据。"""

    report_id: str
    event_id: str
    review_id: str
    review_revision: int
    template_version: str
    sha256: str
    artifact_path: str
    size_bytes: int
    created_ms: int


@dataclass(frozen=True)
class EventEpisode:
    """从账本重建出的 episode 生命周期快照。"""

    episode_id: str
    canonical_event_id: str
    source: str
    event_type: str | None
    rule_id: str | None
    rule_version: str | None
    stream_generation: int
    start_pts: int
    last_seen_pts: int
    last_seen_timestamp_ms: int
    end_pts: int | None
    state: EpisodeState
    close_reason: EpisodeCloseReason | None
    evidence_window_start: int | None
    evidence_window_end: int | None
    event_ids: tuple[str, ...]


@dataclass(frozen=True)
class EpisodeObservation:
    """一次已接收的 Redis 观测，按 ingress 和 event ID 幂等。"""

    episode_id: str
    event_id: str
    ingress_id: str
    source_pts: int | None
    timestamp_ms: int
    event_phase: str | None
    track_states: tuple[str, ...]
    dedup_decision: str


@dataclass(frozen=True)
class RecordOutcome:
    """一次幂等 record 的结果。"""

    case: EventCase
    jobs: tuple[DurableJob, ...]
    created: bool


@dataclass(frozen=True)
class EpisodeIngestOutcome:
    """一次 episode 观测落账后的结果。"""

    episode: EventEpisode | None
    case: EventCase | None
    created: bool
    updated: bool
    closed: bool
    duplicate: bool


def _now_ms() -> int:
    return int(time.time() * 1000)


def _json_value(value: Any, fallback: Any) -> Any:
    if not value:
        return fallback
    try:
        return json.loads(value)
    except (TypeError, json.JSONDecodeError):
        return fallback


def _transition_close_reason(transition: "EpisodeTransition") -> EpisodeCloseReason:
    if transition.close_reason is None:
        raise ValueError(f"episode action {transition.action.value} requires a close reason")
    return transition.close_reason


def _evidence_id(event_id: str, kind: str, path: str) -> str:
    value = f"{event_id}\0{kind}\0{path}".encode()
    return hashlib.sha256(value).hexdigest()


def _normalise_evidence_roots(value: object, *, source: str) -> tuple[Path, ...]:
    if not isinstance(value, list):
        raise ValueError(f"{source} must be a JSON string array" if source.startswith("SSV_") else f"{source} must be a list")

    roots: list[Path] = []
    for item in value:
        if not isinstance(item, str) or not Path(item).is_absolute():
            raise ValueError(f"{source} entries must be absolute paths")
        try:
            resolved = Path(item).resolve(strict=False)
        except (OSError, RuntimeError) as exc:
            raise ValueError(f"{source} contains an unresolvable path") from exc
        if resolved not in roots:
            roots.append(resolved)
    return tuple(roots)


def _load_evidence_roots(evidence_roots: list[str] | None) -> tuple[Path, ...]:
    if evidence_roots is not None:
        return _normalise_evidence_roots(evidence_roots, source="evidence_roots")

    raw = os.environ.get("SSV_EVIDENCE_ROOTS")
    if raw is None:
        return ()
    try:
        value = json.loads(raw)
    except (TypeError, json.JSONDecodeError) as exc:
        raise ValueError("SSV_EVIDENCE_ROOTS must be a JSON string array") from exc
    return _normalise_evidence_roots(value, source="SSV_EVIDENCE_ROOTS")


class EventLedger:
    """封装案件、证据和首批持久任务的权威 SQLite 事务。"""

    def __init__(
        self,
        db_path: str | os.PathLike[str] | None = None,
        evidence_roots: list[str] | None = None,
        recording_evidence: RecordingEvidenceConfig | None = None,
    ) -> None:
        self._evidence_roots = _load_evidence_roots(evidence_roots)
        self._recording_evidence = recording_evidence
        self._store = SsvEventStore(db_path)

    @property
    def evidence_roots(self) -> tuple[Path, ...]:
        """返回已规范化的可信证据根目录。"""
        return self._evidence_roots

    def resolve_evidence_path(self, path: str | os.PathLike[str]) -> Path | None:
        """解析并验证证据路径；越界、相对或无法解析的路径返回 None。"""
        try:
            candidate = Path(path)
        except (TypeError, ValueError):
            return None
        if not candidate.is_absolute():
            return None
        try:
            resolved = candidate.resolve(strict=False)
        except (OSError, RuntimeError):
            return None
        if any(resolved == root or root in resolved.parents for root in self._evidence_roots):
            return resolved
        return None

    def close(self) -> None:
        self._store.close()

    def __enter__(self) -> "EventLedger":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def record(self, context: ReviewContext) -> RecordOutcome:
        """原子登记一件案件及其首批 review/index 工作。"""
        now_ms = _now_ms()
        event_id = context.event_id
        with self._store.transaction(immediate=True) as connection:
            created = self._record_event_in_transaction(
                connection,
                context,
                now_ms=now_ms,
                create_jobs=True,
            )

        case = self.get_case(event_id)
        if case is None:  # pragma: no cover - guarded by the transaction above
            raise RuntimeError(f"event ledger lost recorded case: {event_id}")
        return RecordOutcome(
            case=case,
            jobs=self.jobs_for_event(event_id),
            created=created,
        )

    def _record_event_in_transaction(
        self,
        connection: Any,
        context: ReviewContext,
        *,
        now_ms: int,
        create_jobs: bool,
        episode_id: str | None = None,
    ) -> bool:
        """在当前事务内幂等登记 canonical event。"""
        event_id = context.event_id
        existing = connection.execute(
            "SELECT 1 FROM events WHERE event_id = ?", (event_id,)
        ).fetchone()
        if existing is not None:
            if episode_id is not None:
                connection.execute(
                    "UPDATE events SET episode_id = COALESCE(episode_id, ?) WHERE event_id = ?",
                    (episode_id, event_id),
                )
            return False

        connection.execute(
            """
            INSERT INTO events (
                event_id, ingress_id, source, timestamp_ms, frame_id,
                stream_generation, source_pts, event_type, event_phase, severity,
                rule_id, rule_version, rule_facts_json, episode_id,
                revision, status, created_ms
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 'pending', ?)
            """,
            (
                event_id,
                getattr(context, "ingress_id", event_id),
                context.source,
                context.timestamp_ms,
                context.frame_id,
                getattr(context, "stream_generation", None),
                getattr(context, "source_pts", None),
                context.event_type,
                getattr(context, "event_phase", None),
                context.severity,
                getattr(context, "rule_id", None),
                getattr(context, "rule_version", None),
                json.dumps(getattr(context, "rule_facts", {}), ensure_ascii=False),
                episode_id,
                now_ms,
            ),
        )
        self._record_detections(connection, event_id, context)
        self._record_evidence(connection, event_id, context)
        if create_jobs:
            jobs_to_create: tuple[tuple[JobKind, int], ...]
            if self._recording_evidence is not None:
                jobs_to_create = (
                    (
                        JobKind.EVIDENCE_EXTRACT,
                        context.timestamp_ms + self._recording_evidence.clip_after_ms,
                    ),
                )
            else:
                jobs_to_create = ((JobKind.REVIEW, now_ms), (JobKind.INDEX, now_ms))
            self._create_jobs_in_transaction(
                connection,
                event_id,
                jobs_to_create,
                now_ms,
            )
        return True

    @staticmethod
    def _create_jobs_in_transaction(
        connection: Any,
        entity_id: str,
        jobs: tuple[tuple[JobKind, int], ...],
        now_ms: int,
    ) -> None:
        for job_kind, available_at_ms in jobs:
            connection.execute(
                """
                INSERT INTO durable_jobs (
                    kind, entity_id, entity_revision, state, attempts,
                    available_at_ms, created_ms, updated_ms
                ) VALUES (?, ?, 0, 'pending', 0, ?, ?, ?)
                ON CONFLICT(kind, entity_id, entity_revision) DO NOTHING
                """,
                (job_kind.value, entity_id, available_at_ms, now_ms, now_ms),
            )

    def get_case(self, event_id: str) -> EventCase | None:
        """返回可供 worker 和只读工具消费的案件快照。"""
        stored = self._store.get_event(event_id)
        if stored is None:
            return None
        event = stored["event"]
        result = self._store.get_result(event_id)
        if result is not None:
            result["evidence_ids"] = _json_value(result.pop("evidence_ids_json", "[]"), [])
            result["claims"] = _json_value(result.pop("claims_json", "[]"), [])
        evidence = tuple(
            EvidenceRef(
                evidence_id=row["evidence_id"] or f"legacy-{row['id']}",
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
            )
            for row in stored["evidence"]
        )
        episode = self._episode_for_event(event)
        return EventCase(
            event_id=event["event_id"],
            ingress_id=event["ingress_id"],
            source=event["source"],
            timestamp_ms=event["timestamp_ms"],
            frame_id=event["frame_id"],
            stream_generation=event["stream_generation"],
            source_pts=event["source_pts"],
            event_type=event["event_type"],
            severity=event["severity"],
            rule_id=event["rule_id"],
            rule_version=event["rule_version"],
            rule_facts=_json_value(event["rule_facts_json"], {}),
            revision=event["revision"],
            status=event["status"],
            verdict=event["verdict"],
            confidence=event["confidence"],
            detections=tuple(stored["detections"]),
            evidence=evidence,
            review=result,
            episode_id=event.get("episode_id"),
            episode_state=episode.state if episode else None,
            episode_close_reason=episode.close_reason if episode else None,
            episode_start_pts=episode.start_pts if episode else None,
            episode_last_seen_pts=episode.last_seen_pts if episode else None,
            episode_end_pts=episode.end_pts if episode else None,
            evidence_window_start=(episode.evidence_window_start if episode else None),
            evidence_window_end=(episode.evidence_window_end if episode else None),
            event_phase=event.get("event_phase"),
        )

    def get_review_record(self, event_id: str, revision: int) -> ReviewRecord | None:
        """从 append-only 历史读取指定 revision，而非当前 review 投影。"""
        row = self._store.get_review_record(event_id, revision)
        if row is None:
            return None
        return ReviewRecord(
            review_id=row["review_id"],
            event_id=row["event_id"],
            revision=row["revision"],
            verdict=row["verdict"],
            confidence=row["confidence"],
            evidence_status=row["evidence_status"],
            explanation=row["explanation"],
            evidence_ids=tuple(_json_value(row["evidence_ids_json"], [])),
            claims=tuple(_json_value(row["claims_json"], [])),
            rule_citations=tuple(_json_value(row["rule_citations_json"], [])),
            policy_id=row["policy_id"],
            model_id=row["model_id"],
            result_path=row["result_path"],
            created_ms=row["created_ms"],
        )

    def get_report_for_review(
        self,
        event_id: str,
        review_id: str,
    ) -> ReportRecord | None:
        """返回指定复核版本已登记的报告 artifact 元数据。"""
        row = self._store.get_report_for_review(event_id, review_id)
        return self._report_record_from_row(row) if row is not None else None

    def report_records_for_event(self, event_id: str) -> tuple[ReportRecord, ...]:
        """按复核 revision 返回事件的报告元数据。"""
        return tuple(
            self._report_record_from_row(row)
            for row in self._store.get_reports_for_event(event_id)
        )

    def get_episode(self, episode_id: str) -> EventEpisode | None:
        """返回一个 episode 的权威生命周期快照。"""
        row = self._store.get_episode(episode_id)
        return self._episode_from_row(row) if row is not None else None

    def ingest_episode(
        self,
        context: ReviewContext,
        *,
        policy: EpisodePolicy,
        dedup_decision: object,
        now_ms: int | None = None,
    ) -> EpisodeIngestOutcome:
        """原子登记 episode 观测，并在关闭时创建唯一取证任务。

        ``policy`` 只提供无副作用的状态决策；SQLite 事务、幂等和任务创建仍由
        ``EventLedger`` 负责。这样 Redis 重投和多个 Consumer 进程都经过同一
        持久化 seam。
        """
        from ssv_agent.event_episode import EpisodeAction

        observed_at_ms = _now_ms() if now_ms is None else now_ms
        event_id = context.event_id
        ingress_id = context.ingress_id or event_id
        decision = getattr(dedup_decision, "value", str(dedup_decision))
        episode_id: str | None = None
        canonical_event_id: str | None = None
        created = False
        updated = False
        closed = False
        duplicate = False

        with self._store.transaction(immediate=True) as connection:
            existing_observation = connection.execute(
                """
                SELECT episode_id FROM episode_observations
                WHERE ingress_id = ? OR event_id = ?
                LIMIT 1
                """,
                (ingress_id, event_id),
            ).fetchone()
            if existing_observation is not None:
                duplicate = True
                episode_id = existing_observation["episode_id"]
                episode_row = connection.execute(
                    "SELECT canonical_event_id FROM episodes WHERE episode_id = ?",
                    (episode_id,),
                ).fetchone()
                canonical_event_id = (
                    episode_row["canonical_event_id"] if episode_row is not None else event_id
                )
            elif context.source_pts is None or context.stream_generation is None:
                # 没有精确 anchor 的消息仍保留为事实，但不生成会误导取证的 job。
                created = self._record_event_in_transaction(
                    connection,
                    context,
                    now_ms=observed_at_ms,
                    create_jobs=False,
                )
                canonical_event_id = event_id
            else:
                for row in self._active_episode_rows_for_key(
                    connection,
                    context,
                    include_generation=False,
                ):
                    if row["stream_generation"] != context.stream_generation:
                        old_episode = self._episode_from_row(row)
                        self._close_episode_in_transaction(
                            connection,
                            old_episode,
                            policy,
                            policy.generation_change_reason,
                            now_ms=observed_at_ms,
                        )
                        closed = True

                current_row = self._active_episode_row(
                    connection,
                    context,
                    context.stream_generation,
                )
                current = self._episode_from_row(current_row) if current_row is not None else None
                if current is not None:
                    expiration_reason = policy.expiration_reason(
                        current,
                        now_ms=observed_at_ms,
                    )
                    if expiration_reason is not None:
                        self._close_episode_in_transaction(
                            connection,
                            current,
                            policy,
                            expiration_reason,
                            now_ms=observed_at_ms,
                        )
                        closed = True
                        current = None
                transition = policy.transition(
                    context,
                    current,
                    now_ms=observed_at_ms,
                )

                if current is not None and transition.action is EpisodeAction.ROLLOVER:
                    self._close_episode_in_transaction(
                        connection,
                        current,
                        policy,
                        _transition_close_reason(transition),
                        now_ms=observed_at_ms,
                    )
                    closed = True
                    current = None

                if current is None:
                    episode_id = event_id
                    canonical_event_id = event_id
                    created = self._record_event_in_transaction(
                        connection,
                        context,
                        now_ms=observed_at_ms,
                        create_jobs=False,
                        episode_id=episode_id,
                    )
                    self._insert_episode_in_transaction(
                        connection,
                        episode_id,
                        context,
                        transition.state,
                        observed_at_ms,
                    )
                    self._insert_episode_observation(
                        connection,
                        episode_id,
                        context,
                        decision,
                        observed_at_ms,
                    )
                    updated = True
                    if transition.close_after_open:
                        self._close_episode_in_transaction(
                            connection,
                            self._episode_from_row(
                                connection.execute(
                                    "SELECT * FROM episodes WHERE episode_id = ?",
                                    (episode_id,),
                                ).fetchone()
                            ),
                            policy,
                            _transition_close_reason(transition),
                            now_ms=observed_at_ms,
                        )
                        closed = True
                else:
                    episode_id = current.episode_id
                    canonical_event_id = current.canonical_event_id
                    self._insert_episode_observation(
                        connection,
                        episode_id,
                        context,
                        decision,
                        observed_at_ms,
                    )
                    self._update_episode_in_transaction(
                        connection,
                        current,
                        context,
                        (
                            current.state
                            if transition.action is EpisodeAction.CLOSE
                            else transition.state
                        ),
                        observed_at_ms,
                    )
                    updated = True
                    if transition.action is EpisodeAction.CLOSE:
                        refreshed = self._episode_from_row(
                            connection.execute(
                                "SELECT * FROM episodes WHERE episode_id = ?",
                                (episode_id,),
                            ).fetchone()
                        )
                        self._close_episode_in_transaction(
                            connection,
                            refreshed,
                            policy,
                            _transition_close_reason(transition),
                            end_pts=context.source_pts,
                            now_ms=observed_at_ms,
                        )
                        closed = True

        case = self.get_case(canonical_event_id) if canonical_event_id is not None else None
        episode = self.get_episode(episode_id) if episode_id is not None else None
        return EpisodeIngestOutcome(
            episode=episode,
            case=case,
            created=created,
            updated=updated,
            closed=closed,
            duplicate=duplicate,
        )

    def close_expired_episodes(
        self,
        *,
        policy: EpisodePolicy,
        now_ms: int | None = None,
    ) -> tuple[EventEpisode, ...]:
        """关闭已超过静默、丢失宽限或最大时长的 episode。"""
        observed_at_ms = _now_ms() if now_ms is None else now_ms
        closed_ids: list[str] = []
        with self._store.transaction(immediate=True) as connection:
            rows = connection.execute(
                "SELECT * FROM episodes WHERE state IN ('open', 'lost_grace')"
            ).fetchall()
            for row in rows:
                episode = self._episode_from_row(row)
                reason = policy.expiration_reason(episode, now_ms=observed_at_ms)
                if reason is None:
                    continue
                self._close_episode_in_transaction(
                    connection,
                    episode,
                    policy,
                    reason,
                    now_ms=observed_at_ms,
                )
                closed_ids.append(episode.episode_id)
        closed_episodes: list[EventEpisode] = []
        for episode_id in closed_ids:
            closed_episode = self.get_episode(episode_id)
            if closed_episode is not None:
                closed_episodes.append(closed_episode)
        return tuple(closed_episodes)

    def _active_episode_rows_for_key(
        self,
        connection: Any,
        context: ReviewContext,
        *,
        include_generation: bool,
    ) -> list[Any]:
        query = """
            SELECT * FROM episodes
            WHERE source = ? AND event_type IS ? AND rule_id IS ?
                AND rule_version IS ? AND state IN ('open', 'lost_grace')
        """
        params: list[Any] = [
            context.source,
            context.event_type,
            getattr(context, "rule_id", None),
            getattr(context, "rule_version", None),
        ]
        if include_generation:
            query += " AND stream_generation = ?"
            params.append(context.stream_generation)
        query += " ORDER BY last_seen_pts DESC, episode_id"
        return connection.execute(query, params).fetchall()

    def _active_episode_row(
        self,
        connection: Any,
        context: ReviewContext,
        generation: int,
    ) -> Any | None:
        rows = self._active_episode_rows_for_key(
            connection,
            context,
            include_generation=True,
        )
        for row in rows:
            if row["stream_generation"] == generation:
                return row
        return None

    @staticmethod
    def _insert_episode_in_transaction(
        connection: Any,
        episode_id: str,
        context: ReviewContext,
        state: EpisodeState,
        now_ms: int,
    ) -> None:
        source_pts = context.source_pts
        generation = context.stream_generation
        if source_pts is None or generation is None:
            raise ValueError("episode requires exact source PTS and stream generation")
        if source_pts < 0 or generation < 0:
            raise ValueError("episode source timing anchor is invalid")
        connection.execute(
            """
            INSERT INTO episodes (
                episode_id, canonical_event_id, source, event_type, rule_id, rule_version,
                stream_generation, start_pts, last_seen_pts, last_seen_timestamp_ms,
                state, event_ids_json, created_ms, updated_ms
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                episode_id,
                context.event_id,
                context.source,
                context.event_type,
                getattr(context, "rule_id", None),
                getattr(context, "rule_version", None),
                generation,
                source_pts,
                source_pts,
                context.timestamp_ms,
                state.value,
                json.dumps([context.event_id], ensure_ascii=False),
                now_ms,
                now_ms,
            ),
        )

    @staticmethod
    def _insert_episode_observation(
        connection: Any,
        episode_id: str,
        context: ReviewContext,
        dedup_decision: str,
        now_ms: int,
    ) -> None:
        track_states = [
            str(detection.track_state)
            for detection in context.detections
            if detection.track_state is not None
        ]
        connection.execute(
            """
            INSERT INTO episode_observations (
                episode_id, event_id, ingress_id, source_pts, timestamp_ms,
                event_phase, track_states_json, dedup_decision, created_ms
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                episode_id,
                context.event_id,
                context.ingress_id or context.event_id,
                context.source_pts,
                context.timestamp_ms,
                getattr(context, "event_phase", None),
                json.dumps(track_states, ensure_ascii=False),
                dedup_decision,
                now_ms,
            ),
        )

    @staticmethod
    def _update_episode_in_transaction(
        connection: Any,
        episode: EventEpisode,
        context: ReviewContext,
        state: EpisodeState,
        now_ms: int,
    ) -> None:
        event_ids = list(episode.event_ids)
        if context.event_id not in event_ids:
            event_ids.append(context.event_id)
        last_seen_pts = max(episode.last_seen_pts, context.source_pts or episode.last_seen_pts)
        last_seen_timestamp_ms = max(episode.last_seen_timestamp_ms, context.timestamp_ms)
        connection.execute(
            """
            UPDATE episodes
            SET last_seen_pts = ?, last_seen_timestamp_ms = ?, state = ?,
                event_ids_json = ?, updated_ms = ?
            WHERE episode_id = ? AND state IN ('open', 'lost_grace')
            """,
            (
                last_seen_pts,
                last_seen_timestamp_ms,
                state.value,
                json.dumps(event_ids, ensure_ascii=False),
                now_ms,
                episode.episode_id,
            ),
        )

    def _close_episode_in_transaction(
        self,
        connection: Any,
        episode: EventEpisode,
        policy: EpisodePolicy,
        reason: EpisodeCloseReason,
        *,
        end_pts: int | None = None,
        now_ms: int,
    ) -> None:
        if episode.state is EpisodeState.CLOSED:
            return
        final_end_pts = max(episode.last_seen_pts, end_pts or episode.last_seen_pts)
        window_start, window_end = policy.evidence_window(episode, final_end_pts)
        available_at_ms = max(
            now_ms,
            episode.last_seen_timestamp_ms + policy.post_roll_ms,
        )
        connection.execute(
            """
            UPDATE episodes
            SET end_pts = ?, state = 'closed', close_reason = ?,
                evidence_window_start = ?, evidence_window_end = ?, updated_ms = ?
            WHERE episode_id = ? AND state IN ('open', 'lost_grace')
            """,
            (
                final_end_pts,
                reason.value,
                window_start,
                window_end,
                now_ms,
                episode.episode_id,
            ),
        )
        self._create_jobs_in_transaction(
            connection,
            episode.canonical_event_id,
            ((JobKind.EVIDENCE_EXTRACT, available_at_ms),),
            now_ms,
        )

    @staticmethod
    def _episode_from_row(row: Any) -> EventEpisode:
        raw_reason = row["close_reason"]
        reason = EpisodeCloseReason(raw_reason) if raw_reason else None
        event_ids = _json_value(row["event_ids_json"], [])
        if not isinstance(event_ids, list):
            event_ids = []
        return EventEpisode(
            episode_id=row["episode_id"],
            canonical_event_id=row["canonical_event_id"],
            source=row["source"],
            event_type=row["event_type"],
            rule_id=row["rule_id"],
            rule_version=row["rule_version"],
            stream_generation=row["stream_generation"],
            start_pts=row["start_pts"],
            last_seen_pts=row["last_seen_pts"],
            last_seen_timestamp_ms=row["last_seen_timestamp_ms"],
            end_pts=row["end_pts"],
            state=EpisodeState(row["state"]),
            close_reason=reason,
            evidence_window_start=row["evidence_window_start"],
            evidence_window_end=row["evidence_window_end"],
            event_ids=tuple(str(item) for item in event_ids),
        )

    def _episode_for_event(self, event: dict[str, Any]) -> EventEpisode | None:
        row = self._store.get_episode_for_event(
            event["event_id"], episode_id=event.get("episode_id")
        )
        return self._episode_from_row(row) if row is not None else None

    def append_review(self, event_id: str, result: Any, result_path: str) -> int:
        """追加模型复核、更新兼容投影，并为新 revision 建立索引工作。"""
        now_ms = _now_ms()
        with self._store.transaction(immediate=True) as connection:
            event = connection.execute(
                "SELECT revision FROM events WHERE event_id = ?", (event_id,)
            ).fetchone()
            if event is None:
                raise KeyError(f"unknown event case: {event_id}")
            return self._append_review_in_transaction(
                connection,
                event_id,
                event,
                result,
                result_path,
                now_ms,
            )

    def refresh_evidence(self, event_id: str, evidence_id: str | None = None) -> int:
        """刷新已登记证据并返回匹配数；未知 event/evidence 返回 0。"""
        with self._store.transaction(immediate=True) as connection:
            return self._refresh_evidence_in_transaction(
                connection,
                event_id,
                evidence_id,
            )

    def complete_review_job(
        self,
        job: DurableJob,
        worker_id: str,
        result: Any,
        result_path: str,
    ) -> int:
        """以持有者 fence 原子提交 review、投影、派生任务和 job 完成状态。"""
        if job.kind != JobKind.REVIEW:
            raise ValueError("complete_review_job requires a review job")

        with self._store.transaction(immediate=True) as connection:
            now_ms = _now_ms()
            current_job = connection.execute(
                "SELECT * FROM durable_jobs WHERE job_id = ?", (job.job_id,)
            ).fetchone()
            self._validate_review_lease(current_job, job, worker_id, now_ms)

            event = connection.execute(
                "SELECT revision FROM events WHERE event_id = ?", (job.entity_id,)
            ).fetchone()
            if event is None:
                raise KeyError(f"unknown event case: {job.entity_id}")
            if event["revision"] != job.entity_revision:
                raise ValueError(
                    "review job revision no longer matches the current event revision"
                )
            self._refresh_evidence_in_transaction(connection, job.entity_id)

            revision = self._append_review_in_transaction(
                connection,
                job.entity_id,
                event,
                result,
                result_path,
                now_ms,
            )
            self._complete_owned_job(connection, job, worker_id)
        return revision

    def complete_report_job(
        self,
        job: DurableJob,
        worker_id: str,
        artifact: ReportArtifact,
        template_version: str,
    ) -> ReportRecord:
        """原子登记报告元数据并完成当前 lease 持有的 report job。"""
        if job.kind != JobKind.REPORT:
            raise ValueError("complete_report_job requires a report job")
        if not template_version:
            raise ValueError("template_version must not be empty")

        artifact_path = str(artifact.path)
        sha256 = artifact.sha256
        size_bytes = artifact.size_bytes
        if (
            not isinstance(sha256, str)
            or len(sha256) != 64
            or any(character not in "0123456789abcdef" for character in sha256)
            or not isinstance(size_bytes, int)
            or isinstance(size_bytes, bool)
            or size_bytes < 0
            or not artifact_path
        ):
            raise ValueError("report artifact metadata is invalid")

        with self._store.transaction(immediate=True) as connection:
            now_ms = _now_ms()
            current_job = connection.execute(
                "SELECT * FROM durable_jobs WHERE job_id = ?", (job.job_id,)
            ).fetchone()
            if current_job is None or (
                current_job["kind"] != JobKind.REPORT.value
                or current_job["entity_id"] != job.entity_id
                or current_job["entity_revision"] != job.entity_revision
            ):
                raise LeaseLostError(f"lost lease for durable job {job.job_id}")

            review = connection.execute(
                "SELECT review_id FROM reviews WHERE event_id = ? AND revision = ?",
                (job.entity_id, job.entity_revision),
            ).fetchone()
            if review is None:
                raise ValueError("report job has no matching committed review")

            if current_job["state"] == JobState.COMPLETED.value:
                if job.lease_owner != worker_id or current_job["attempts"] != job.attempts:
                    raise LeaseLostError(f"lost lease for durable job {job.job_id}")
                previous = connection.execute(
                    "SELECT * FROM reports WHERE event_id = ? AND review_id = ?",
                    (job.entity_id, review["review_id"]),
                ).fetchone()
                if previous is None:
                    raise LeaseLostError(f"completed report job {job.job_id} has no report record")
                previous_record = self._report_record_from_row(previous)
                if (
                    previous_record.template_version != template_version
                    or previous_record.sha256 != sha256
                    or previous_record.artifact_path != artifact_path
                    or previous_record.size_bytes != size_bytes
                ):
                    raise ValueError("report artifact conflicts with the completed report")
                return previous_record

            self._validate_report_lease(current_job, job, worker_id, now_ms)

            report = ReportRecord(
                report_id=uuid.uuid4().hex,
                event_id=job.entity_id,
                review_id=review["review_id"],
                review_revision=job.entity_revision,
                template_version=template_version,
                sha256=sha256,
                artifact_path=artifact_path,
                size_bytes=size_bytes,
                created_ms=now_ms,
            )
            connection.execute(
                """
                INSERT INTO reports (
                    report_id, event_id, review_id, review_revision,
                    template_version, sha256, artifact_path, size_bytes, created_ms
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    report.report_id,
                    report.event_id,
                    report.review_id,
                    report.review_revision,
                    report.template_version,
                    report.sha256,
                    report.artifact_path,
                    report.size_bytes,
                    report.created_ms,
                ),
            )
            self._complete_owned_job_kind(connection, job, worker_id, JobKind.REPORT)
        return report

    def complete_evidence_extract_job(
        self,
        job: DurableJob,
        worker_id: str,
        artifacts: tuple["EvidenceArtifact", ...],
    ) -> None:
        """原子登记完整取证集、创建 review，并完成 extract job。"""
        if job.kind != JobKind.EVIDENCE_EXTRACT:
            raise ValueError("complete_evidence_extract_job requires an evidence extract job")
        with self._store.transaction(immediate=True) as connection:
            now_ms = _now_ms()
            current = connection.execute(
                "SELECT * FROM durable_jobs WHERE job_id = ?", (job.job_id,)
            ).fetchone()
            if current is not None and current["state"] == JobState.COMPLETED.value:
                return
            self._validate_extract_lease(current, job, worker_id, now_ms)
            event = connection.execute(
                "SELECT revision FROM events WHERE event_id = ?", (job.entity_id,)
            ).fetchone()
            if event is None:
                raise KeyError(f"unknown event case: {job.entity_id}")
            if event["revision"] != job.entity_revision:
                raise ValueError("evidence extract job revision no longer matches the current event revision")
            kinds = [artifact.kind for artifact in artifacts]
            if len(artifacts) != 4 or kinds.count("clip") != 1 or kinds.count("frame") != 3:
                raise ValueError("evidence extract requires exactly one clip and three frames")
            for artifact in artifacts:
                self._validate_artifact_timing(artifact)
                resolved = self.resolve_evidence_path(artifact.path)
                if resolved is None or not self._artifact_metadata_matches(resolved, artifact):
                    raise ValueError("evidence artifact path or metadata is invalid")
                canonical = str(resolved)
                evidence_id = _evidence_id(job.entity_id, artifact.kind, canonical)
                connection.execute(
                    """
                    INSERT INTO evidence (
                        event_id, evidence_id, kind, path, mime_type, available,
                        size, mtime, sha256, source_pts_start, source_pts_end, stream_generation
                    ) VALUES (?, ?, ?, ?, ?, 1, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT(event_id, kind, path) DO UPDATE SET
                        evidence_id = excluded.evidence_id, mime_type = excluded.mime_type,
                        available = 1, size = excluded.size, mtime = excluded.mtime,
                        sha256 = excluded.sha256,
                        source_pts_start = excluded.source_pts_start,
                        source_pts_end = excluded.source_pts_end,
                        stream_generation = excluded.stream_generation
                    """,
                    (job.entity_id, evidence_id, artifact.kind, canonical,
                     artifact.mime_type, artifact.size, artifact.mtime, artifact.sha256,
                     artifact.source_pts_start, artifact.source_pts_end,
                     artifact.stream_generation),
                )
            connection.execute(
                """
                INSERT INTO durable_jobs (
                    kind, entity_id, entity_revision, state, attempts,
                    available_at_ms, created_ms, updated_ms
                ) VALUES ('review', ?, ?, 'pending', 0, ?, ?, ?)
                ON CONFLICT(kind, entity_id, entity_revision) DO NOTHING
                """,
                (job.entity_id, job.entity_revision, now_ms, now_ms, now_ms),
            )
            self._complete_owned_job_kind(connection, job, worker_id, JobKind.EVIDENCE_EXTRACT)

    def fail_evidence_extract_job(
        self,
        job: DurableJob,
        worker_id: str,
        error: str,
        max_retries: int,
        retry_delay_ms: int,
    ) -> JobState:
        """记录取证失败；达到上限时与事件转人工复核同事务提交。"""
        if job.kind != JobKind.EVIDENCE_EXTRACT:
            raise ValueError("fail_evidence_extract_job requires an evidence extract job")
        with self._store.transaction(immediate=True) as connection:
            state = self._fail_job_in_transaction(
                connection, job.job_id, worker_id, job.attempts,
                error, max_retries, retry_delay_ms,
            )
            if state == JobState.DEAD:
                connection.execute(
                    "UPDATE events SET status = 'manual_review' WHERE event_id = ?",
                    (job.entity_id,),
                )
            return state

    def enqueue_all_index_jobs(self) -> int:
        """为当前案件投影重新入队 index job，供可重建的全量索引使用。"""
        now_ms = _now_ms()
        requeued = 0
        with self._store.transaction(immediate=True) as connection:
            rows = connection.execute("SELECT event_id, revision FROM events").fetchall()
            for row in rows:
                cursor = connection.execute(
                    """
                    INSERT INTO durable_jobs (
                        kind, entity_id, entity_revision, state, attempts,
                        available_at_ms, created_ms, updated_ms
                    ) VALUES ('index', ?, ?, 'pending', 0, ?, ?, ?)
                    ON CONFLICT(kind, entity_id, entity_revision) DO UPDATE SET
                        state = 'pending', lease_owner = NULL,
                        lease_expires_ms = NULL, available_at_ms = excluded.available_at_ms,
                        last_error = NULL, updated_ms = excluded.updated_ms
                    WHERE durable_jobs.state IN ('completed', 'dead')
                    """,
                    (row["event_id"], row["revision"], now_ms, now_ms, now_ms),
                )
                requeued += cursor.rowcount
        return requeued

    def renew_job(
        self,
        job_id: int,
        worker_id: str,
        attempts: int,
        lease_ms: int,
    ) -> None:
        """仅允许当前未过期 attempt 延长自己的 lease。"""
        with self._store.transaction(immediate=True) as connection:
            now_ms = _now_ms()
            cursor = connection.execute(
                """
                UPDATE durable_jobs
                SET lease_expires_ms = ?, updated_ms = ?
                WHERE job_id = ? AND state = 'processing' AND lease_owner = ?
                    AND attempts = ? AND lease_expires_ms > ?
                """,
                (
                    now_ms + max(lease_ms, 0),
                    now_ms,
                    job_id,
                    worker_id,
                    attempts,
                    now_ms,
                ),
            )
            if cursor.rowcount != 1:
                raise LeaseLostError(f"lost lease for durable job {job_id}")

    def claim_job(
        self,
        kind: JobKind,
        worker_id: str,
        lease_ms: int,
    ) -> DurableJob | None:
        """以短租约独占领取一条待处理或已过期的工作。"""
        with self._store.transaction(immediate=True) as connection:
            now_ms = _now_ms()
            row = connection.execute(
                """
                SELECT * FROM durable_jobs
                WHERE kind = ? AND (
                    (state = 'pending' AND available_at_ms <= ?)
                    OR (state = 'processing' AND lease_expires_ms <= ?)
                )
                ORDER BY job_id
                LIMIT 1
                """,
                (kind.value, now_ms, now_ms),
            ).fetchone()
            if row is None:
                return None
            connection.execute(
                """
                UPDATE durable_jobs
                SET state = 'processing', attempts = attempts + 1,
                    lease_owner = ?, lease_expires_ms = ?, updated_ms = ?
                WHERE job_id = ?
                """,
                (worker_id, now_ms + max(lease_ms, 0), now_ms, row["job_id"]),
            )
            claimed = connection.execute(
                "SELECT * FROM durable_jobs WHERE job_id = ?", (row["job_id"],)
            ).fetchone()
        return self._job_from_row(claimed)

    def complete_job(self, job_id: int, worker_id: str, attempts: int) -> None:
        """仅允许持有未过期 lease 的 worker 标记工作完成。"""
        with self._store.transaction(immediate=True) as connection:
            now_ms = _now_ms()
            cursor = connection.execute(
                """
                UPDATE durable_jobs
                SET state = 'completed', lease_owner = NULL, lease_expires_ms = NULL,
                    updated_ms = ?
                WHERE job_id = ? AND state = 'processing' AND lease_owner = ?
                    AND attempts = ? AND lease_expires_ms > ?
                """,
                (now_ms, job_id, worker_id, attempts, now_ms),
            )
            if cursor.rowcount != 1:
                raise LeaseLostError(f"lost lease for durable job {job_id}")

    def fail_job(
        self,
        job_id: int,
        worker_id: str,
        attempts: int,
        error: str,
        max_retries: int,
        retry_delay_ms: int,
    ) -> JobState:
        """仅允许持有未过期 lease 的 worker 记录失败或重试。"""
        with self._store.transaction(immediate=True) as connection:
            return self._fail_job_in_transaction(
                connection, job_id, worker_id, attempts, error,
                max_retries, retry_delay_ms,
            )

    @staticmethod
    def _fail_job_in_transaction(
        connection: Any,
        job_id: int,
        worker_id: str,
        attempts: int,
        error: str,
        max_retries: int,
        retry_delay_ms: int,
    ) -> JobState:
        now_ms = _now_ms()
        row = connection.execute(
            "SELECT attempts FROM durable_jobs WHERE job_id = ? AND state = 'processing' "
            "AND lease_owner = ? AND attempts = ? AND lease_expires_ms > ?",
            (job_id, worker_id, attempts, now_ms),
        ).fetchone()
        if row is None:
            raise LeaseLostError(f"lost lease for durable job {job_id}")
        next_state = JobState.DEAD if row["attempts"] >= max_retries else JobState.PENDING
        cursor = connection.execute(
            """UPDATE durable_jobs SET state = ?, lease_owner = NULL, lease_expires_ms = NULL,
                available_at_ms = ?, last_error = ?, updated_ms = ?
                WHERE job_id = ? AND state = 'processing' AND lease_owner = ?
                AND attempts = ? AND lease_expires_ms > ?""",
            (next_state.value, now_ms + max(retry_delay_ms, 0), error, now_ms,
             job_id, worker_id, attempts, now_ms),
        )
        if cursor.rowcount != 1:
            raise LeaseLostError(f"lost lease for durable job {job_id}")
        return next_state

    def _append_review_in_transaction(
        self,
        connection: Any,
        event_id: str,
        event: Any,
        result: Any,
        result_path: str,
        now_ms: int,
    ) -> int:
        evidence_ids = tuple(getattr(result, "evidence_ids", ()))
        claims = [
            claim.model_dump(mode="json") if hasattr(claim, "model_dump") else claim
            for claim in getattr(result, "claims", ())
        ]
        rule_citations = [
            citation.model_dump(mode="json")
            if hasattr(citation, "model_dump")
            else citation
            for citation in getattr(result, "rule_citations", ())
        ]
        self._validate_evidence_references(connection, event_id, result, evidence_ids)
        revision = event["revision"] + 1
        policy_id = getattr(result, "policy_id", None)
        model_id = getattr(result, "model_id", None)
        review_id = uuid.uuid4().hex
        connection.execute(
            """
            INSERT INTO reviews (
                review_id, event_id, revision, policy_id, model_id, verdict,
                confidence, evidence_status, explanation, evidence_ids_json,
                claims_json, rule_citations_json, result_path, created_ms
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                review_id,
                event_id,
                revision,
                policy_id,
                model_id,
                result.verdict,
                result.confidence,
                result.evidence_status,
                result.explanation,
                json.dumps(evidence_ids, ensure_ascii=False),
                json.dumps(claims, ensure_ascii=False),
                json.dumps(rule_citations, ensure_ascii=False),
                result_path,
                now_ms,
            ),
        )
        connection.execute(
            """
            INSERT INTO review_results (
                event_id, verdict, confidence, evidence_status, explanation,
                evidence_ids_json, claims_json, policy_id, model_id, parsed_at_ms
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(event_id) DO UPDATE SET
                verdict = excluded.verdict,
                confidence = excluded.confidence,
                evidence_status = excluded.evidence_status,
                explanation = excluded.explanation,
                evidence_ids_json = excluded.evidence_ids_json,
                claims_json = excluded.claims_json,
                policy_id = excluded.policy_id,
                model_id = excluded.model_id,
                parsed_at_ms = excluded.parsed_at_ms
            """,
            (
                event_id,
                result.verdict,
                result.confidence,
                result.evidence_status,
                result.explanation,
                json.dumps(evidence_ids, ensure_ascii=False),
                json.dumps(claims, ensure_ascii=False),
                policy_id,
                model_id,
                now_ms,
            ),
        )
        connection.execute(
            """
            UPDATE events
            SET revision = ?, status = 'completed', verdict = ?, confidence = ?,
                result_path = ?
            WHERE event_id = ?
            """,
            (revision, result.verdict, result.confidence, result_path, event_id),
        )
        connection.execute(
            """
            INSERT INTO durable_jobs (
                kind, entity_id, entity_revision, state, attempts,
                available_at_ms, created_ms, updated_ms
            ) VALUES ('index', ?, ?, 'pending', 0, ?, ?, ?)
            ON CONFLICT(kind, entity_id, entity_revision) DO NOTHING
            """,
            (event_id, revision, now_ms, now_ms, now_ms),
        )
        connection.execute(
            """
            INSERT INTO durable_jobs (
                kind, entity_id, entity_revision, state, attempts,
                available_at_ms, created_ms, updated_ms
            ) VALUES ('report', ?, ?, 'pending', 0, ?, ?, ?)
            ON CONFLICT(kind, entity_id, entity_revision) DO NOTHING
            """,
            (event_id, revision, now_ms, now_ms, now_ms),
        )
        return revision

    @staticmethod
    def _validate_review_lease(
        row: Any,
        job: DurableJob,
        worker_id: str,
        now_ms: int,
    ) -> None:
        if row is None:
            raise LeaseLostError(f"lost lease for durable job {job.job_id}")
        if (
            row["kind"] != JobKind.REVIEW.value
            or row["entity_id"] != job.entity_id
            or row["entity_revision"] != job.entity_revision
            or row["state"] != JobState.PROCESSING.value
            or row["lease_owner"] != worker_id
            or row["attempts"] != job.attempts
            or row["lease_expires_ms"] is None
            or row["lease_expires_ms"] <= now_ms
        ):
            raise LeaseLostError(f"lost lease for durable job {job.job_id}")

    @staticmethod
    def _validate_extract_lease(row: Any, job: DurableJob, worker_id: str, now_ms: int) -> None:
        if row is None or (
            row["kind"] != JobKind.EVIDENCE_EXTRACT.value
            or row["entity_id"] != job.entity_id
            or row["entity_revision"] != job.entity_revision
            or row["state"] != JobState.PROCESSING.value
            or row["lease_owner"] != worker_id
            or row["attempts"] != job.attempts
            or row["lease_expires_ms"] is None
            or row["lease_expires_ms"] <= now_ms
        ):
            raise LeaseLostError(f"lost lease for durable job {job.job_id}")

    @staticmethod
    def _validate_report_lease(row: Any, job: DurableJob, worker_id: str, now_ms: int) -> None:
        if row is None or (
            row["kind"] != JobKind.REPORT.value
            or row["entity_id"] != job.entity_id
            or row["entity_revision"] != job.entity_revision
            or row["state"] != JobState.PROCESSING.value
            or row["lease_owner"] != worker_id
            or row["attempts"] != job.attempts
            or row["lease_expires_ms"] is None
            or row["lease_expires_ms"] <= now_ms
        ):
            raise LeaseLostError(f"lost lease for durable job {job.job_id}")
    @staticmethod
    def _complete_owned_job(
        connection: Any,
        job: DurableJob,
        worker_id: str,
    ) -> None:
        EventLedger._complete_owned_job_kind(connection, job, worker_id, JobKind.REVIEW)

    @staticmethod
    def _complete_owned_job_kind(
        connection: Any,
        job: DurableJob,
        worker_id: str,
        kind: JobKind,
    ) -> None:
        now_ms = _now_ms()
        cursor = connection.execute(
            """
            UPDATE durable_jobs
            SET state = 'completed', lease_owner = NULL, lease_expires_ms = NULL,
                updated_ms = ?
            WHERE job_id = ? AND kind = ? AND entity_id = ? AND entity_revision = ?
                AND state = 'processing' AND lease_owner = ? AND attempts = ?
                AND lease_expires_ms > ?
            """,
            (
                now_ms,
                job.job_id,
                kind.value,
                job.entity_id,
                job.entity_revision,
                worker_id,
                job.attempts,
                now_ms,
            ),
        )
        if cursor.rowcount != 1:
            raise LeaseLostError(f"lost lease for durable job {job.job_id}")

    @staticmethod
    def _validate_artifact_timing(artifact: "EvidenceArtifact") -> None:
        source_pts_start = artifact.source_pts_start
        source_pts_end = artifact.source_pts_end
        stream_generation = artifact.stream_generation
        timing = (source_pts_start, source_pts_end, stream_generation)
        if all(value is None for value in timing):
            return
        if any(value is None for value in timing):
            raise ValueError("evidence artifact timing is incomplete")
        if (
            source_pts_start is None
            or source_pts_end is None
            or stream_generation is None
            or source_pts_start < 0
            or source_pts_end <= source_pts_start
            or stream_generation < 0
        ):
            raise ValueError("evidence artifact timing is invalid")

    def _artifact_metadata_matches(self, resolved: Path, artifact: "EvidenceArtifact") -> bool:
        try:
            info = resolved.lstat()
            if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
                return False
            expected_mime = "video/mp4" if artifact.kind == "clip" else "image/jpeg"
            if artifact.mime_type != expected_mime:
                return False
            if info.st_size != artifact.size or info.st_mtime != artifact.mtime:
                return False
            if hashlib.sha256(resolved.read_bytes()).hexdigest() != artifact.sha256:
                return False
        except OSError:
            return False
        return True

    def _record_detections(
        self,
        connection: Any,
        event_id: str,
        context: ReviewContext,
    ) -> None:
        rows = [
            (
                event_id,
                detection.class_name,
                detection.class_id,
                detection.confidence,
                json.dumps(detection.bbox, ensure_ascii=False),
                detection.track_id,
                str(detection.track_state) if detection.track_state is not None else None,
                int(detection.occluded),
            )
            for detection in context.detections
        ]
        if rows:
            connection.executemany(
                """
                INSERT OR IGNORE INTO detections (
                    event_id, class_name, class_id, confidence, bbox_json,
                    track_id, track_state, occluded
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                """,
                rows,
            )

    @staticmethod
    def _validate_evidence_references(
        connection: Any,
        event_id: str,
        result: Any,
        evidence_ids: tuple[str, ...],
    ) -> None:
        known = {
            row["evidence_id"]
            for row in connection.execute(
                "SELECT evidence_id FROM evidence WHERE event_id = ? AND available = 1",
                (event_id,),
            )
        }
        claim_evidence_ids = {
            evidence_id
            for claim in getattr(result, "claims", ())
            for evidence_id in getattr(claim, "evidence_ids", ())
        }
        if any(evidence_id not in known for evidence_id in (*evidence_ids, *claim_evidence_ids)):
            raise ValueError("review references unavailable evidence")
        if result.verdict != "uncertain" and (
            result.evidence_status != "available" or not evidence_ids
        ):
            raise ValueError("non-uncertain review requires available evidence")

    def _refresh_evidence_in_transaction(
        self,
        connection: Any,
        event_id: str,
        evidence_id: str | None = None,
    ) -> int:
        query = "SELECT id, path FROM evidence WHERE event_id = ?"
        params: list[str] = [event_id]
        if evidence_id is not None:
            query += " AND evidence_id = ?"
            params.append(evidence_id)
        rows = connection.execute(query, params).fetchall()
        for row in rows:
            available, size, mtime = self._evidence_metadata(row["path"])
            connection.execute(
                """
                UPDATE evidence SET available = ?, size = ?, mtime = ?
                WHERE id = ?
                """,
                (int(available), size, mtime, row["id"]),
            )
        return len(rows)

    def _evidence_metadata(self, path: str) -> tuple[bool, int | None, float | None]:
        resolved = self.resolve_evidence_path(path)
        if resolved is None:
            return False, None, None
        try:
            metadata = resolved.stat()
        except OSError:
            return False, None, None
        if not stat.S_ISREG(metadata.st_mode):
            return False, None, None
        return True, metadata.st_size, metadata.st_mtime

    def _record_evidence(
        self,
        connection: Any,
        event_id: str,
        context: ReviewContext,
    ) -> None:
        for kind, path in (("frame", context.frame_path), ("clip", context.clip_path)):
            if not path:
                continue
            resolved = self.resolve_evidence_path(path)
            if resolved is None:
                continue
            canonical_path = str(resolved)
            available, size, mtime = self._evidence_metadata(canonical_path)
            connection.execute(
                """
                INSERT OR IGNORE INTO evidence (
                    event_id, evidence_id, kind, path, mime_type, available, size, mtime,
                    source_pts_start, source_pts_end, stream_generation
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    event_id,
                    _evidence_id(event_id, kind, canonical_path),
                    kind,
                    canonical_path,
                    mimetypes.guess_type(canonical_path)[0],
                    int(available),
                    size,
                    mtime,
                    # Ingress paths are point-in-time references, not exact
                    # intervals. The event row owns the source timing anchor;
                    # derived artifacts provide their own interval metadata.
                    None,
                    None,
                    None,
                ),
            )

    def jobs_for_event(self, event_id: str) -> tuple[DurableJob, ...]:
        """返回事件实体的持久任务，保持创建顺序。"""
        rows = self._store.get_jobs_for_entity(event_id)
        return tuple(self._job_from_row(row) for row in rows)

    @staticmethod
    def _report_record_from_row(row: Any) -> ReportRecord:
        return ReportRecord(
            report_id=row["report_id"],
            event_id=row["event_id"],
            review_id=row["review_id"],
            review_revision=row["review_revision"],
            template_version=row["template_version"],
            sha256=row["sha256"],
            artifact_path=row["artifact_path"],
            size_bytes=row["size_bytes"],
            created_ms=row["created_ms"],
        )

    @staticmethod
    def _job_from_row(row: Any) -> DurableJob:
        return DurableJob(
            job_id=row["job_id"],
            kind=JobKind(row["kind"]),
            entity_id=row["entity_id"],
            entity_revision=row["entity_revision"],
            state=JobState(row["state"]),
            attempts=row["attempts"],
            lease_owner=row["lease_owner"],
            lease_expires_ms=row["lease_expires_ms"],
            available_at_ms=row["available_at_ms"],
            last_error=row["last_error"],
        )
